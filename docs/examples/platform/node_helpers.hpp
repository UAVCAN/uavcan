/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef EXAMPLE_PLATFORM_NODE_HELPERS_HPP_INCLUDED
#define EXAMPLE_PLATFORM_NODE_HELPERS_HPP_INCLUDED

#include <cetl/pf17/cetlpf.hpp>
#include <libcyphal/executor.hpp>
#include <libcyphal/presentation/presentation.hpp>
#include <libcyphal/transport/msg_sessions.hpp>
#include <libcyphal/transport/scattered_buffer.hpp>
#include <libcyphal/transport/svc_sessions.hpp>
#include <libcyphal/transport/transport.hpp>
#include <libcyphal/transport/types.hpp>
#include <libcyphal/types.hpp>

#include <cassert>  // NOLINT for NUNAVUT_ASSERT
#include <nunavut/support/serialization.hpp>
#include <uavcan/node/GetInfo_1_0.hpp>
#include <uavcan/node/Health_1_0.hpp>
#include <uavcan/node/Heartbeat_1_0.hpp>
#include <uavcan/node/Mode_1_0.hpp>
#include <uavcan/node/Version_1_0.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

using std::literals::chrono_literals::operator""s;

namespace example
{
namespace platform
{

struct NodeHelpers
{
    using Callback = libcyphal::IExecutor::Callback;

    using ServiceRxMetadata    = libcyphal::transport::ServiceRxMetadata;
    using ServiceTxMetadata    = libcyphal::transport::ServiceTxMetadata;
    using TransferTxMetadata   = libcyphal::transport::TransferTxMetadata;
    using MessageRxSessionPtr  = libcyphal::UniquePtr<libcyphal::transport::IMessageRxSession>;
    using MessageTxSessionPtr  = libcyphal::UniquePtr<libcyphal::transport::IMessageTxSession>;
    using RequestRxSessionPtr  = libcyphal::UniquePtr<libcyphal::transport::IRequestRxSession>;
    using ResponseTxSessionPtr = libcyphal::UniquePtr<libcyphal::transport::IResponseTxSession>;

    using Presentation = libcyphal::presentation::Presentation;
    template <typename T>
    using Publisher = libcyphal::presentation::Publisher<T>;

    template <typename T, typename TxSession, typename TxMetadata>
    static cetl::optional<libcyphal::transport::AnyFailure> serializeAndSend(const T&          value,
                                                                             TxSession&        tx_session,
                                                                             const TxMetadata& metadata)
    {
        using traits = typename T::_traits_;
        std::array<std::uint8_t, traits::SerializationBufferSizeBytes> buffer;

        const auto data_size = serialize(value, buffer).value();

        // NOLINTNEXTLINE
        const cetl::span<const cetl::byte> fragment{reinterpret_cast<cetl::byte*>(buffer.data()), data_size};
        const std::array<const cetl::span<const cetl::byte>, 1> payload{fragment};

        return tx_session.send(metadata, payload);
    }

    template <typename T>
    static bool tryDeserialize(T& obj, const libcyphal::transport::ScatteredBuffer& buffer)
    {
        std::vector<std::uint8_t>       data_vec(buffer.size());
        const auto                      data_size = buffer.copy(0, data_vec.data(), data_vec.size());
        nunavut::support::const_bitspan bitspan{data_vec.data(), data_size};

        return deserialize(obj, bitspan);
    }

    struct Heartbeat
    {
        using Message = uavcan::node::Heartbeat_1_0;

        bool makeRxSession(libcyphal::transport::ITransport& transport, const libcyphal::TimePoint startup_time)
        {
            auto maybe_msg_rx_session =
                transport.makeMessageRxSession({Message::_traits_::ExtentBytes, Message::_traits_::FixedPortId});
            EXPECT_THAT(maybe_msg_rx_session, testing::VariantWith<MessageRxSessionPtr>(testing::_))
                << "Failed to create Heartbeat RX session.";
            if (auto* const session = cetl::get_if<MessageRxSessionPtr>(&maybe_msg_rx_session))
            {
                startup_time_   = startup_time;
                msg_rx_session_ = std::move(*session);
            }
            return nullptr != msg_rx_session_;
        }

        void makeTxSession(libcyphal::transport::ITransport& transport,
                           libcyphal::IExecutor&             executor,
                           const libcyphal::TimePoint        startup_time)
        {
            auto maybe_msg_tx_session = transport.makeMessageTxSession({Message::_traits_::FixedPortId});
            EXPECT_THAT(maybe_msg_tx_session, testing::VariantWith<MessageTxSessionPtr>(testing::_))
                << "Failed to create Heartbeat TX session.";
            if (auto* const session = cetl::get_if<MessageTxSessionPtr>(&maybe_msg_tx_session))
            {
                startup_time_   = startup_time;
                msg_tx_session_ = std::move(*session);

                publish_every_1s_cb_  = executor.registerCallback([&](const auto& arg) { publish(arg.approx_now); });
                constexpr auto period = std::chrono::seconds{Message::MAX_PUBLICATION_PERIOD};
                publish_every_1s_cb_.schedule(Callback::Schedule::Repeat{startup_time + period, period});
            }
        }

        static cetl::optional<Publisher<Message>> makePublisher(Presentation& presentation)
        {
            auto maybe_publisher = presentation.makePublisher<Message>(Message::_traits_::FixedPortId);
            EXPECT_THAT(maybe_publisher, testing::VariantWith<Publisher<Message>>(testing::_))
                << "Failed to create Heartbeat publisher.";
            if (auto* const publisher = cetl::get_if<Publisher<Message>>(&maybe_publisher))
            {
                return std::move(*publisher);
            }
            return cetl::nullopt;
        }

        void receive(const libcyphal::TimePoint now) const
        {
            if (msg_rx_session_)
            {
                if (const auto rx_heartbeat = msg_rx_session_->receive())
                {
                    print(now, *rx_heartbeat);
                }
            }
        }

    private:
        void publish(const libcyphal::TimePoint now)
        {
            transfer_id_ += 1;

            const auto uptime_in_secs = std::chrono::duration_cast<std::chrono::seconds>(now - startup_time_);

            const Message            heartbeat{static_cast<std::uint32_t>(uptime_in_secs.count()),
                                               {uavcan::node::Health_1_0::NOMINAL},
                                               {uavcan::node::Mode_1_0::OPERATIONAL}};
            const TransferTxMetadata metadata{{transfer_id_, libcyphal::transport::Priority::Nominal}, now + 1s};

            EXPECT_THAT(serializeAndSend(heartbeat, *msg_tx_session_, metadata), testing::Eq(cetl::nullopt))
                << "Failed to publish Heartbeat_1_0.";
        }

        void print(const libcyphal::TimePoint now, const libcyphal::transport::MessageRxTransfer& rx_heartbeat) const
        {
            uavcan::node::Heartbeat_1_0 heartbeat{};
            if (tryDeserialize(heartbeat, rx_heartbeat.payload))
            {
                const auto rel_time = now - startup_time_;
                std::cout << "Received heartbeat from Node " << std::setw(5)
                          << rx_heartbeat.metadata.publisher_node_id.value_or(0) << ", Uptime " << std::setw(8)
                          << heartbeat.uptime << "   @ " << std::setw(8)
                          << std::chrono::duration_cast<std::chrono::milliseconds>(rel_time).count()
                          << " ms, tx_id=" << std::setw(8) << rx_heartbeat.metadata.rx_meta.base.transfer_id << "\n"
                          << std::flush;
            }
        }

        libcyphal::TimePoint             startup_time_;
        MessageRxSessionPtr              msg_rx_session_;
        libcyphal::transport::TransferId transfer_id_{0};
        MessageTxSessionPtr              msg_tx_session_;
        Callback::Any                    publish_every_1s_cb_;

    };  // Heartbeat

    struct GetInfo
    {
        using Request  = uavcan::node::GetInfo::Request_1_0;
        using Response = uavcan::node::GetInfo::Response_1_0;

        void setName(const std::string& name)
        {
            response.name.clear();
            std::copy_n(name.begin(), std::min(name.size(), 50UL), std::back_inserter(response.name));
        }

        bool makeRxSession(libcyphal::transport::ITransport& transport)
        {
            auto maybe_svc_rx_session =
                transport.makeRequestRxSession({Request::_traits_::ExtentBytes, Request::_traits_::FixedPortId});
            EXPECT_THAT(maybe_svc_rx_session, testing::VariantWith<RequestRxSessionPtr>(testing::_))
                << "Failed to create GetInfo request RX session.";
            if (auto* const session = cetl::get_if<RequestRxSessionPtr>(&maybe_svc_rx_session))
            {
                svc_req_rx_session_ = std::move(*session);
            }
            return nullptr != svc_req_rx_session_;
        }
        void makeTxSession(libcyphal::transport::ITransport& transport)
        {
            auto maybe_svc_tx_session = transport.makeResponseTxSession({Response::_traits_::FixedPortId});
            EXPECT_THAT(maybe_svc_tx_session, testing::VariantWith<ResponseTxSessionPtr>(testing::_))
                << "Failed to create GetInfo response TX session.";
            if (auto* const session = cetl::get_if<ResponseTxSessionPtr>(&maybe_svc_tx_session))
            {
                svc_res_tx_session_ = std::move(*session);
            }
        }

        void receive(const libcyphal::TimePoint now) const
        {
            if (svc_req_rx_session_ && svc_res_tx_session_)
            {
                if (const auto request = svc_req_rx_session_->receive())
                {
                    const ServiceTxMetadata metadata{{{request->metadata.rx_meta.base.transfer_id,
                                                       request->metadata.rx_meta.base.priority},
                                                      now + 1s},
                                                     request->metadata.remote_node_id};

                    EXPECT_THAT(serializeAndSend(response, *svc_res_tx_session_, metadata), testing::Eq(cetl::nullopt))
                        << "Failed to send GetInfo::Response_1_0.";
                }
            }
        }

    private:
        RequestRxSessionPtr  svc_req_rx_session_;
        ResponseTxSessionPtr svc_res_tx_session_;
        Response             response{{1, 0}};

    };  // GetInfo

};  // NodeHelpers

}  // namespace platform
}  // namespace example

#endif  // EXAMPLE_PLATFORM_NODE_HELPERS_HPP_INCLUDED