/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#include "memory_resource_mock.hpp"
#include "tracking_memory_resource.hpp"
#include "verification_utilities.hpp"

#include <canard.h>
#include <cetl/pf17/cetlpf.hpp>
#include <libcyphal/errors.hpp>
#include <libcyphal/transport/can/delegate.hpp>
#include <libcyphal/transport/errors.hpp>
#include <libcyphal/transport/svc_sessions.hpp>
#include <libcyphal/transport/types.hpp>
#include <libcyphal/types.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace
{

using libcyphal::TimePoint;
using libcyphal::MemoryError;
using namespace libcyphal::transport;       // NOLINT This our main concern here in the unit tests.
using namespace libcyphal::transport::can;  // NOLINT This our main concern here in the unit tests.

using cetl::byte;
using libcyphal::verification_utilities::b;
using libcyphal::verification_utilities::fillIotaBytes;

using testing::_;
using testing::Eq;
using testing::Each;
using testing::Return;
using testing::IsNull;
using testing::IsEmpty;
using testing::Optional;
using testing::StrictMock;
using testing::ElementsAre;
using testing::VariantWith;

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

class TestCanDelegate : public testing::Test
{
protected:
    using CanardMemory      = can::detail::CanardMemory;
    using TransportDelegate = can::detail::TransportDelegate;

    class TransportDelegateImpl final : public TransportDelegate
    {
    public:
        explicit TransportDelegateImpl(cetl::pmr::memory_resource& memory)
            : TransportDelegate{memory}
        {
        }

        // MARK: TransportDelegate

        MOCK_METHOD((cetl::optional<AnyFailure>),
                    sendTransfer,
                    (const libcyphal::TimePoint    deadline,
                     const CanardTransferMetadata& metadata,
                     const PayloadFragments        payload_fragments),
                    (override));

        MOCK_METHOD(void, onSessionEvent, (const SessionEvent::Variant& event_var), (noexcept, override));  // NOLINT

        MOCK_METHOD(can::detail::IRxSessionDelegate*,
                    tryFindRxSessionDelegateFor,
                    (const ResponseRxParams& params),
                    (override));
    };

    void SetUp() override
    {
        cetl::pmr::set_default_resource(&mr_);
    }

    void TearDown() override
    {
        EXPECT_THAT(mr_.allocations, IsEmpty());
        EXPECT_THAT(mr_.total_allocated_bytes, mr_.total_deallocated_bytes);
    }

    // MARK: Data members:

    // NOLINTBEGIN
    TrackingMemoryResource mr_;
    // NOLINTEND
};

// MARK: Tests:

TEST_F(TestCanDelegate, CanardMemory_copy)
{
    TransportDelegateImpl delegate{mr_};
    auto&                 canard_instance = delegate.canardInstance();

    constexpr std::size_t payload_size   = 4;
    constexpr std::size_t allocated_size = payload_size + 1;
    auto* const           payload =
        static_cast<byte*>(canard_instance.memory.allocate(static_cast<TransportDelegate*>(&delegate), allocated_size));
    fillIotaBytes({payload, allocated_size}, b('0'));

    CanardMutablePayload canard_payload{payload_size, payload, allocated_size};
    const CanardMemory canard_memory{mr_, canard_payload};
    EXPECT_THAT(canard_memory.size(), payload_size);
    EXPECT_THAT(canard_payload.size, 0);
    EXPECT_THAT(canard_payload.data, nullptr);
    EXPECT_THAT(canard_payload.allocated_size, 0);

    // Ask exactly as payload
    {
        constexpr std::size_t      ask_size = payload_size;
        std::array<byte, ask_size> buffer{};

        EXPECT_THAT(canard_memory.copy(0, buffer.data(), ask_size), ask_size);
        EXPECT_THAT(buffer, ElementsAre(b('0'), b('1'), b('2'), b('3')));
    }

    // Ask more than payload
    {
        constexpr std::size_t      ask_size = payload_size + 2;
        std::array<byte, ask_size> buffer{};

        EXPECT_THAT(canard_memory.copy(0, buffer.data(), ask_size), payload_size);
        EXPECT_THAT(buffer, ElementsAre(b('0'), b('1'), b('2'), b('3'), b('\0'), b('\0')));
    }

    // Ask less than payload (with different offsets)
    {
        constexpr std::size_t      ask_size = payload_size - 2;
        std::array<byte, ask_size> buffer{};

        EXPECT_THAT(canard_memory.copy(0, buffer.data(), ask_size), ask_size);
        EXPECT_THAT(buffer, ElementsAre(b('0'), b('1')));

        EXPECT_THAT(canard_memory.copy(3, buffer.data(), buffer.size()), 1);
        EXPECT_THAT(buffer, ElementsAre(b('3'), b('1')));

        EXPECT_THAT(canard_memory.copy(2, buffer.data(), ask_size), ask_size);
        EXPECT_THAT(buffer, ElementsAre(b('2'), b('3')));

        EXPECT_THAT(canard_memory.copy(payload_size, buffer.data(), ask_size), 0);
        EXPECT_THAT(buffer, ElementsAre(b('2'), b('3')));

        // Ask nothing
        EXPECT_THAT(canard_memory.copy(0, buffer.data(), 0), 0);
        EXPECT_THAT(buffer, ElementsAre(b('2'), b('3')));

        // No output buffer
        EXPECT_THAT(canard_memory.copy(0, nullptr, 0), 0);
    }
}

TEST_F(TestCanDelegate, CanardMemory_copy_on_moved)
{
    TransportDelegateImpl delegate{mr_};
    auto&                 canard_instance = delegate.canardInstance();

    constexpr std::size_t payload_size = 4;
    auto* const           payload =
        static_cast<byte*>(canard_instance.memory.allocate(static_cast<TransportDelegate*>(&delegate), payload_size));
    fillIotaBytes({payload, payload_size}, b('0'));

    CanardMutablePayload canard_payload{payload_size, payload, payload_size};
    CanardMemory old_canard_memory{mr_, canard_payload};
    EXPECT_THAT(old_canard_memory.size(), payload_size);
    EXPECT_THAT(canard_payload.size, 0);
    EXPECT_THAT(canard_payload.data, nullptr);
    EXPECT_THAT(canard_payload.allocated_size, 0);

    const CanardMemory new_canard_memory{std::move(old_canard_memory)};
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move,bugprone-use-after-move,hicpp-invalid-access-moved)
    EXPECT_THAT(old_canard_memory.size(), 0);
    EXPECT_THAT(new_canard_memory.size(), payload_size);

    // Try old one
    {
        std::array<byte, payload_size> buffer{};
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        EXPECT_THAT(old_canard_memory.copy(0, buffer.data(), buffer.size()), 0);
        EXPECT_THAT(buffer, Each(b('\0')));
    }

    // Try a new one
    {
        std::array<byte, payload_size> buffer{};
        EXPECT_THAT(new_canard_memory.copy(0, buffer.data(), buffer.size()), payload_size);
        EXPECT_THAT(buffer, ElementsAre(b('0'), b('1'), b('2'), b('3')));
    }
}

TEST_F(TestCanDelegate, optAnyFailureFromCanard)
{
    EXPECT_THAT(TransportDelegate::optAnyFailureFromCanard(-CANARD_ERROR_OUT_OF_MEMORY),
                Optional(VariantWith<MemoryError>(_)));

    EXPECT_THAT(TransportDelegate::optAnyFailureFromCanard(-CANARD_ERROR_INVALID_ARGUMENT),
                Optional(VariantWith<libcyphal::ArgumentError>(_)));

    EXPECT_THAT(TransportDelegate::optAnyFailureFromCanard(0), Eq(cetl::nullopt));
    EXPECT_THAT(TransportDelegate::optAnyFailureFromCanard(1), Eq(cetl::nullopt));
    EXPECT_THAT(TransportDelegate::optAnyFailureFromCanard(-1), Eq(cetl::nullopt));
}

TEST_F(TestCanDelegate, canardMemoryAllocate_no_memory)
{
    StrictMock<MemoryResourceMock> mr_mock;

    TransportDelegateImpl delegate{mr_mock};
    const auto&           canard_instance = delegate.canardInstance();

    // Emulate that there is no memory at all.
    EXPECT_CALL(mr_mock, do_allocate(_, _))  //
        .WillOnce(Return(nullptr));

    EXPECT_THAT(canard_instance.memory.allocate(static_cast<TransportDelegate*>(&delegate), 1), IsNull());
}

TEST_F(TestCanDelegate, CanardConcreteTree_visitCounting)
{
    struct MyNode final : public CanardTreeNode
    {
        explicit MyNode(std::string _name)
            : CanardTreeNode{}
            , name{std::move(_name)}
        {
        }
        std::string name;  // NOLINT
    };
    //        Root
    //      ↙     ↘
    //  Left       Right
    //      ↘      ↙   ↘
    //       LR   RL    RR
    //                 ↙
    //               RRL
    //
    // NOLINTBEGIN(readability-isolate-declaration)
    MyNode root{"Root"}, left{"Left"}, right{"Right"};
    MyNode left_r{"LR"}, right_l{"RL"}, right_r{"RR"}, right_rl{"RRL"};
    // NOLINTEND(readability-isolate-declaration)
    root.lr[0]  = &left;
    root.lr[1]  = &right;
    left_r.up   = &left;
    left.lr[1]  = &left_r;
    right.lr[0] = &right_l;
    right.lr[1] = &right_r;
    left.up = right.up = &root;
    right_l.up = right_r.up = &right;
    right_rl.up             = &right_r;
    right_r.lr[0]           = &right_rl;

    using MyTree = TransportDelegate::CanardConcreteTree<const MyNode>;
    {
        std::vector<std::string> names;
        auto count = MyTree::visitCounting(&root, [&names](const MyNode& node) { names.push_back(node.name); });
        EXPECT_THAT(count, 7);
        EXPECT_THAT(names, ElementsAre("Left", "LR", "Root", "RL", "Right", "RRL", "RR"));
    }
    {
        std::vector<std::string> names;
        auto count = MyTree::visitCounting(nullptr, [&names](const MyNode& node) { names.push_back(node.name); });
        EXPECT_THAT(count, 0);
        EXPECT_THAT(names, IsEmpty());
    }
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

}  // namespace
