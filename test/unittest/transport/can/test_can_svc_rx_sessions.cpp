/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#include "can_gtest_helpers.hpp"   // NOLINT(misc-include-cleaner)
#include "cetl_gtest_helpers.hpp"  // NOLINT(misc-include-cleaner)
#include "gtest_helpers.hpp"       // NOLINT(misc-include-cleaner)
#include "media_mock.hpp"
#include "memory_resource_mock.hpp"
#include "tracking_memory_resource.hpp"
#include "transport/transport_gtest_helpers.hpp"  // NOLINT(misc-include-cleaner)
#include "verification_utilities.hpp"
#include "virtual_time_scheduler.hpp"

#include <canard.h>
#include <cetl/pf17/cetlpf.hpp>
#include <cetl/pf20/cetlpf.hpp>
#include <libcyphal/errors.hpp>
#include <libcyphal/transport/can/can_transport.hpp>
#include <libcyphal/transport/can/can_transport_impl.hpp>
#include <libcyphal/transport/can/media.hpp>
#include <libcyphal/transport/can/svc_rx_sessions.hpp>
#include <libcyphal/transport/errors.hpp>
#include <libcyphal/transport/svc_sessions.hpp>
#include <libcyphal/transport/types.hpp>
#include <libcyphal/types.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

using libcyphal::Duration;
using libcyphal::TimePoint;
using libcyphal::UniquePtr;
using libcyphal::MemoryError;
using namespace libcyphal::transport;       // NOLINT This our main concern here in the unit tests.
using namespace libcyphal::transport::can;  // NOLINT This our main concern here in the unit tests.

using cetl::byte;
using libcyphal::verification_utilities::b;

using testing::_;
using testing::Eq;
using testing::Invoke;
using testing::Return;
using testing::SizeIs;
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

class TestCanSvcRxSessions : public testing::Test
{
protected:
    void SetUp() override
    {
        cetl::pmr::set_default_resource(&mr_);

        EXPECT_CALL(media_mock_, getMtu())  //
            .WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
        EXPECT_CALL(media_mock_, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));
    }

    void TearDown() override
    {
        EXPECT_THAT(mr_.allocations, IsEmpty());
        EXPECT_THAT(mr_.total_allocated_bytes, mr_.total_deallocated_bytes);

        EXPECT_THAT(tx_mr_.allocations, IsEmpty());
        EXPECT_THAT(tx_mr_.total_allocated_bytes, tx_mr_.total_deallocated_bytes);
    }

    TimePoint now() const
    {
        return scheduler_.now();
    }

    UniquePtr<ICanTransport> makeTransport(cetl::pmr::memory_resource& mr,
                                           const NodeId                local_node_id,
                                           IMedia*                     extra_media = nullptr)
    {
        std::array<IMedia*, 2> media_array{&media_mock_, extra_media};

        auto maybe_transport = can::makeTransport(mr, scheduler_, media_array, 0);
        EXPECT_THAT(maybe_transport, VariantWith<UniquePtr<ICanTransport>>(NotNull()));
        auto transport = cetl::get<UniquePtr<ICanTransport>>(std::move(maybe_transport));

        transport->setLocalNodeId(local_node_id);

        return transport;
    }

    IMedia::PopResult::Metadata makeFragmentFromCanDumpLine(const std::string&           can_dump_line,
                                                            const cetl::span<cetl::byte> payload) const
    {
        const auto pound = can_dump_line.find('#');

        const auto can_id = std::stoul(can_dump_line.substr(0, pound), nullptr, 16);

        const auto data_str = can_dump_line.substr(pound + 1);
        const auto data_len = data_str.size() / 2;
        CETL_DEBUG_ASSERT(data_len <= CANARD_MTU_CAN_CLASSIC, "");

        std::size_t end_pos{0};
        auto        data = std::stoull(data_str, &end_pos, 16);
        CETL_DEBUG_ASSERT(end_pos == data_len * 2, "");

        for (std::size_t i = data_len; i > 0; --i, data >>= 8U)
        {
            payload[i - 1] = static_cast<cetl::byte>(data & 0xFFU);
        }

        return {now(), static_cast<CanId>(can_id), data_len};
    }

    // MARK: Data members:

    // NOLINTBEGIN
    libcyphal::VirtualTimeScheduler scheduler_{};
    TrackingMemoryResource          mr_;
    TrackingMemoryResource          tx_mr_;
    StrictMock<MediaMock>           media_mock_{};
    // NOLINTEND
};

// MARK: - Tests:

TEST_F(TestCanSvcRxSessions, make_request_setTransferIdTimeout)
{
    auto transport = makeTransport(mr_, 0x31);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx", std::move(function));
        }));

    auto maybe_session = transport->makeRequestRxSession({42, 123});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    EXPECT_THAT(session->getParams().extent_bytes, 42);
    EXPECT_THAT(session->getParams().service_id, 123);

    // NOLINTNEXTLINE
    auto&                 delegate        = static_cast<can::detail::TransportImpl*>(transport.get())->asDelegate();
    auto&                 canard_instance = delegate.canardInstance();
    CanardRxSubscription* subscription    = nullptr;
    EXPECT_THAT(canardRxGetSubscription(&canard_instance, CanardTransferKindRequest, 123, &subscription), 1);
    ASSERT_THAT(subscription, NotNull());
    EXPECT_THAT(subscription->transfer_id_timeout_usec, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);

    session->setTransferIdTimeout(-1ms);  // negative value is not allowed (rejected)
    EXPECT_THAT(subscription->transfer_id_timeout_usec, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);

    session->setTransferIdTimeout(0s);
    EXPECT_THAT(subscription->transfer_id_timeout_usec, 0);

    session->setTransferIdTimeout(500ms);
    EXPECT_THAT(subscription->transfer_id_timeout_usec, 500'000);

    EXPECT_THAT(scheduler_.hasNamedCallback("rx"), true);
    session.reset();
    EXPECT_THAT(scheduler_.hasNamedCallback("rx"), false);
}

TEST_F(TestCanSvcRxSessions, make_response_no_memory)
{
    StrictMock<MemoryResourceMock> mr_mock;
    mr_mock.redirectExpectedCallsTo(mr_);

    // Emulate that there is no memory available for the message session.
    EXPECT_CALL(mr_mock, do_allocate(sizeof(can::detail::SvcResponseRxSession), _))  //
        .WillOnce(Return(nullptr));

    const auto transport = makeTransport(mr_mock, 0x13);

    const auto maybe_session = transport->makeResponseRxSession({64, 0x23, 0x45});
    EXPECT_THAT(maybe_session, VariantWith<AnyFailure>(VariantWith<MemoryError>(_)));
}

TEST_F(TestCanSvcRxSessions, make_request_no_memory)
{
    StrictMock<MemoryResourceMock> mr_mock;
    mr_mock.redirectExpectedCallsTo(mr_);

    // Emulate that there is no memory available for the message session.
    EXPECT_CALL(mr_mock, do_allocate(sizeof(can::detail::SvcRequestRxSession), _))  //
        .WillOnce(Return(nullptr));

    const auto transport = makeTransport(mr_mock, 0x31);

    const auto maybe_session = transport->makeRequestRxSession({64, 123});
    EXPECT_THAT(maybe_session, VariantWith<AnyFailure>(VariantWith<MemoryError>(_)));
}

TEST_F(TestCanSvcRxSessions, make_request_fails_due_to_argument_error)
{
    const auto transport = makeTransport(mr_, 0x31);

    // Try invalid subject id
    const auto maybe_session = transport->makeRequestRxSession({64, CANARD_SERVICE_ID_MAX + 1});
    EXPECT_THAT(maybe_session, VariantWith<AnyFailure>(VariantWith<libcyphal::ArgumentError>(_)));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(TestCanSvcRxSessions, receive_request)
{
    auto transport = makeTransport(mr_, 0x31);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx", std::move(function));
        }));

    constexpr std::size_t extent_bytes  = 8;
    auto                  maybe_session = transport->makeRequestRxSession({extent_bytes, 0x17B});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    EXPECT_CALL(media_mock_, setFilters(SizeIs(1)))  //
        .WillOnce([&](const Filters filters) {
            EXPECT_THAT(filters,
                        Contains(FilterEq({0b1'0'0'101111011'0110001'0000000, 0b1'0'1'111111111'1111111'0000000})));
            return cetl::nullopt;
        });

    const auto params = session->getParams();
    EXPECT_THAT(params.extent_bytes, extent_bytes);
    EXPECT_THAT(params.service_id, 0x17B);

    constexpr auto timeout = 200ms;
    session->setTransferIdTimeout(timeout);

    TimePoint rx_timestamp;

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        SCOPED_TRACE("1-st iteration: one frame available @ 1s");

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(media_mock_, pop(_))  //
            .WillOnce([&](auto p) {
                EXPECT_THAT(now(), rx_timestamp);
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b(42);
                p[1] = b(147);
                p[2] = b(0b111'11101);
                return IMedia::PopResult::Metadata{rx_timestamp, 0b011'1'1'0'101111011'0110001'0010011, 3};
            });
        scheduler_.scheduleNamedCallback("rx", rx_timestamp);

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
            EXPECT_THAT(rx_transfer.payload.size(), 2);
            EXPECT_THAT(rx_transfer.payload.copy(0, buffer.data(), buffer.size()), 2);
            EXPECT_THAT(buffer, ElementsAre(42, 147));
        });
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        SCOPED_TRACE("2-nd iteration: no frames available @ 2s");

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(media_mock_, pop(_))  //
            .WillOnce([&](auto payload) {
                EXPECT_THAT(now(), rx_timestamp);
                EXPECT_THAT(payload.size(), CANARD_MTU_MAX);
                return cetl::nullopt;
            });
        scheduler_.scheduleNamedCallback("rx", rx_timestamp);

        scheduler_.scheduleAt(rx_timestamp + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session->receive();
            EXPECT_THAT(maybe_rx_transfer, Eq(cetl::nullopt));
        });
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanSvcRxSessions, receive_request_via_callback)
{
    const auto transport = makeTransport(mr_, 0x31);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx", std::move(function));
        }));

    auto maybe_session = transport->makeRequestRxSession({8, 0x17B});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    EXPECT_CALL(media_mock_, setFilters(SizeIs(1)))  //
        .WillOnce([&](const Filters filters) {
            EXPECT_THAT(filters,
                        Contains(FilterEq({0b1'0'0'101111011'0110001'0000000, 0b1'0'1'111111111'1111111'0000000})));
            return cetl::nullopt;
        });

    TimePoint rx_timestamp;

    session->setOnReceiveCallback([&](const IRequestRxSession::OnReceiveCallback::Arg& arg) {
        //
        EXPECT_THAT(arg.transfer.metadata.rx_meta.timestamp, rx_timestamp);
        EXPECT_THAT(arg.transfer.metadata.rx_meta.base.transfer_id, 0x1D);
        EXPECT_THAT(arg.transfer.metadata.rx_meta.base.priority, Priority::High);
        EXPECT_THAT(arg.transfer.metadata.remote_node_id, 0x13);

        std::array<char, 2> buffer{};
        ASSERT_THAT(arg.transfer.payload.size(), buffer.size());
        EXPECT_THAT(arg.transfer.payload.copy(0, buffer.data(), buffer.size()), buffer.size());
        EXPECT_THAT(buffer, ElementsAre(42, 147));
    });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        rx_timestamp = now() + 10ms;
        EXPECT_CALL(media_mock_, pop(_))  //
            .WillOnce([&](auto p) {
                EXPECT_THAT(now(), rx_timestamp);
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b(42);
                p[1] = b(147);
                p[2] = b(0b111'11101);
                return IMedia::PopResult::Metadata{rx_timestamp, 0b011'1'1'0'101111011'0110001'0010011, 3};
            });
        scheduler_.scheduleNamedCallback("rx", rx_timestamp);

        scheduler_.scheduleAt(rx_timestamp + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session->receive();
            ASSERT_THAT(maybe_rx_transfer, Eq(cetl::nullopt));  // b/c was "consumed" by the callback.
        });
    });
    scheduler_.spinFor(10s);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(TestCanSvcRxSessions, receive_response)
{
    auto transport = makeTransport(mr_, 0x13);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx", std::move(function));
        }));

    constexpr std::size_t extent_bytes      = 8;
    auto                  maybe_session_n31 = transport->makeResponseRxSession({extent_bytes, 0x17B, 0x31});
    ASSERT_THAT(maybe_session_n31, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));
    auto session_n31 = cetl::get<UniquePtr<IResponseRxSession>>(std::move(maybe_session_n31));

    EXPECT_CALL(media_mock_, setFilters(SizeIs(1)))  //
        .WillOnce([&](const Filters filters) {
            EXPECT_THAT(filters,
                        Contains(FilterEq({0b1'0'0'101111011'0010011'0000000, 0b1'0'1'111111111'1111111'0000000})));
            return cetl::nullopt;
        });

    const auto params = session_n31->getParams();
    EXPECT_THAT(params.extent_bytes, extent_bytes);
    EXPECT_THAT(params.service_id, 0x17B);
    EXPECT_THAT(params.server_node_id, 0x31);

    constexpr auto timeout = 200ms;
    session_n31->setTransferIdTimeout(timeout);

    // Create another session with the same port ID but different server node ID (0x32).
    //
    auto maybe_session_n32 = transport->makeResponseRxSession({extent_bytes, 0x17B, 0x32});
    ASSERT_THAT(maybe_session_n32, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));
    auto session_n32 = cetl::get<UniquePtr<IResponseRxSession>>(std::move(maybe_session_n32));

    TimePoint rx_timestamp;

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        SCOPED_TRACE("1-st iteration: one frame available @ 1s");

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(media_mock_, pop(_))  //
            .WillOnce([&](auto p) {
                EXPECT_THAT(now(), rx_timestamp);
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b(42);
                p[1] = b(147);
                p[2] = b(0b111'11101);
                return IMedia::PopResult::Metadata{rx_timestamp, 0b011'1'0'0'101111011'0010011'0110001, 3};
            });
        scheduler_.scheduleNamedCallback("rx", rx_timestamp);

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
            EXPECT_THAT(rx_transfer.payload.size(), 2);
            EXPECT_THAT(rx_transfer.payload.copy(0, buffer.data(), buffer.size()), 2);
            EXPECT_THAT(buffer, ElementsAre(42, 147));
        });
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        SCOPED_TRACE("2-nd iteration: no frames available @ 2s");

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(media_mock_, pop(_))  //
            .WillOnce([&](auto payload) {
                EXPECT_THAT(now(), rx_timestamp);
                EXPECT_THAT(payload.size(), CANARD_MTU_MAX);
                return cetl::nullopt;
            });
        scheduler_.scheduleNamedCallback("rx", rx_timestamp);

        scheduler_.scheduleAt(rx_timestamp + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session_n31->receive();
            EXPECT_THAT(maybe_rx_transfer, Eq(cetl::nullopt));
        });
    });
    scheduler_.scheduleAt(3s, [&](const auto&) {
        //
        SCOPED_TRACE("3-rd iteration: unsolicited (node 0x33) response @ 3s");

        rx_timestamp = now() + 10ms;
        EXPECT_CALL(media_mock_, pop(_))  //
            .WillOnce([&](auto p) {
                EXPECT_THAT(now(), rx_timestamp);
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b(0b111'11101);
                return IMedia::PopResult::Metadata{rx_timestamp, 0b011'1'0'0'101111011'0010011'0110011, 1};
            });
        scheduler_.scheduleNamedCallback("rx", rx_timestamp);

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
        transport.reset();
    });
    scheduler_.spinFor(10s);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(TestCanSvcRxSessions, receive_two_frames)
{
    const auto transport = makeTransport(mr_, 0x31);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx", std::move(function));
        }));

    constexpr std::size_t extent_bytes  = 8;
    auto                  maybe_session = transport->makeRequestRxSession({extent_bytes, 0x17B});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    const auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    EXPECT_CALL(media_mock_, setFilters(SizeIs(1)))  //
        .WillOnce([&](const Filters filters) {
            EXPECT_THAT(filters,
                        Contains(FilterEq({0b1'0'0'101111011'0110001'0000000, 0b1'0'1'111111111'1111111'0000000})));
            return cetl::nullopt;
        });

    constexpr auto first_rx_timestamp = TimePoint{1s + 10ms};

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, pop(_))  //
            .WillOnce([&](auto p) {
                EXPECT_THAT(now(), first_rx_timestamp);
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b('0');
                p[1] = b('1');
                p[2] = b('2');
                p[3] = b('3');
                p[4] = b('4');
                p[5] = b('5');
                p[6] = b('6');
                p[7] = b(0b101'11110);
                return IMedia::PopResult::Metadata{first_rx_timestamp, 0b000'1'1'0'101111011'0110001'0010011, 8};
            });
        scheduler_.scheduleNamedCallback("rx", first_rx_timestamp);

        scheduler_.scheduleAt(first_rx_timestamp + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session->receive();
            EXPECT_THAT(maybe_rx_transfer, Eq(cetl::nullopt));
        });
    });
    scheduler_.scheduleAt(first_rx_timestamp + 3ms, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, pop(_))  //
            .WillOnce([&](auto p) {
                EXPECT_THAT(now(), first_rx_timestamp + 3ms);
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b('7');
                p[1] = b('8');
                p[2] = b('9');
                p[3] = b(0x7D);
                p[4] = b(0x61);  // expected 16-bit CRC
                p[5] = b(0b010'11110);
                return IMedia::PopResult::Metadata{first_rx_timestamp, 0b000'1'1'0'101111011'0110001'0010011, 6};
            });
        scheduler_.scheduleNamedCallback("rx", now());

        scheduler_.scheduleAt(now() + 1ms, [&](const auto&) {
            //
            const auto maybe_rx_transfer = session->receive();
            ASSERT_THAT(maybe_rx_transfer, Optional(_));
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            const auto& rx_transfer = maybe_rx_transfer.value();

            EXPECT_THAT(rx_transfer.metadata.rx_meta.timestamp, first_rx_timestamp);
            EXPECT_THAT(rx_transfer.metadata.rx_meta.base.transfer_id, 0x1E);
            EXPECT_THAT(rx_transfer.metadata.rx_meta.base.priority, Priority::Exceptional);
            EXPECT_THAT(rx_transfer.metadata.remote_node_id, 0x13);

            std::array<char, extent_bytes> buffer{};
            EXPECT_THAT(rx_transfer.payload.size(), buffer.size());
            EXPECT_THAT(rx_transfer.payload.copy(0, buffer.data(), buffer.size()), buffer.size());
            EXPECT_THAT(buffer, ElementsAre('0', '1', '2', '3', '4', '5', '6', '7'));
        });
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanSvcRxSessions, unsubscribe)
{
    const auto transport = makeTransport(mr_, 0x31);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx", std::move(function));
        }));

    constexpr std::size_t extent_bytes  = 8;
    auto                  maybe_session = transport->makeRequestRxSession({extent_bytes, 0x17B});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    EXPECT_CALL(media_mock_, setFilters(SizeIs(1)))  //
        .WillOnce([&](const Filters filters) {
            EXPECT_THAT(filters, Contains(FilterEq({0x025ED880, 0x02FFFF80})));
            return cetl::nullopt;
        });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
            .WillOnce([&](const Filters) {               //
                return cetl::nullopt;
            });
        session.reset();
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanSvcRxSessions, receive_multiple_tids_frames)
{
    StrictMock<MediaMock> media_mock2{};
    EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
    EXPECT_CALL(media_mock2, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));

    auto transport = makeTransport(mr_, 42, &media_mock2);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("slcan0", std::move(function));
        }));
    EXPECT_CALL(media_mock_, setFilters(SizeIs(1))).WillOnce(Return(cetl::nullopt));
    EXPECT_CALL(media_mock2, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("slcan2", std::move(function));
        }));
    EXPECT_CALL(media_mock2, setFilters(SizeIs(1))).WillOnce(Return(cetl::nullopt));

    constexpr std::size_t extent_bytes  = 8;
    auto                  maybe_session = transport->makeResponseRxSession({extent_bytes, 147, 47});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IResponseRxSession>>(std::move(maybe_session));
    session->setTransferIdTimeout(0s);

    std::vector<std::tuple<TimePoint, std::string>> calls;
    session->setOnReceiveCallback([&](const auto& arg) {
        //
        calls.push_back(std::make_tuple(now(), testing::PrintToString(arg.transfer.metadata)));
    });

    const std::array<std::tuple<MediaMock&, Duration, std::string, std::string>, 20> frames = {
        //
        // response 1001, tid=0, accepted
        std::make_tuple(std::ref(media_mock_), 350755us, "slcan0", "1224D52F#E9030000000000A0"),  // ☑️create!,iface←0
        std::make_tuple(std::ref(media_mock_), 350764us, "slcan0", "1224D52F#00C08C40"),          // ⚡️0️⃣tid←1
        //
        // CAN2 response 1001, tid=0, dropped as duplicate
        std::make_tuple(std::ref(media_mock2), 350783us, "slcan2", "1224D52F#E9030000000000A0"),  // ❌tid≠1
        std::make_tuple(std::ref(media_mock2), 351331us, "slcan2", "1224D52F#00C08C40"),          // ❌tid≠1
        //
        // CAN2 response 2001, tid=1, accepted by resync to interface #2
        std::make_tuple(std::ref(media_mock2), 351336us, "slcan2", "1224D52F#D1070000000000A1"),  // ☑️tid=1,iface←2
        std::make_tuple(std::ref(media_mock2), 351338us, "slcan2", "1224D52F#00594C41"),          // ⚡️1️⃣tid←2
        //
        // CAN2 partial response 3001, accepted
        std::make_tuple(std::ref(media_mock2), 351340us, "slcan2", "1224D52F#B90B0000000000A2"),  // ☑️
        //
        // CAN0 response 2001, tid=1, dropped as duplicate
        std::make_tuple(std::ref(media_mock_), 351473us, "slcan0", "1224D52F#D1070000000000A1"),  // ❌tid≠2
        std::make_tuple(std::ref(media_mock_), 351476us, "slcan0", "1224D52F#00594C41"),          // ❌tid≠2
        //
        // CAN0 response 3001, tid=2, dropped as wrong interface (expected #2)
        std::make_tuple(std::ref(media_mock_), 351478us, "slcan0", "1224D52F#B90B0000000000A2"),  // ❌iface≠2 & !idle
        std::make_tuple(std::ref(media_mock_), 351479us, "slcan0", "1224D52F#00984542"),          // ❌iface≠2 & !idle
        //
        // CAN2 final fragment response 3001, tid=2, accepted
        std::make_tuple(std::ref(media_mock2), 351697us, "slcan2", "1224D52F#00984542"),  //      // ⚡️2️⃣tid←3
        //
        // CAN2 response 4001, tid=3, accepted
        std::make_tuple(std::ref(media_mock2), 351700us, "slcan2", "1224D52F#A10F0000000000A3"),  // ☑️
        std::make_tuple(std::ref(media_mock2), 351702us, "slcan2", "1224D52F#007AED43"),          // ⚡️3️⃣tid←4
        //
        // CAN0 response 4001, tid=3, dropped as duplicate
        std::make_tuple(std::ref(media_mock_), 351730us, "slcan0", "1224D52F#A10F0000000000A3"),  // ❌tid≠4
        std::make_tuple(std::ref(media_mock_), 351732us, "slcan0", "1224D52F#007AED43"),          // ❌tid≠4
        //
        // CAN2 response 5001, tid=4, accepted
        std::make_tuple(std::ref(media_mock2), 352747us, "slcan2", "1224D52F#89130000000000A4"),  // ☑️
        std::make_tuple(std::ref(media_mock2), 352777us, "slcan2", "1224D52F#007A4F44"),          // ⚡️4️⃣tid←5
        //
        // CAN0 response 5001, tid=4, dropped as duplicate
        std::make_tuple(std::ref(media_mock_), 352800us, "slcan0", "1224D52F#89130000000000A4"),  // ❌tid≠5
        std::make_tuple(std::ref(media_mock_), 352812us, "slcan0", "1224D52F#007A4F44"),          // ❌tid≠5
    };
    for (const auto& frame : frames)
    {
        scheduler_.scheduleAt(TimePoint{std::get<1>(frame)}, [this, &frame](const auto&) {
            //
            scheduler_.scheduleNamedCallback(std::get<2>(frame), TimePoint{std::get<1>(frame)});
            EXPECT_CALL(std::get<0>(frame), pop(_)).WillOnce([&](auto p) {  //
                return makeFragmentFromCanDumpLine(std::get<3>(frame), p);
            });
        });
    }
    scheduler_.spinFor(10s);

    EXPECT_THAT(calls,
                ElementsAre(std::make_tuple(  //
                                TimePoint{350764us},
                                "SvcRxMetadata{rx_meta=TransferRxMetadata{base=TransferMetadata{transfer_id=0, "
                                "priority=Nominal(4)}, timestamp=350755us}, remote_node_id=47}"),
                            std::make_tuple(  //
                                TimePoint{351338us},
                                "SvcRxMetadata{rx_meta=TransferRxMetadata{base=TransferMetadata{transfer_id=1, "
                                "priority=Nominal(4)}, timestamp=351336us}, remote_node_id=47}"),
                            std::make_tuple(  //
                                TimePoint{351697us},
                                "SvcRxMetadata{rx_meta=TransferRxMetadata{base=TransferMetadata{transfer_id=2, "
                                "priority=Nominal(4)}, timestamp=351340us}, remote_node_id=47}"),
                            std::make_tuple(  //
                                TimePoint{351702us},
                                "SvcRxMetadata{rx_meta=TransferRxMetadata{base=TransferMetadata{transfer_id=3, "
                                "priority=Nominal(4)}, timestamp=351700us}, remote_node_id=47}"),
                            std::make_tuple(  //
                                TimePoint{352777us},
                                "SvcRxMetadata{rx_meta=TransferRxMetadata{base=TransferMetadata{transfer_id=4, "
                                "priority=Nominal(4)}, timestamp=352747us}, remote_node_id=47}")));
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

}  // namespace
