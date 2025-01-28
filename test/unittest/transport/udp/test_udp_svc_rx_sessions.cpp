/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#include "media_mock.hpp"
#include "memory_resource_mock.hpp"
#include "tracking_memory_resource.hpp"
#include "transient_error_handler_mock.hpp"
#include "tx_rx_sockets_mock.hpp"
#include "udp_gtest_helpers.hpp"
#include "verification_utilities.hpp"
#include "virtual_time_scheduler.hpp"

#include <cetl/pf17/cetlpf.hpp>
#include <libcyphal/errors.hpp>
#include <libcyphal/executor.hpp>
#include <libcyphal/transport/errors.hpp>
#include <libcyphal/transport/svc_sessions.hpp>
#include <libcyphal/transport/types.hpp>
#include <libcyphal/transport/udp/media.hpp>
#include <libcyphal/transport/udp/svc_rx_sessions.hpp>
#include <libcyphal/transport/udp/tx_rx_sockets.hpp>
#include <libcyphal/transport/udp/udp_transport.hpp>
#include <libcyphal/transport/udp/udp_transport_impl.hpp>
#include <libcyphal/types.hpp>
#include <udpard.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace
{

using libcyphal::TimePoint;
using libcyphal::UniquePtr;
using libcyphal::MemoryError;
using namespace libcyphal::transport;       // NOLINT This our main concern here in the unit tests.
using namespace libcyphal::transport::udp;  // NOLINT This our main concern here in the unit tests.

using cetl::byte;
using libcyphal::verification_utilities::b;

using testing::_;
using testing::Eq;
using testing::Ref;
using testing::Truly;
using testing::Invoke;
using testing::Return;
using testing::IsEmpty;
using testing::NotNull;
using testing::Optional;
using testing::ReturnRef;
using testing::StrictMock;
using testing::ElementsAre;
using testing::VariantWith;

// https://github.com/llvm/llvm-project/issues/53444
// NOLINTBEGIN(misc-unused-using-decls, misc-include-cleaner)
using std::literals::chrono_literals::operator""s;
using std::literals::chrono_literals::operator""ms;
using std::literals::chrono_literals::operator""us;
// NOLINTEND(misc-unused-using-decls, misc-include-cleaner)

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

class TestUdpSvcRxSessions : public testing::Test
{
protected:
    void SetUp() override
    {
        cetl::pmr::set_default_resource(&mr_);

        EXPECT_CALL(media_mock_, makeTxSocket())  //
            .WillRepeatedly(Invoke([this] {
                return libcyphal::detail::makeUniquePtr<TxSocketMock::RefWrapper::Spec>(mr_, tx_socket_mock_);
            }));
        EXPECT_CALL(media_mock_, makeRxSocket(_))  //
            .WillRepeatedly(Invoke([this](auto& endpoint) {
                rx_socket_mock_.setEndpoint(endpoint);
                return libcyphal::detail::makeUniquePtr<RxSocketMock::RefWrapper::Spec>(mr_, rx_socket_mock_);
            }));
        EXPECT_CALL(media_mock_, getTxMemoryResource()).WillRepeatedly(ReturnRef(mr_));
    }

    void TearDown() override
    {
        EXPECT_THAT(mr_.allocations, IsEmpty());
        EXPECT_THAT(mr_.total_allocated_bytes, mr_.total_deallocated_bytes);

        EXPECT_THAT(tx_mr_.allocations, IsEmpty());
        EXPECT_THAT(tx_mr_.total_allocated_bytes, tx_mr_.total_deallocated_bytes);

        EXPECT_THAT(payload_mr_.allocations, IsEmpty());
        EXPECT_THAT(payload_mr_.total_allocated_bytes, payload_mr_.total_deallocated_bytes);
    }

    TimePoint now() const
    {
        return scheduler_.now();
    }

    UniquePtr<IUdpTransport> makeTransport(const MemoryResourcesSpec&   mem_res_spec,
                                           const cetl::optional<NodeId> local_node_id = cetl::nullopt)
    {
        std::array<IMedia*, 1> media_array{&media_mock_};

        auto maybe_transport = udp::makeTransport(mem_res_spec, scheduler_, media_array, 0);
        EXPECT_THAT(maybe_transport, VariantWith<UniquePtr<IUdpTransport>>(NotNull()));
        auto transport = cetl::get<UniquePtr<IUdpTransport>>(std::move(maybe_transport));

        if (local_node_id)
        {
            transport->setLocalNodeId(*local_node_id);
        }

        return transport;
    }

    // MARK: Data members:

    // NOLINTBEGIN
    libcyphal::VirtualTimeScheduler scheduler_{};
    TrackingMemoryResource          mr_;
    TrackingMemoryResource          tx_mr_;
    TrackingMemoryResource          payload_mr_;
    StrictMock<MediaMock>           media_mock_{};
    StrictMock<RxSocketMock>        rx_socket_mock_{"RxS1"};
    StrictMock<TxSocketMock>        tx_socket_mock_{"TxS1"};
    // NOLINTEND
};

// MARK: - Tests:

TEST_F(TestUdpSvcRxSessions, make_request_setTransferIdTimeout)
{
    auto transport = makeTransport({mr_});

    auto maybe_session = transport->makeRequestRxSession({42, 123});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    EXPECT_THAT(session->getParams().extent_bytes, 42);
    EXPECT_THAT(session->getParams().service_id, 123);

    // NOLINTNEXTLINE
    const auto& rpc_port = static_cast<udp::detail::SvcRequestRxSession*>(session.get())->asRpcPort();
    EXPECT_THAT(rpc_port.port.transfer_id_timeout_usec, UDPARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);

    session->setTransferIdTimeout(-1ms);  // negative value is not allowed (rejected)
    EXPECT_THAT(rpc_port.port.transfer_id_timeout_usec, UDPARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);

    session->setTransferIdTimeout(0s);
    EXPECT_THAT(rpc_port.port.transfer_id_timeout_usec, 0);

    session->setTransferIdTimeout(500ms);
    EXPECT_THAT(rpc_port.port.transfer_id_timeout_usec, 500'000);
}

TEST_F(TestUdpSvcRxSessions, make_response_no_memory)
{
    StrictMock<MemoryResourceMock> mr_mock;
    mr_mock.redirectExpectedCallsTo(mr_);

    // Emulate that there is no memory available for the message session.
    EXPECT_CALL(mr_mock, do_allocate(sizeof(udp::detail::SvcResponseRxSession), _))  //
        .WillOnce(Return(nullptr));

    const auto transport = makeTransport({mr_mock});

    const auto maybe_session = transport->makeResponseRxSession({64, 0x23, 0x45});
    EXPECT_THAT(maybe_session, VariantWith<AnyFailure>(VariantWith<MemoryError>(_)));
}

TEST_F(TestUdpSvcRxSessions, make_response_fails_due_to_rx_socket_error)
{
    using MediaReport = IUdpTransport::TransientErrorReport::MediaMakeRxSocket;

    auto transport = makeTransport({mr_}, NodeId{0x13});

    // Emulate that RX socket creation fails due to a memory error.
    {
        EXPECT_CALL(media_mock_, makeRxSocket(_))  //
            .WillOnce(Return(MemoryError{}));

        auto maybe_session = transport->makeResponseRxSession({64, 0x23, 0x45});
        EXPECT_THAT(maybe_session, VariantWith<AnyFailure>(VariantWith<MemoryError>(_)));
    }

    // Try again but with error handler.
    {
        EXPECT_CALL(media_mock_, makeRxSocket(_))  //
            .WillOnce(Return(MemoryError{}));

        StrictMock<TransientErrorHandlerMock> handler_mock;
        transport->setTransientErrorHandler(std::ref(handler_mock));
        EXPECT_CALL(handler_mock, invoke(VariantWith<MediaReport>(Truly([&](auto& report) {
                        EXPECT_THAT(report.failure, VariantWith<MemoryError>(_));
                        EXPECT_THAT(report.media_index, 0);
                        EXPECT_THAT(report.culprit, Ref(media_mock_));
                        return true;
                    }))))
            .WillOnce(Return(CapacityError{}));

        auto maybe_session = transport->makeResponseRxSession({64, 0x23, 0x45});
        EXPECT_THAT(maybe_session, VariantWith<AnyFailure>(VariantWith<CapacityError>(_)));
    }
}

TEST_F(TestUdpSvcRxSessions, make_request_no_memory)
{
    StrictMock<MemoryResourceMock> mr_mock;
    mr_mock.redirectExpectedCallsTo(mr_);

    // Emulate that there is no memory available for the message session.
    EXPECT_CALL(mr_mock, do_allocate(sizeof(udp::detail::SvcRequestRxSession), _))  //
        .WillOnce(Return(nullptr));

    const auto transport = makeTransport({mr_mock});

    const auto maybe_session = transport->makeRequestRxSession({64, 0x7B});
    EXPECT_THAT(maybe_session, VariantWith<AnyFailure>(VariantWith<MemoryError>(_)));
}

TEST_F(TestUdpSvcRxSessions, make_request_fails_due_to_argument_error)
{
    const auto transport = makeTransport({mr_});

    // Try invalid subject id
    const auto maybe_session = transport->makeRequestRxSession({64, UDPARD_SERVICE_ID_MAX + 1});
    EXPECT_THAT(maybe_session, VariantWith<AnyFailure>(VariantWith<libcyphal::ArgumentError>(_)));
}

TEST_F(TestUdpSvcRxSessions, make_request_fails_due_to_rx_socket_error)
{
    using MediaReport = IUdpTransport::TransientErrorReport::MediaMakeRxSocket;

    auto transport = makeTransport({mr_}, NodeId{0x31});

    // Emulate that RX socket creation fails due to a memory error.
    {
        EXPECT_CALL(media_mock_, makeRxSocket(_))  //
            .WillOnce(Return(MemoryError{}));

        auto maybe_session = transport->makeRequestRxSession({64, 0x7B});
        EXPECT_THAT(maybe_session, VariantWith<AnyFailure>(VariantWith<MemoryError>(_)));
    }

    // Try again but with error handler.
    {
        EXPECT_CALL(media_mock_, makeRxSocket(_))  //
            .WillOnce(Return(MemoryError{}));

        StrictMock<TransientErrorHandlerMock> handler_mock;
        transport->setTransientErrorHandler(std::ref(handler_mock));
        EXPECT_CALL(handler_mock, invoke(VariantWith<MediaReport>(Truly([&](auto& report) {
                        EXPECT_THAT(report.failure, VariantWith<MemoryError>(_));
                        EXPECT_THAT(report.media_index, 0);
                        EXPECT_THAT(report.culprit, Ref(media_mock_));
                        return true;
                    }))))
            .WillOnce(Return(CapacityError{}));

        auto maybe_session = transport->makeRequestRxSession({64, 0x7B});
        EXPECT_THAT(maybe_session, VariantWith<AnyFailure>(VariantWith<CapacityError>(_)));
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(TestUdpSvcRxSessions, receive_request)
{
    StrictMock<MemoryResourceMock> payload_mr_mock;

    auto transport = makeTransport({mr_, nullptr, nullptr, &payload_mr_mock}, NodeId{0x31});

    EXPECT_THAT(rx_socket_mock_.getEndpoint().ip_address, 0);
    EXPECT_THAT(rx_socket_mock_.getEndpoint().udp_port, 0);

    EXPECT_CALL(rx_socket_mock_, registerCallback(_))  //
        .WillOnce(Invoke([&](auto function) {          //
            return scheduler_.registerNamedCallback("rx_socket", std::move(function));
        }));

    constexpr std::size_t extent_bytes  = 8;
    auto                  maybe_session = transport->makeRequestRxSession({extent_bytes, 0x17B});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    EXPECT_THAT(rx_socket_mock_.getEndpoint().ip_address, 0xEF010031);
    EXPECT_THAT(rx_socket_mock_.getEndpoint().udp_port, 9382);

    const auto params = session->getParams();
    EXPECT_THAT(params.extent_bytes, extent_bytes);
    EXPECT_THAT(params.service_id, 0x17B);

    constexpr auto timeout = 200ms;
    session->setTransferIdTimeout(timeout);

    TimePoint rx_timestamp;

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        SCOPED_TRACE("1-st iteration: one frame available @ 1s");

        constexpr std::size_t payload_size = 2;
        constexpr std::size_t frame_size   = UdpardFrame::SizeOfHeaderAndTxCrc + payload_size;

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(rx_socket_mock_, receive())  //
            .WillOnce([&]() -> IRxSocket::ReceiveResult::Metadata {
                EXPECT_THAT(now(), rx_timestamp);
                auto frame         = UdpardFrame(0x13, 0x31, 0x1D, payload_size, &payload_mr_mock, Priority::High);
                frame.payload()[0] = b(42);
                frame.payload()[1] = b(147);
                frame.setPortId(0x17B, true /*is_service*/, true /*is_request*/);
                std::uint32_t tx_crc = UdpardFrame::InitialTxCrc;
                return {rx_timestamp, std::move(frame).release(tx_crc)};
            });
        EXPECT_CALL(payload_mr_mock, do_allocate(frame_size, alignof(std::max_align_t)))
            .WillOnce([this](const std::size_t size_bytes, const std::size_t alignment) -> void* {
                return payload_mr_.allocate(size_bytes, alignment);
            });
        scheduler_.scheduleNamedCallback("rx_socket", rx_timestamp);

        scheduler_.scheduleAt(rx_timestamp + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session->receive();
            ASSERT_THAT(maybe_rx_transfer, Optional(_));
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            const auto& rx_transfer = maybe_rx_transfer.value();

            EXPECT_THAT(rx_transfer.metadata.rx_meta.timestamp, rx_timestamp);
            EXPECT_THAT(rx_transfer.metadata.rx_meta.base.transfer_id, 0x1D);
            EXPECT_THAT(rx_transfer.metadata.rx_meta.base.priority, Priority::High);
            EXPECT_THAT(rx_transfer.metadata.remote_node_id, 0x13);

            std::array<std::uint8_t, 2> buffer{};
            ASSERT_THAT(rx_transfer.payload.size(), payload_size);
            EXPECT_THAT(rx_transfer.payload.copy(0, buffer.data(), buffer.size()), payload_size);
            EXPECT_THAT(buffer, ElementsAre(42, 147));

            EXPECT_CALL(payload_mr_mock, do_deallocate(_, frame_size, alignof(std::max_align_t)))
                .WillOnce([this](void* const p, const std::size_t size_bytes, const std::size_t alignment) {
                    payload_mr_.deallocate(p, size_bytes, alignment);
                });
        });
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        SCOPED_TRACE("2-nd iteration: invalid null frame available @ 2s");

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(rx_socket_mock_, receive())  //
            .WillOnce([&]() -> IRxSocket::ReceiveResult::Metadata {
                return {rx_timestamp, {nullptr, libcyphal::PmrRawBytesDeleter{0, &payload_mr_mock}}};
            });
        scheduler_.scheduleNamedCallback("rx_socket", rx_timestamp);

        scheduler_.scheduleAt(rx_timestamp + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session->receive();
            EXPECT_THAT(maybe_rx_transfer, Eq(cetl::nullopt));
        });
    });
    scheduler_.scheduleAt(3s, [&](const auto&) {
        //
        SCOPED_TRACE("3-rd iteration: malformed frame available @ 3s - no error, just drop");

        constexpr std::size_t payload_size = 0;
        constexpr std::size_t frame_size   = UdpardFrame::SizeOfHeaderAndTxCrc + payload_size;

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(rx_socket_mock_, receive())  //
            .WillOnce([&]() -> IRxSocket::ReceiveResult::Metadata {
                EXPECT_THAT(now(), rx_timestamp);
                auto frame = UdpardFrame(0x13, 0x31, 0x1D, payload_size, &payload_mr_mock, Priority::High);
                frame.setPortId(0x17B, true /*is_service*/, false /*is_request*/);  // Malformed b/c it's response.
                std::uint32_t tx_crc = UdpardFrame::InitialTxCrc;
                return {rx_timestamp, std::move(frame).release(tx_crc)};
            });
        EXPECT_CALL(payload_mr_mock, do_allocate(frame_size, alignof(std::max_align_t)))
            .WillOnce([this](const std::size_t size_bytes, const std::size_t alignment) -> void* {
                return payload_mr_.allocate(size_bytes, alignment);
            });
        EXPECT_CALL(payload_mr_mock, do_deallocate(_, frame_size, alignof(std::max_align_t)))
            .WillOnce([this](void* const p, const std::size_t size_bytes, const std::size_t alignment) {
                payload_mr_.deallocate(p, size_bytes, alignment);
            });
        scheduler_.scheduleNamedCallback("rx_socket", rx_timestamp);

        scheduler_.scheduleAt(rx_timestamp + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session->receive();
            EXPECT_THAT(maybe_rx_transfer, Eq(cetl::nullopt));
        });
    });
    scheduler_.scheduleAt(9s, [&](const auto&) {
        //
        session.reset();
        EXPECT_CALL(rx_socket_mock_, deinit());
        transport.reset();
        testing::Mock::VerifyAndClearExpectations(&rx_socket_mock_);
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestUdpSvcRxSessions, receive_request_via_callback)
{
    StrictMock<MemoryResourceMock> payload_mr_mock;

    auto transport = makeTransport({mr_, nullptr, nullptr, &payload_mr_mock}, NodeId{0x31});

    EXPECT_THAT(rx_socket_mock_.getEndpoint().ip_address, 0);
    EXPECT_THAT(rx_socket_mock_.getEndpoint().udp_port, 0);

    EXPECT_CALL(rx_socket_mock_, registerCallback(_))  //
        .WillOnce(Invoke([&](auto function) {          //
            return scheduler_.registerNamedCallback("rx_socket", std::move(function));
        }));

    auto maybe_session = transport->makeRequestRxSession({8, 0x17B});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    EXPECT_THAT(rx_socket_mock_.getEndpoint().ip_address, 0xEF010031);
    EXPECT_THAT(rx_socket_mock_.getEndpoint().udp_port, 9382);

    constexpr std::size_t payload_size = 2;
    constexpr std::size_t frame_size   = UdpardFrame::SizeOfHeaderAndTxCrc + payload_size;

    TimePoint rx_timestamp;

    session->setOnReceiveCallback([&](const IRequestRxSession::OnReceiveCallback::Arg& arg) {
        //
        EXPECT_THAT(arg.transfer.metadata.rx_meta.timestamp, rx_timestamp);
        EXPECT_THAT(arg.transfer.metadata.rx_meta.base.transfer_id, 0x1D);
        EXPECT_THAT(arg.transfer.metadata.rx_meta.base.priority, Priority::High);
        EXPECT_THAT(arg.transfer.metadata.remote_node_id, 0x13);

        std::array<std::uint8_t, 2> buffer{};
        ASSERT_THAT(arg.transfer.payload.size(), payload_size);
        EXPECT_THAT(arg.transfer.payload.copy(0, buffer.data(), buffer.size()), payload_size);
        EXPECT_THAT(buffer, ElementsAre(42, 147));

        EXPECT_CALL(payload_mr_mock, do_deallocate(_, frame_size, alignof(std::max_align_t)))
            .WillOnce([this](void* const p, const std::size_t size_bytes, const std::size_t alignment) {
                payload_mr_.deallocate(p, size_bytes, alignment);
            });
    });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        SCOPED_TRACE("1-st iteration: one frame available @ 1s");

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(rx_socket_mock_, receive())  //
            .WillOnce([&]() -> IRxSocket::ReceiveResult::Metadata {
                EXPECT_THAT(now(), rx_timestamp);
                auto frame         = UdpardFrame(0x13, 0x31, 0x1D, payload_size, &payload_mr_mock, Priority::High);
                frame.payload()[0] = b(42);
                frame.payload()[1] = b(147);
                frame.setPortId(0x17B, true /*is_service*/, true /*is_request*/);
                std::uint32_t tx_crc = UdpardFrame::InitialTxCrc;
                return {rx_timestamp, std::move(frame).release(tx_crc)};
            });
        EXPECT_CALL(payload_mr_mock, do_allocate(frame_size, alignof(std::max_align_t)))
            .WillOnce([this](const std::size_t size_bytes, const std::size_t alignment) -> void* {
                return payload_mr_.allocate(size_bytes, alignment);
            });
        scheduler_.scheduleNamedCallback("rx_socket", rx_timestamp);

        scheduler_.scheduleAt(rx_timestamp + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session->receive();
            EXPECT_THAT(maybe_rx_transfer, Eq(cetl::nullopt));  // b/c was "consumed" by the callback.
        });
    });
    scheduler_.scheduleAt(9s, [&](const auto&) {
        //
        session.reset();
        EXPECT_CALL(rx_socket_mock_, deinit());
        transport.reset();
        testing::Mock::VerifyAndClearExpectations(&rx_socket_mock_);
    });
    scheduler_.spinFor(10s);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(TestUdpSvcRxSessions, receive_response)
{
    StrictMock<MemoryResourceMock> payload_mr_mock;

    auto transport = makeTransport({mr_, nullptr, nullptr, &payload_mr_mock}, NodeId{0x13});

    EXPECT_THAT(rx_socket_mock_.getEndpoint().ip_address, 0);
    EXPECT_THAT(rx_socket_mock_.getEndpoint().udp_port, 0);

    EXPECT_CALL(rx_socket_mock_, registerCallback(_))  //
        .WillOnce(Invoke([&](auto function) {          //
            return scheduler_.registerNamedCallback("rx_socket", std::move(function));
        }));

    constexpr std::size_t extent_bytes  = 8;
    auto                  maybe_session_n31 = transport->makeResponseRxSession({extent_bytes, 0x17B, 0x31});
    ASSERT_THAT(maybe_session_n31, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));
    auto session_n31 = cetl::get<UniquePtr<IResponseRxSession>>(std::move(maybe_session_n31));

    EXPECT_THAT(rx_socket_mock_.getEndpoint().ip_address, 0xEF010013);
    EXPECT_THAT(rx_socket_mock_.getEndpoint().udp_port, 9382);

    const auto params = session_n31->getParams();
    EXPECT_THAT(params.extent_bytes, extent_bytes);
    EXPECT_THAT(params.service_id, 0x17B);
    EXPECT_THAT(params.server_node_id, 0x31);

    constexpr auto timeout = 200ms;
    session_n31->setTransferIdTimeout(timeout);

    // Create another session with the same port ID but different server node ID (0x32).
    //
    auto                  maybe_session_n32 = transport->makeResponseRxSession({extent_bytes, 0x17B, 0x32});
    ASSERT_THAT(maybe_session_n32, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));
    auto session_n32 = cetl::get<UniquePtr<IResponseRxSession>>(std::move(maybe_session_n32));

    TimePoint rx_timestamp;

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        SCOPED_TRACE("1-st iteration: one frame available @ 1s");

        constexpr std::size_t payload_size = 2;
        constexpr std::size_t frame_size   = UdpardFrame::SizeOfHeaderAndTxCrc + payload_size;

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(rx_socket_mock_, receive())  //
            .WillOnce([&]() -> IRxSocket::ReceiveResult::Metadata {
                EXPECT_THAT(now(), rx_timestamp);
                auto frame         = UdpardFrame(0x31, 0x13, 0x1D, payload_size, &payload_mr_mock, Priority::High);
                frame.payload()[0] = b(42);
                frame.payload()[1] = b(147);
                frame.setPortId(0x17B, true /*is_service*/, false /*is_request*/);
                std::uint32_t tx_crc = UdpardFrame::InitialTxCrc;
                return {rx_timestamp, std::move(frame).release(tx_crc)};
            });
        EXPECT_CALL(payload_mr_mock, do_allocate(frame_size, alignof(std::max_align_t)))
            .WillOnce([this](const std::size_t size_bytes, const std::size_t alignment) -> void* {
                return payload_mr_.allocate(size_bytes, alignment);
            });
        scheduler_.scheduleNamedCallback("rx_socket", rx_timestamp);

        scheduler_.scheduleAt(rx_timestamp + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session_n31->receive();
            ASSERT_THAT(maybe_rx_transfer, Optional(_));
            EXPECT_THAT(session_n32->receive(), Eq(cetl::nullopt));  // Different server node ID.
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            const auto& rx_transfer = maybe_rx_transfer.value();

            EXPECT_THAT(rx_transfer.metadata.rx_meta.timestamp, rx_timestamp);
            EXPECT_THAT(rx_transfer.metadata.rx_meta.base.transfer_id, 0x1D);
            EXPECT_THAT(rx_transfer.metadata.rx_meta.base.priority, Priority::High);
            EXPECT_THAT(rx_transfer.metadata.remote_node_id, 0x31);

            std::array<std::uint8_t, 2> buffer{};
            ASSERT_THAT(rx_transfer.payload.size(), payload_size);
            EXPECT_THAT(rx_transfer.payload.copy(0, buffer.data(), buffer.size()), payload_size);
            EXPECT_THAT(buffer, ElementsAre(42, 147));

            EXPECT_CALL(payload_mr_mock, do_deallocate(_, frame_size, alignof(std::max_align_t)))
                .WillOnce([this](void* const p, const std::size_t size_bytes, const std::size_t alignment) {
                    payload_mr_.deallocate(p, size_bytes, alignment);
                });
        });
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        SCOPED_TRACE("2-nd iteration: media RX socket error @ 2s");

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(rx_socket_mock_, receive())  //
            .WillOnce([&, rx_timestamp] {
                EXPECT_THAT(now(), rx_timestamp);
                return libcyphal::ArgumentError{};
            });
        scheduler_.scheduleNamedCallback("rx_socket", rx_timestamp);

        scheduler_.scheduleAt(rx_timestamp + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session_n31->receive();
            EXPECT_THAT(maybe_rx_transfer, Eq(cetl::nullopt));
        });
    });
    scheduler_.scheduleAt(3s, [&](const auto&) {
        //
        SCOPED_TRACE("3-rd iteration: unsolicited (node 0x33) response @ 3s");

        constexpr std::size_t payload_size = 2;
        constexpr std::size_t frame_size   = UdpardFrame::SizeOfHeaderAndTxCrc + payload_size;

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(rx_socket_mock_, receive())  //
            .WillOnce([&]() -> IRxSocket::ReceiveResult::Metadata {
                EXPECT_THAT(now(), rx_timestamp);
                auto frame         = UdpardFrame(0x33, 0x13, 0x1D, payload_size, &payload_mr_mock, Priority::High);
                frame.payload()[0] = b(42);
                frame.payload()[1] = b(147);
                frame.setPortId(0x17B, true /*is_service*/, false /*is_request*/);
                std::uint32_t tx_crc = UdpardFrame::InitialTxCrc;
                return {rx_timestamp, std::move(frame).release(tx_crc)};
            });
        EXPECT_CALL(payload_mr_mock, do_allocate(frame_size, alignof(std::max_align_t)))
            .WillOnce([this](const std::size_t size_bytes, const std::size_t alignment) -> void* {
                return payload_mr_.allocate(size_bytes, alignment);
            });
        EXPECT_CALL(payload_mr_mock, do_deallocate(_, frame_size, alignof(std::max_align_t)))
            .WillOnce([this](void* const p, const std::size_t size_bytes, const std::size_t alignment) {
                payload_mr_.deallocate(p, size_bytes, alignment);
            });
        scheduler_.scheduleNamedCallback("rx_socket", rx_timestamp);

        scheduler_.scheduleAt(rx_timestamp + 1ms, [&](const auto&) {
            //
            EXPECT_THAT(session_n31->receive(), Eq(cetl::nullopt));
            EXPECT_THAT(session_n32->receive(), Eq(cetl::nullopt));
        });
    });
    scheduler_.scheduleAt(9s, [&](const auto&) {
        //
        session_n31.reset();
        session_n32.reset();
        EXPECT_CALL(rx_socket_mock_, deinit());
        transport.reset();
        testing::Mock::VerifyAndClearExpectations(&rx_socket_mock_);
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestUdpSvcRxSessions, unsubscribe)
{
    auto transport = makeTransport({mr_}, NodeId{0x31});

    EXPECT_CALL(rx_socket_mock_, registerCallback(_))  //
        .WillOnce(Invoke([&](auto function) {          //
            return scheduler_.registerCallback(std::move(function));
        }));

    constexpr std::size_t extent_bytes  = 8;
    auto                  maybe_session = transport->makeRequestRxSession({extent_bytes, 0x17B});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        session.reset();
    });
    scheduler_.scheduleAt(9s, [&](const auto&) {
        //
        EXPECT_CALL(rx_socket_mock_, deinit());
        transport.reset();
        testing::Mock::VerifyAndClearExpectations(&rx_socket_mock_);
    });
    scheduler_.spinFor(10s);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

}  // namespace
