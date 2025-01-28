/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#include "can_gtest_helpers.hpp"
#include "cetl_gtest_helpers.hpp"  // NOLINT(misc-include-cleaner)
#include "gtest_helpers.hpp"       // NOLINT(misc-include-cleaner)
#include "media_mock.hpp"
#include "memory_resource_mock.hpp"
#include "tracking_memory_resource.hpp"
#include "transient_error_handler_mock.hpp"
#include "verification_utilities.hpp"
#include "virtual_time_scheduler.hpp"

#include <canard.h>
#include <cetl/pf17/cetlpf.hpp>
#include <libcyphal/errors.hpp>
#include <libcyphal/transport/can/can_transport.hpp>
#include <libcyphal/transport/can/can_transport_impl.hpp>
#include <libcyphal/transport/can/media.hpp>
#include <libcyphal/transport/errors.hpp>
#include <libcyphal/transport/msg_sessions.hpp>
#include <libcyphal/transport/svc_sessions.hpp>
#include <libcyphal/transport/types.hpp>
#include <libcyphal/types.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>

namespace
{

using libcyphal::TimePoint;
using libcyphal::UniquePtr;
using libcyphal::MemoryError;
using namespace libcyphal::transport;       // NOLINT This our main concern here in the unit tests.
using namespace libcyphal::transport::can;  // NOLINT This our main concern here in the unit tests.

using libcyphal::verification_utilities::b;
using libcyphal::verification_utilities::makeIotaArray;
using libcyphal::verification_utilities::makeSpansFrom;

using testing::_;
using testing::Eq;
using testing::Ref;
using testing::Truly;
using testing::Invoke;
using testing::Return;
using testing::SizeIs;
using testing::IsEmpty;
using testing::NotNull;
using testing::Contains;
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

class MyPlatformError final : public IPlatformError
{
public:
    explicit MyPlatformError(const std::uint32_t code)
        : code_{code}
    {
    }
    virtual ~MyPlatformError()                             = default;
    MyPlatformError(const MyPlatformError&)                = default;
    MyPlatformError(MyPlatformError&&) noexcept            = default;
    MyPlatformError& operator=(const MyPlatformError&)     = default;
    MyPlatformError& operator=(MyPlatformError&&) noexcept = default;

    // MARK: IPlatformError

    std::uint32_t code() const noexcept override
    {
        return code_;
    }

private:
    std::uint32_t code_;

};  // MyPlatformError

class TestCanTransport : public testing::Test
{
protected:
    void SetUp() override
    {
        cetl::pmr::set_default_resource(&mr_);

        EXPECT_CALL(media_mock_, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
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
                                           IMedia*                     extra_media = nullptr,
                                           const std::size_t           tx_capacity = 16)
    {
        std::array<IMedia*, 2> media_array{&media_mock_, extra_media};

        auto maybe_transport = can::makeTransport(mr, scheduler_, media_array, tx_capacity);
        EXPECT_THAT(maybe_transport, VariantWith<UniquePtr<ICanTransport>>(NotNull()));
        return cetl::get<UniquePtr<ICanTransport>>(std::move(maybe_transport));
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

TEST_F(TestCanTransport, makeTransport_no_memory_at_all)
{
    StrictMock<MemoryResourceMock> mr_mock;
    mr_mock.redirectExpectedCallsTo(mr_);

    // Emulate that there is no memory at all (even for initial array of media).
    EXPECT_CALL(mr_mock, do_allocate(_, _)).WillRepeatedly(Return(nullptr));
#if (__cplusplus < CETL_CPP_STANDARD_17)
    EXPECT_CALL(mr_mock, do_reallocate(nullptr, 0, _, _)).WillRepeatedly(Return(nullptr));
#endif

    std::array<IMedia*, 1> media_array{&media_mock_};
    auto                   maybe_transport = can::makeTransport(mr_mock, scheduler_, media_array, 0);
    EXPECT_THAT(maybe_transport, VariantWith<FactoryFailure>(VariantWith<MemoryError>(_)));
}

TEST_F(TestCanTransport, makeTransport_no_memory_for_impl)
{
    StrictMock<MemoryResourceMock> mr_mock;
    mr_mock.redirectExpectedCallsTo(mr_);

    // Emulate that there is no memory available for the transport.
    EXPECT_CALL(mr_mock, do_allocate(sizeof(can::detail::TransportImpl), _)).WillOnce(Return(nullptr));

    std::array<IMedia*, 1> media_array{&media_mock_};
    auto                   maybe_transport = can::makeTransport(mr_mock, scheduler_, media_array, 0);
    EXPECT_THAT(maybe_transport, VariantWith<FactoryFailure>(VariantWith<MemoryError>(_)));
}

TEST_F(TestCanTransport, makeTransport_too_many_media)
{
    // Canard use `std::uint8_t` as a media index, so 256+ media interfaces are not allowed.
    std::array<IMedia*, std::numeric_limits<std::uint8_t>::max() + 1> media_array{};
    std::fill(media_array.begin(), media_array.end(), &media_mock_);

    auto maybe_transport = can::makeTransport(mr_, scheduler_, media_array, 0);
    EXPECT_THAT(maybe_transport, VariantWith<FactoryFailure>(VariantWith<libcyphal::ArgumentError>(_)));
}

TEST_F(TestCanTransport, makeTransport_getLocalNodeId)
{
    // Anonymous node
    {
        std::array<IMedia*, 1> media_array{&media_mock_};
        auto                   maybe_transport = can::makeTransport(mr_, scheduler_, media_array, 0);
        ASSERT_THAT(maybe_transport, VariantWith<UniquePtr<ICanTransport>>(NotNull()));

        auto transport = cetl::get<UniquePtr<ICanTransport>>(std::move(maybe_transport));
        EXPECT_THAT(transport->getLocalNodeId(), Eq(cetl::nullopt));
    }

    // Node with ID
    {
        std::array<IMedia*, 1> media_array{&media_mock_};
        auto                   maybe_transport = can::makeTransport(mr_, scheduler_, media_array, 0);
        ASSERT_THAT(maybe_transport, VariantWith<UniquePtr<ICanTransport>>(NotNull()));

        auto transport = cetl::get<UniquePtr<ICanTransport>>(std::move(maybe_transport));
        transport->setLocalNodeId(42);

        EXPECT_THAT(transport->getLocalNodeId(), Optional(42));
    }

    // Two media interfaces
    {
        StrictMock<MediaMock> media_mock2;
        EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_MAX));
        EXPECT_CALL(media_mock2, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));

        std::array<IMedia*, 3> media_array{&media_mock_, nullptr, &media_mock2};
        auto                   maybe_transport = can::makeTransport(mr_, scheduler_, media_array, 0);
        EXPECT_THAT(maybe_transport, VariantWith<UniquePtr<ICanTransport>>(NotNull()));
    }

    // All 3 maximum number of media interfaces
    {
        StrictMock<MediaMock> media_mock2{};
        StrictMock<MediaMock> media_mock3{};
        EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_MAX));
        EXPECT_CALL(media_mock3, getMtu()).WillRepeatedly(Return(CANARD_MTU_MAX));
        EXPECT_CALL(media_mock2, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));
        EXPECT_CALL(media_mock3, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));

        std::array<IMedia*, 3> media_array{&media_mock_, &media_mock2, &media_mock3};
        auto                   maybe_transport = can::makeTransport(mr_, scheduler_, media_array, 0);
        EXPECT_THAT(maybe_transport, VariantWith<UniquePtr<ICanTransport>>(NotNull()));
    }
}

TEST_F(TestCanTransport, setLocalNodeId)
{
    auto transport = makeTransport(mr_);

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_THAT(transport->setLocalNodeId(CANARD_NODE_ID_MAX + 1),
                    Optional(testing::A<libcyphal::ArgumentError>()));
        EXPECT_THAT(transport->getLocalNodeId(), Eq(cetl::nullopt));
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        EXPECT_THAT(transport->setLocalNodeId(CANARD_NODE_ID_MAX), Eq(cetl::nullopt));
        EXPECT_THAT(transport->getLocalNodeId(), Optional(CANARD_NODE_ID_MAX));
    });
    scheduler_.scheduleAt(3s, [&](const auto&) {
        //
        EXPECT_THAT(transport->setLocalNodeId(CANARD_NODE_ID_MAX), Eq(cetl::nullopt));
        EXPECT_THAT(transport->getLocalNodeId(), Optional(CANARD_NODE_ID_MAX));
    });
    scheduler_.scheduleAt(4s, [&](const auto&) {
        //
        EXPECT_THAT(transport->setLocalNodeId(0), Optional(testing::A<libcyphal::ArgumentError>()));
        EXPECT_THAT(transport->getLocalNodeId(), Optional(CANARD_NODE_ID_MAX));
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, makeTransport_with_invalid_arguments)
{
    // No media
    const auto maybe_transport = can::makeTransport(mr_, scheduler_, {}, 0);
    EXPECT_THAT(maybe_transport, VariantWith<FactoryFailure>(VariantWith<libcyphal::ArgumentError>(_)));
}

TEST_F(TestCanTransport, getProtocolParams)
{
    StrictMock<MediaMock> media_mock2{};
    EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_MAX));
    EXPECT_CALL(media_mock2, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));

    std::array<IMedia*, 2> media_array{&media_mock_, &media_mock2};
    auto transport = cetl::get<UniquePtr<ICanTransport>>(can::makeTransport(mr_, scheduler_, media_array, 0));

    EXPECT_CALL(media_mock_, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_FD));
    EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));

    auto params = transport->getProtocolParams();
    EXPECT_THAT(params.transfer_id_modulo, static_cast<TransferId>(1) << CANARD_TRANSFER_ID_BIT_LENGTH);
    EXPECT_THAT(params.max_nodes, CANARD_NODE_ID_MAX + 1);
    EXPECT_THAT(params.mtu_bytes, CANARD_MTU_CAN_CLASSIC);

    // Manipulate MTU values on fly
    {
        EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_FD));
        EXPECT_THAT(transport->getProtocolParams().mtu_bytes, CANARD_MTU_CAN_FD);

        EXPECT_CALL(media_mock_, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
        EXPECT_THAT(transport->getProtocolParams().mtu_bytes, CANARD_MTU_CAN_CLASSIC);

        EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
        EXPECT_THAT(transport->getProtocolParams().mtu_bytes, CANARD_MTU_CAN_CLASSIC);
    }
}

TEST_F(TestCanTransport, makeMessageRxSession)
{
    auto transport = makeTransport(mr_);

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, registerPopCallback(_))  //
            .WillOnce(Invoke([&](auto function) {         //
                return scheduler_.registerNamedCallback("rx", std::move(function));
            }));

        auto maybe_rx_session = transport->makeMessageRxSession({42, 123});
        ASSERT_THAT(maybe_rx_session, VariantWith<UniquePtr<IMessageRxSession>>(NotNull()));

        auto session = cetl::get<UniquePtr<IMessageRxSession>>(std::move(maybe_rx_session));
        EXPECT_THAT(session->getParams().extent_bytes, 42);
        EXPECT_THAT(session->getParams().subject_id, 123);

        session.reset();

        EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
            .WillOnce([&](Filters) { return cetl::nullopt; });
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, makeMessageRxSession_invalid_subject_id)
{
    auto transport = makeTransport(mr_);

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        auto maybe_rx_session = transport->makeMessageRxSession({0, CANARD_SUBJECT_ID_MAX + 1});
        EXPECT_THAT(maybe_rx_session, VariantWith<AnyFailure>(VariantWith<libcyphal::ArgumentError>(_)));
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, makeMessageRxSession_invalid_resubscription)
{
    auto transport = makeTransport(mr_);

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    const PortId test_subject_id = 111;

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, registerPopCallback(_))  //
            .WillOnce(Invoke([&](auto function) {         //
                return scheduler_.registerNamedCallback("rx", std::move(function));
            }));

        auto maybe_rx_session1 = transport->makeMessageRxSession({0, test_subject_id});
        ASSERT_THAT(maybe_rx_session1, VariantWith<UniquePtr<IMessageRxSession>>(NotNull()));

        auto maybe_rx_session2 = transport->makeMessageRxSession({0, test_subject_id});
        EXPECT_THAT(maybe_rx_session2, VariantWith<AnyFailure>(VariantWith<AlreadyExistsError>(_)));

        EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
            .WillOnce([&](Filters) { return cetl::nullopt; });
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, registerPopCallback(_))  //
            .WillOnce(Invoke([&](auto function) {         //
                return scheduler_.registerNamedCallback("rx", std::move(function));
            }));

        auto maybe_rx_session2 = transport->makeMessageRxSession({0, test_subject_id});
        ASSERT_THAT(maybe_rx_session2, VariantWith<UniquePtr<IMessageRxSession>>(NotNull()));

        EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
            .WillOnce([&](Filters) { return cetl::nullopt; });
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, makeRequestRxSession_invalid_resubscription)
{
    auto transport = makeTransport(mr_);

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    const PortId test_subject_id = 111;

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, registerPopCallback(_))  //
            .WillOnce(Invoke([&](auto function) {         //
                return scheduler_.registerNamedCallback("rx", std::move(function));
            }));

        auto maybe_rx_session1 = transport->makeRequestRxSession({0, test_subject_id});
        ASSERT_THAT(maybe_rx_session1, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));

        auto maybe_rx_session2 = transport->makeRequestRxSession({0, test_subject_id});
        EXPECT_THAT(maybe_rx_session2, VariantWith<AnyFailure>(VariantWith<AlreadyExistsError>(_)));

        EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
            .WillOnce([&](Filters) { return cetl::nullopt; });
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, registerPopCallback(_))  //
            .WillOnce(Invoke([&](auto function) {         //
                return scheduler_.registerNamedCallback("rx", std::move(function));
            }));

        auto maybe_rx_session2 = transport->makeRequestRxSession({0, test_subject_id});
        ASSERT_THAT(maybe_rx_session2, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));

        EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
            .WillOnce([&](Filters) { return cetl::nullopt; });
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, makeResponseRxSession_invalid_resubscription)
{
    auto transport = makeTransport(mr_);

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    const PortId test_subject_id = 111;

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, registerPopCallback(_))  //
            .WillOnce(Invoke([&](auto function) {         //
                return scheduler_.registerNamedCallback("rx", std::move(function));
            }));

        auto maybe_rx_session1 = transport->makeResponseRxSession({0, test_subject_id, 0x31});
        ASSERT_THAT(maybe_rx_session1, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));

        auto maybe_rx_session2 = transport->makeResponseRxSession({0, test_subject_id, 0x31});
        EXPECT_THAT(maybe_rx_session2, VariantWith<AnyFailure>(VariantWith<AlreadyExistsError>(_)));

        EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
            .WillOnce([&](Filters) { return cetl::nullopt; });
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, registerPopCallback(_))  //
            .WillOnce(Invoke([&](auto function) {         //
                return scheduler_.registerNamedCallback("rx", std::move(function));
            }));

        auto maybe_rx_session2 = transport->makeResponseRxSession({0, test_subject_id, 0x31});
        ASSERT_THAT(maybe_rx_session2, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));

        EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
            .WillOnce([&](Filters) { return cetl::nullopt; });

        // Different remote node id 0x32!
        auto maybe_rx_session3 = transport->makeResponseRxSession({0, test_subject_id, 0x32});
        ASSERT_THAT(maybe_rx_session3, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, makeMessageTxSession)
{
    auto transport = makeTransport(mr_);

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        auto maybe_tx_session = transport->makeMessageTxSession({123});
        ASSERT_THAT(maybe_tx_session, VariantWith<UniquePtr<IMessageTxSession>>(NotNull()));

        auto session = cetl::get<UniquePtr<IMessageTxSession>>(std::move(maybe_tx_session));
        EXPECT_THAT(session->getParams().subject_id, 123);
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, sending_multiframe_payload_should_fail_for_anonymous)
{
    auto transport = makeTransport(mr_);

    auto maybe_session = transport->makeMessageTxSession({7});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IMessageTxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IMessageTxSession>>(std::move(maybe_session));

    const auto         payload = makeIotaArray<CANARD_MTU_CAN_CLASSIC>(b('0'));
    TransferTxMetadata metadata{{0x13, Priority::Nominal}, {}};

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        metadata.deadline = now() + 1s;
        auto failure      = session->send(metadata, makeSpansFrom(payload));
        EXPECT_THAT(failure, Optional(VariantWith<libcyphal::ArgumentError>(_)));
    });
    scheduler_.spinFor(10s);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(TestCanTransport, sending_multiframe_payload_for_non_anonymous)
{
    auto transport = makeTransport(mr_);
    EXPECT_THAT(transport->setLocalNodeId(0x45), Eq(cetl::nullopt));

    auto maybe_session = transport->makeMessageTxSession({7});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IMessageTxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IMessageTxSession>>(std::move(maybe_session));

    constexpr auto timeout = 1s;

    const auto         payload = makeIotaArray<CANARD_MTU_CAN_CLASSIC>(b('0'));
    TransferTxMetadata metadata{{0x13, Priority::Nominal}, {}};

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, push(_, _, _)).WillOnce([&](auto deadline, auto can_id, auto& pld) {
            EXPECT_THAT(now(), metadata.deadline - timeout);
            EXPECT_THAT(deadline, metadata.deadline);
            EXPECT_THAT(can_id, AllOf(SubjectOfCanIdEq(7), SourceNodeOfCanIdEq(0x45)));
            EXPECT_THAT(can_id, AllOf(PriorityOfCanIdEq(metadata.base.priority), IsMessageCanId()));

            const auto tbm = TailByteEq(metadata.base.transfer_id, true, false);
            EXPECT_THAT(pld.getSpan(), ElementsAre(b('0'), b('1'), b('2'), b('3'), b('4'), b('5'), b('6'), tbm));
            return IMedia::PushResult::Success{true /* is_accepted */};
        });
        EXPECT_CALL(media_mock_, registerPushCallback(_))  //
            .WillOnce(Invoke([&](auto function) {          //
                return scheduler_.registerAndScheduleNamedCallback("", now() + 10us, std::move(function));
            }));

        metadata.deadline = now() + timeout;
        auto failure      = session->send(metadata, makeSpansFrom(payload));
        EXPECT_THAT(failure, Eq(cetl::nullopt));
    });
    scheduler_.scheduleAt(1s + 10us, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, push(_, _, _)).WillOnce([&](auto deadline, auto can_id, auto& pld) {
            EXPECT_THAT(now(), metadata.deadline - timeout + 10us);
            EXPECT_THAT(deadline, metadata.deadline);
            EXPECT_THAT(can_id, AllOf(SubjectOfCanIdEq(7), SourceNodeOfCanIdEq(0x45)));
            EXPECT_THAT(can_id, AllOf(PriorityOfCanIdEq(metadata.base.priority), IsMessageCanId()));

            const auto tbm = TailByteEq(metadata.base.transfer_id, false, true, false);
            EXPECT_THAT(pld.getSpan(), ElementsAre(b('7'), _, _ /* CRC bytes */, tbm));
            return IMedia::PushResult::Success{true /* is_accepted */};
        });
    });
    scheduler_.spinFor(10s);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(TestCanTransport, send_multiframe_payload_to_redundant_not_ready_media)
{
    StrictMock<MediaMock> media_mock2{};
    EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
    EXPECT_CALL(media_mock2, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));

    auto transport = makeTransport(mr_, &media_mock2);
    EXPECT_THAT(transport->setLocalNodeId(0x45), Eq(cetl::nullopt));

    auto maybe_session = transport->makeMessageTxSession({7});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IMessageTxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IMessageTxSession>>(std::move(maybe_session));

    constexpr auto timeout = 1s;

    const auto         payload = makeIotaArray<10>(b('0'));
    TransferTxMetadata metadata{{0x13, Priority::Nominal}, {}};

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });
    EXPECT_CALL(media_mock2, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        // Emulate once that the first media is not ready to push fragment (@10us). So transport will switch to
        // the second media, and will retry with the 1st only when its socket is ready @ +20us.
        //
        EXPECT_CALL(media_mock_, push(_, _, _))  //
            .WillOnce([&](auto, auto, auto&) {
                EXPECT_THAT(now(), metadata.deadline - timeout);
                return IMedia::PushResult::Success{false /* is_accepted */};
            });
        EXPECT_CALL(media_mock_, registerPushCallback(_))  //
            .WillOnce(Invoke([&](auto function) {          //
                return scheduler_.registerAndScheduleNamedCallback("tx1", now() + 20us, std::move(function));
            }));
        EXPECT_CALL(media_mock2, push(_, _, _))  //
            .WillOnce([&](auto deadline, auto can_id, auto& pld) {
                EXPECT_THAT(now(), metadata.deadline - timeout);
                EXPECT_THAT(deadline, metadata.deadline);
                EXPECT_THAT(can_id, AllOf(SubjectOfCanIdEq(7), SourceNodeOfCanIdEq(0x45)));
                EXPECT_THAT(can_id, AllOf(PriorityOfCanIdEq(metadata.base.priority), IsMessageCanId()));

                auto tbm = TailByteEq(metadata.base.transfer_id, true, false);
                EXPECT_THAT(pld.getSpan(), ElementsAre(b('0'), b('1'), b('2'), b('3'), b('4'), b('5'), b('6'), tbm));
                return IMedia::PushResult::Success{true /* is_accepted */};
            });
        EXPECT_CALL(media_mock2, registerPushCallback(_))  //
            .WillOnce(Invoke([&](auto function) {          //
                return scheduler_.registerAndScheduleNamedCallback("tx2", now() + 10us, std::move(function));
            }));

        metadata.deadline = now() + timeout;
        auto failure      = session->send(metadata, makeSpansFrom(payload));
        EXPECT_THAT(failure, Eq(cetl::nullopt));
    });
    scheduler_.scheduleAt(1s + 10us, [&](const auto&) {
        //
        EXPECT_CALL(media_mock2, push(_, _, _))  //
            .WillOnce([&](auto deadline, auto can_id, auto& pld) {
                EXPECT_THAT(now(), metadata.deadline - timeout + 10us);
                EXPECT_THAT(deadline, metadata.deadline);
                EXPECT_THAT(can_id, AllOf(SubjectOfCanIdEq(7), SourceNodeOfCanIdEq(0x45)));
                EXPECT_THAT(can_id, AllOf(PriorityOfCanIdEq(metadata.base.priority), IsMessageCanId()));

                const auto tbm = TailByteEq(metadata.base.transfer_id, false, true, false);
                EXPECT_THAT(pld.getSpan(), ElementsAre(b('7'), b('8'), b('9'), b(0x7D), b(0x61) /* CRC bytes */, tbm));
                return IMedia::PushResult::Success{true /* is_accepted */};
            });
    });
    scheduler_.scheduleAt(1s + 20us, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, push(_, _, _))  //
            .WillOnce([&](auto deadline, auto can_id, auto& pld) {
                EXPECT_THAT(now(), metadata.deadline - timeout + 20us);
                EXPECT_THAT(deadline, metadata.deadline);
                EXPECT_THAT(can_id, AllOf(SubjectOfCanIdEq(7), SourceNodeOfCanIdEq(0x45)));
                EXPECT_THAT(can_id, AllOf(PriorityOfCanIdEq(metadata.base.priority), IsMessageCanId()));

                const auto tbm = TailByteEq(metadata.base.transfer_id, true, false);
                EXPECT_THAT(pld.getSpan(), ElementsAre(b('0'), b('1'), b('2'), b('3'), b('4'), b('5'), b('6'), tbm));
                return IMedia::PushResult::Success{true /* is_accepted */};
            });
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, send_payload_to_redundant_fallible_media)
{
    using MediaPushReport = ICanTransport::TransientErrorReport::MediaPush;

    StrictMock<MediaMock> media_mock2{};
    EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
    EXPECT_CALL(media_mock2, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));

    StrictMock<TransientErrorHandlerMock> handler_mock;

    auto transport = makeTransport(mr_, &media_mock2);
    transport->setTransientErrorHandler(std::ref(handler_mock));
    EXPECT_THAT(transport->setLocalNodeId(0x45), Eq(cetl::nullopt));

    auto maybe_session = transport->makeMessageTxSession({7});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IMessageTxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IMessageTxSession>>(std::move(maybe_session));

    constexpr auto timeout = 1s;

    const auto         payload = makeIotaArray<6>(b('0'));
    TransferTxMetadata metadata{{0x13, Priority::Nominal}, {}};

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });
    EXPECT_CALL(media_mock2, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    // 1. First attempt to push payload.
    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        // Media #0 failed to push, but not media #2 - its frame should be dropped (but not for #2).
        //
        EXPECT_CALL(media_mock_, push(_, _, _))  //
            .WillOnce(Return(CapacityError{}));
        EXPECT_CALL(handler_mock, invoke(VariantWith<MediaPushReport>(Truly([&](auto& report) {
                        EXPECT_THAT(report.failure, VariantWith<CapacityError>(_));
                        EXPECT_THAT(report.media_index, 0);
                        EXPECT_THAT(report.culprit, Ref(media_mock_));
                        return true;
                    }))))
            .WillOnce(Return(cetl::nullopt));
        //
        EXPECT_CALL(media_mock2, push(_, _, _))  //
            .WillOnce(Return(IMedia::PushResult::Success{true /* is_accepted */}));
        EXPECT_CALL(media_mock2, registerPushCallback(_))  //
            .WillOnce(Invoke([&](auto function) {          //
                return scheduler_.registerAndScheduleNamedCallback("", now() + 20us, std::move(function));
            }));

        metadata.deadline = now() + timeout;
        EXPECT_THAT(session->send(metadata, makeSpansFrom(payload)), Eq(cetl::nullopt));
    });
    // 2. Second attempt to push payload (while 1st attempt still in progress for socket 2).
    scheduler_.scheduleAt(1s + 10us, [&](const auto&) {
        //
        // Socket #0 is fine but Socket #2 failed to send - its frame should be dropped (but not for #0).
        //
        EXPECT_CALL(media_mock_, push(_, _, _))  //
            .WillOnce(Return(IMedia::PushResult::Success{true /* is_accepted */}));
        EXPECT_CALL(media_mock_, registerPushCallback(_))  //
            .WillOnce(Invoke([&](auto function) {          //
                return scheduler_.registerAndScheduleNamedCallback("", now() + 5us, std::move(function));
            }));
        //
        EXPECT_CALL(media_mock2, push(_, _, _))  //
            .WillOnce(Return(PlatformError{MyPlatformError{13}}));
        EXPECT_CALL(handler_mock, invoke(VariantWith<MediaPushReport>(Truly([&](auto& report) {
                        EXPECT_THAT(report.failure, VariantWith<PlatformError>(_));
                        EXPECT_THAT(report.media_index, 1);
                        EXPECT_THAT(report.culprit, Ref(media_mock2));
                        return true;
                    }))))
            .WillOnce(Return(cetl::nullopt));

        metadata.deadline = now() + timeout;
        EXPECT_THAT(session->send(metadata, makeSpansFrom(payload)), Eq(cetl::nullopt));
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, send_payload_to_out_of_capacity_canard_tx)
{
    using CanardTxPushReport = ICanTransport::TransientErrorReport::CanardTxPush;

    StrictMock<MediaMock> media_mock2{};
    EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
    EXPECT_CALL(media_mock2, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));

    // Make transport with no TX capacity - this will cause `MemoryError` on send attempts.
    //
    auto transport = makeTransport(mr_, &media_mock2, 0 /*capacity*/);
    EXPECT_THAT(transport->setLocalNodeId(0x45), Eq(cetl::nullopt));
    StrictMock<TransientErrorHandlerMock> handler_mock;
    transport->setTransientErrorHandler(std::ref(handler_mock));

    auto maybe_session = transport->makeMessageTxSession({7});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IMessageTxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IMessageTxSession>>(std::move(maybe_session));

    constexpr auto timeout = 1s;

    const auto         payload = makeIotaArray<6>(b('0'));
    TransferTxMetadata metadata{{0x13, Priority::Nominal}, {}};

    EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });
    EXPECT_CALL(media_mock2, setFilters(IsEmpty()))  //
        .WillOnce([&](Filters) { return cetl::nullopt; });

    // 1st. Try to send a frame with "failing" handler - only the 0-th media index will be used.
    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(handler_mock, invoke(VariantWith<CanardTxPushReport>(Truly([](auto& report) {
                        EXPECT_THAT(report.failure, VariantWith<MemoryError>(_));
                        EXPECT_THAT(report.media_index, 0);
                        EXPECT_THAT(report.culprit, Truly([](auto& canard_ins) {
                                        EXPECT_THAT(canard_ins.node_id, 0x45);
                                        return true;
                                    }));
                        return true;
                    }))))
            .WillOnce(Return(libcyphal::ArgumentError{}));

        metadata.deadline = now() + timeout;
        auto failure      = session->send(metadata, makeSpansFrom(payload));
        EXPECT_THAT(failure, Optional(VariantWith<libcyphal::ArgumentError>(_)));
    });
    // 2nd. Try to send a frame with "succeeding" handler - both media indices will be used.
    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(handler_mock, invoke(VariantWith<CanardTxPushReport>(Truly([](auto& report) {
                        EXPECT_THAT(report.failure, VariantWith<MemoryError>(_));
                        return report.media_index == 0;
                    }))))
            .WillOnce(Return(cetl::nullopt));
        EXPECT_CALL(handler_mock, invoke(VariantWith<CanardTxPushReport>(Truly([](auto& report) {
                        EXPECT_THAT(report.failure, VariantWith<MemoryError>(_));
                        return report.media_index == 1;
                    }))))
            .WillOnce(Return(cetl::nullopt));

        EXPECT_THAT(session->send(metadata, makeSpansFrom(payload)), Eq(cetl::nullopt));
    });
    scheduler_.spinFor(10s);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(TestCanTransport, receive_svc_responses_from_redundant_media)
{
    StrictMock<MediaMock> media_mock2{};
    EXPECT_CALL(media_mock_, pop(_)).WillRepeatedly(Return(cetl::nullopt));
    EXPECT_CALL(media_mock2, pop(_)).WillRepeatedly(Return(cetl::nullopt));
    EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
    EXPECT_CALL(media_mock2, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));

    auto transport = makeTransport(mr_, &media_mock2);
    EXPECT_THAT(transport->setLocalNodeId(0x13), Eq(cetl::nullopt));

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx1", std::move(function));
        }));
    EXPECT_CALL(media_mock2, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx2", std::move(function));
        }));

    auto maybe_session = transport->makeResponseRxSession({64, 0x17B, 0x31});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IResponseRxSession>>(std::move(maybe_session));

    const auto params = session->getParams();
    EXPECT_THAT(params.extent_bytes, 64);
    EXPECT_THAT(params.service_id, 0x17B);
    EXPECT_THAT(params.server_node_id, 0x31);

    constexpr auto timeout = 200ms;
    session->setTransferIdTimeout(timeout);

    EXPECT_CALL(media_mock_, setFilters(SizeIs(1))).WillOnce([&](Filters filters) {
        EXPECT_THAT(filters, Contains(FilterEq({0x25EC980, 0x2FFFF80})));
        return cetl::nullopt;
    });
    EXPECT_CALL(media_mock2, setFilters(SizeIs(1))).WillOnce([&](Filters filters) {
        EXPECT_THAT(filters, Contains(FilterEq({0x25EC980, 0x2FFFF80})));
        return cetl::nullopt;
    });

    constexpr auto rx1_timestamp = TimePoint{10s};
    constexpr auto rx2_timestamp = rx1_timestamp + 2 * timeout;

    scheduler_.scheduleAt(rx1_timestamp, [&](const auto&) {
        //
        // 1. Emulate that only one 1st frame came from the 1st media interface (@ rx1_timestamp)...
        //
        EXPECT_CALL(media_mock_, pop(_))  //
            .WillOnce([&](auto p) {
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b('0');
                p[1] = b('1');
                p[2] = b('2');
                p[3] = b('3');
                p[4] = b('4');
                p[5] = b('5');
                p[6] = b('6');
                p[7] = b(0b101'11101);
                return IMedia::PopResult::Metadata{rx1_timestamp, 0b111'1'0'0'101111011'0010011'0110001, 8};
            });
        scheduler_.scheduleNamedCallback("rx1", rx1_timestamp);
    });
    scheduler_.scheduleAt(rx2_timestamp, [&](const auto&) {
        //
        // 2. And then 2nd media delivered all frames once again after timeout (@ rx2_timestamp).
        //
        EXPECT_CALL(media_mock2, pop(_))  //
            .WillOnce([&](auto p) {
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b('0');
                p[1] = b('1');
                p[2] = b('2');
                p[3] = b('3');
                p[4] = b('4');
                p[5] = b('5');
                p[6] = b('6');
                p[7] = b(0b101'11110);
                return IMedia::PopResult::Metadata{rx2_timestamp, 0b111'1'0'0'101111011'0010011'0110001, 8};
            });
        scheduler_.scheduleNamedCallback("rx2", rx1_timestamp);
    });
    scheduler_.scheduleAt(rx2_timestamp + 1ms, [&](const auto&) {
        //
        EXPECT_CALL(media_mock2, pop(_))  //
            .WillOnce([&](auto p) {
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b('7');
                p[1] = b('8');
                p[2] = b('9');
                p[3] = b(0x7D);
                p[4] = b(0x61);  // expected 16-bit CRC
                p[5] = b(0b010'11110);
                return IMedia::PopResult::Metadata{rx2_timestamp, 0b111'1'0'0'101111011'0010011'0110001, 6};
            });
        scheduler_.scheduleNamedCallback("rx2", rx1_timestamp);
    });
    scheduler_.scheduleAt(rx2_timestamp + 2ms, [&](const auto&) {
        //
        const auto maybe_rx_transfer = session->receive();
        ASSERT_THAT(maybe_rx_transfer, Optional(_));
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        const auto& rx_transfer = maybe_rx_transfer.value();

        EXPECT_THAT(rx_transfer.metadata.rx_meta.timestamp, rx2_timestamp);
        EXPECT_THAT(rx_transfer.metadata.rx_meta.base.transfer_id, 0x1E);
        EXPECT_THAT(rx_transfer.metadata.rx_meta.base.priority, Priority::Optional);
        EXPECT_THAT(rx_transfer.metadata.remote_node_id, 0x31);

        std::array<char, 10> buffer{};
        EXPECT_THAT(rx_transfer.payload.size(), buffer.size());
        EXPECT_THAT(rx_transfer.payload.copy(0, buffer.data(), buffer.size()), buffer.size());
        EXPECT_THAT(buffer, ElementsAre('0', '1', '2', '3', '4', '5', '6', '7', '8', '9'));
    });
    scheduler_.scheduleAt(99s, [&](const auto&) {
        //
        SCOPED_TRACE("unsubscribe @ 99s");

        EXPECT_CALL(media_mock_, setFilters(IsEmpty()))  //
            .WillOnce([&](Filters) {
                EXPECT_THAT(now(), TimePoint{99s});
                return cetl::nullopt;
            });
        EXPECT_CALL(media_mock2, setFilters(IsEmpty()))  //
            .WillOnce([&](Filters) {
                EXPECT_THAT(now(), TimePoint{99s});
                return cetl::nullopt;
            });

        session.reset();
    });
    scheduler_.spinFor(100s);
}

TEST_F(TestCanTransport, receive_svc_responses_from_redundant_fallible_media)
{
    using MediaPopReport = ICanTransport::TransientErrorReport::MediaPop;

    StrictMock<MediaMock> media_mock2{};
    EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
    EXPECT_CALL(media_mock2, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));

    StrictMock<TransientErrorHandlerMock> handler_mock;

    auto transport = makeTransport(mr_, &media_mock2);
    EXPECT_THAT(transport->setLocalNodeId(0x13), Eq(cetl::nullopt));

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx1", std::move(function));
        }));
    EXPECT_CALL(media_mock2, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx2", std::move(function));
        }));

    auto maybe_session = transport->makeResponseRxSession({64, 0x17B, 0x31});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IResponseRxSession>>(std::move(maybe_session));

    // skip `setFilters` calls; they are tested elsewhere.
    EXPECT_CALL(media_mock_, setFilters(_)).WillOnce(Return(cetl::nullopt));
    EXPECT_CALL(media_mock2, setFilters(_)).WillOnce(Return(cetl::nullopt));

    // 1st run: media #0 pop has failed and there is no transient error handler.
    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, pop(_)).WillOnce(Return(libcyphal::ArgumentError{}));
        scheduler_.scheduleNamedCallback("rx1");
    });
    // 2nd run: media #0 pop and transient error handler have failed.
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        transport->setTransientErrorHandler(std::ref(handler_mock));
        EXPECT_CALL(handler_mock, invoke(VariantWith<MediaPopReport>(Truly([&](auto& report) {
                        EXPECT_THAT(report.failure, VariantWith<libcyphal::ArgumentError>(_));
                        EXPECT_THAT(report.media_index, 0);
                        EXPECT_THAT(report.culprit, Ref(media_mock_));
                        return true;
                    }))))
            .WillOnce(Return(CapacityError{}));

        EXPECT_CALL(media_mock_, pop(_)).WillOnce(Return(libcyphal::ArgumentError{}));
        scheduler_.scheduleNamedCallback("rx1");
    });
    // 3rd run: media #0 pop failed but transient error handler succeeded.
    scheduler_.scheduleAt(3s, [&](const auto&) {
        //
        EXPECT_CALL(handler_mock, invoke(VariantWith<MediaPopReport>(Truly([&](auto& report) {
                        EXPECT_THAT(report.failure, VariantWith<libcyphal::ArgumentError>(_));
                        EXPECT_THAT(report.media_index, 0);
                        EXPECT_THAT(report.culprit, Ref(media_mock_));
                        return true;
                    }))))
            .WillOnce(Return(cetl::nullopt));

        EXPECT_CALL(media_mock_, pop(_)).WillOnce(Return(libcyphal::ArgumentError{}));
        EXPECT_CALL(media_mock2, pop(_)).WillOnce(Return(cetl::nullopt));
        scheduler_.scheduleNamedCallback("rx1");
        scheduler_.scheduleNamedCallback("rx2");
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, receive_svc_responses_with_fallible_oom_canard)
{
    StrictMock<MemoryResourceMock> mr_mock;
    mr_mock.redirectExpectedCallsTo(mr_);

    StrictMock<TransientErrorHandlerMock> handler_mock;

    auto transport = makeTransport(mr_mock);
    EXPECT_THAT(transport->setLocalNodeId(0x13), Eq(cetl::nullopt));

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx", std::move(function));
        }));

    auto maybe_session = transport->makeResponseRxSession({64, 0x17B, 0x31});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IResponseRxSession>>(std::move(maybe_session));

    // Emulate that there is constantly a new frame to receive, but the Canard RX has no memory to accept it.
    //
    EXPECT_CALL(media_mock_, pop(_)).WillRepeatedly([&](auto p) {
        EXPECT_THAT(p.size(), CANARD_MTU_MAX);
        p[0] = b('0');
        p[1] = b('1');
        p[2] = b('2');
        p[3] = b(0b111'11101);
        return IMedia::PopResult::Metadata{now(), 0b111'1'0'0'101111011'0010011'0110001, 4};
    });
    EXPECT_CALL(mr_mock, do_allocate(_, _)).WillRepeatedly(Return(nullptr));
#if (__cplusplus < CETL_CPP_STANDARD_17)
    EXPECT_CALL(mr_mock, do_reallocate(nullptr, 0, _, _)).WillRepeatedly(Return(nullptr));
#endif

    // 1st run: canard RX has failed to accept frame and there is no transient error handler.
    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        scheduler_.scheduleNamedCallback("rx");
        scheduler_.scheduleAt(now() + 1ms, [&](const auto&) {
            //
            EXPECT_THAT(session->receive(), Eq(cetl::nullopt));
        });
    });
    // 2nd run: canard RX has failed to accept frame and there is "failing" transient error handler.
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        transport->setTransientErrorHandler(std::ref(handler_mock));
        scheduler_.scheduleNamedCallback("rx");
        EXPECT_CALL(handler_mock, invoke(_)).WillOnce(Return(libcyphal::ArgumentError{}));
        scheduler_.scheduleAt(now() + 1ms, [&](const auto&) {
            //
            EXPECT_THAT(session->receive(), Eq(cetl::nullopt));
        });
    });
    // 3rd run: canard RX has failed to accept frame and there is "success" transient error handler -
    // the received frame should be just dropped, but overall `run` result should be success (aka ignore OOM).
    scheduler_.scheduleAt(3s, [&](const auto&) {
        //
        EXPECT_CALL(handler_mock, invoke(_)).WillOnce(Return(cetl::nullopt));
        scheduler_.scheduleNamedCallback("rx");
        scheduler_.scheduleAt(now() + 1ms, [&](const auto&) {
            //
            EXPECT_THAT(session->receive(), Eq(cetl::nullopt));
        });
    });
    // 4th run: fix memory problems - now we should receive the payload.
    scheduler_.scheduleAt(4s, [&](const auto&) {
        //
        mr_mock.redirectExpectedCallsTo(mr_);
        scheduler_.scheduleNamedCallback("rx");
        scheduler_.scheduleAt(now() + 1ms, [&](const auto&) {
            //
            EXPECT_THAT(session->receive(), Optional(Truly([](const auto& rx_transfer) {
                            EXPECT_THAT(rx_transfer.metadata.rx_meta.base.transfer_id, 0x1D);
                            EXPECT_THAT(rx_transfer.metadata.rx_meta.base.priority, Priority::Optional);
                            EXPECT_THAT(rx_transfer.metadata.remote_node_id, 0x31);

                            std::array<char, 3> buffer{};
                            EXPECT_THAT(rx_transfer.payload.size(), buffer.size());
                            EXPECT_THAT(rx_transfer.payload.copy(0, buffer.data(), buffer.size()), buffer.size());
                            EXPECT_THAT(buffer, ElementsAre('0', '1', '2'));

                            return true;
                        })));
        });
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, setLocalNodeId_when_msg_rx_subscription)
{
    auto transport = makeTransport(mr_);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx", std::move(function));
        }));

    auto maybe_msg_session = transport->makeMessageRxSession({0, 0x42});
    ASSERT_THAT(maybe_msg_session, VariantWith<UniquePtr<IMessageRxSession>>(NotNull()));

    EXPECT_CALL(media_mock_, setFilters(SizeIs(1))).WillOnce(Return(cetl::nullopt));

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        ASSERT_THAT(transport->setLocalNodeId(0x13), Eq(cetl::nullopt));

        // No `setFilters` expected b/c there is no service RX subscriptions.
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, setLocalNodeId_when_svc_rx_subscription)
{
    auto transport = makeTransport(mr_);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx", std::move(function));
        }));

    auto maybe_msg_session = transport->makeMessageRxSession({0, 0x42});
    ASSERT_THAT(maybe_msg_session, VariantWith<UniquePtr<IMessageRxSession>>(NotNull()));

    auto maybe_svc_session = transport->makeResponseRxSession({64, 0x17B, 0x31});
    ASSERT_THAT(maybe_svc_session, VariantWith<UniquePtr<IResponseRxSession>>(NotNull()));

    EXPECT_CALL(media_mock_, setFilters(SizeIs(1))).WillOnce([&](Filters filters) {
        EXPECT_THAT(filters, Contains(FilterEq({0x4200, 0x21FFF80})));
        return cetl::nullopt;
    });

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        ASSERT_THAT(transport->setLocalNodeId(0x13), Eq(cetl::nullopt));

        EXPECT_CALL(media_mock_, setFilters(SizeIs(2))).WillOnce([&](Filters filters) {
            EXPECT_THAT(filters, Contains(FilterEq({0x4200, 0x21FFF80})));
            EXPECT_THAT(filters, Contains(FilterEq({0x025EC980, 0x02FFFF80})));
            return cetl::nullopt;
        });
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, setFilters_no_memory)
{
    StrictMock<MemoryResourceMock> mr_mock;
    mr_mock.redirectExpectedCallsTo(mr_);

    auto transport = makeTransport(mr_mock);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx", std::move(function));
        }));

    auto maybe_msg_session = transport->makeMessageRxSession({0, 0x42});
    ASSERT_THAT(maybe_msg_session, VariantWith<UniquePtr<IMessageRxSession>>(NotNull()));

    // Emulate that there is no memory for filters.
    //
    EXPECT_CALL(mr_mock, do_allocate(_, _)).WillOnce(Return(nullptr));
#if (__cplusplus < CETL_CPP_STANDARD_17)
    EXPECT_CALL(mr_mock, do_reallocate(nullptr, 0, _, _)).WillOnce(Return(nullptr));
#endif

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        // Restore normal memory operation, but make media fail to accept filters.
        //
        mr_mock.redirectExpectedCallsTo(mr_);
        EXPECT_CALL(media_mock_, setFilters(SizeIs(1))).WillOnce(Return(PlatformError{MyPlatformError{13}}));
        maybe_msg_session = transport->makeMessageRxSession({0, 0x43});
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        EXPECT_CALL(media_mock_, setFilters(SizeIs(1))).WillOnce(Return(cetl::nullopt));
        maybe_msg_session = transport->makeMessageRxSession({0, 0x42});
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestCanTransport, setFilters_with_transient_handler)
{
    using Report = ICanTransport::TransientErrorReport;

    StrictMock<MediaMock> media_mock2{};
    EXPECT_CALL(media_mock2, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
    EXPECT_CALL(media_mock2, getTxMemoryResource()).WillRepeatedly(ReturnRef(tx_mr_));

    auto transport = makeTransport(mr_, &media_mock2);

    EXPECT_CALL(media_mock_, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx1", std::move(function));
        }));
    EXPECT_CALL(media_mock2, registerPopCallback(_))  //
        .WillOnce(Invoke([&](auto function) {         //
            return scheduler_.registerNamedCallback("rx2", std::move(function));
        }));

    auto maybe_msg_session = transport->makeMessageRxSession({0, 0x42});
    ASSERT_THAT(maybe_msg_session, VariantWith<UniquePtr<IMessageRxSession>>(NotNull()));

    const auto expected_error = PlatformError{MyPlatformError{13}};

    transport->setTransientErrorHandler([&](const Report::Variant& report_var) {
        EXPECT_THAT(report_var, VariantWith<Report::MediaConfig>(Truly([&](auto& report) {
                        EXPECT_THAT(report.failure, VariantWith<PlatformError>(_));
                        EXPECT_THAT(report.media_index, 0);
                        EXPECT_THAT(report.culprit, Ref(media_mock_));
                        return true;
                    })));
        return libcyphal::ArgumentError{};
    });
    EXPECT_CALL(media_mock_, setFilters(SizeIs(1))).WillOnce(Return(expected_error));
    EXPECT_CALL(media_mock2, setFilters(SizeIs(1))).WillOnce(Return(cetl::nullopt));

    scheduler_.spinFor(10s);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

}  // namespace
