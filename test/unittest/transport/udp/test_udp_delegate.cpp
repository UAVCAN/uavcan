/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#include "memory_resource_mock.hpp"
#include "tracking_memory_resource.hpp"
#include "verification_utilities.hpp"

#include <cetl/pf17/cetlpf.hpp>
#include <libcyphal/errors.hpp>
#include <libcyphal/transport/errors.hpp>
#include <libcyphal/transport/svc_sessions.hpp>
#include <libcyphal/transport/types.hpp>
#include <libcyphal/transport/udp/delegate.hpp>
#include <libcyphal/types.hpp>
#include <udpard.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace
{

using libcyphal::TimePoint;
using libcyphal::MemoryError;
using namespace libcyphal::transport;       // NOLINT This our main concern here in the unit tests.
using namespace libcyphal::transport::udp;  // NOLINT This our main concern here in the unit tests.

using cetl::byte;
using libcyphal::verification_utilities::b;
using libcyphal::verification_utilities::fillIotaBytes;

using testing::_;
using testing::Eq;
using testing::Each;
using testing::Return;
using testing::IsNull;
using testing::NotNull;
using testing::IsEmpty;
using testing::Optional;
using testing::StrictMock;
using testing::ElementsAre;
using testing::VariantWith;

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)

class TestUdpDelegate : public testing::Test
{
protected:
    using UdpardMemory      = udp::detail::UdpardMemory;
    using MemoryResources   = udp::detail::MemoryResources;
    using TransportDelegate = udp::detail::TransportDelegate;

    class TransportDelegateImpl final : public TransportDelegate
    {
    public:
        using TransportDelegate::memoryResources;
        using TransportDelegate::makeUdpardMemoryDeleter;
        using TransportDelegate::makeUdpardMemoryResource;

        explicit TransportDelegateImpl(cetl::pmr::memory_resource& general_mr,
                                       cetl::pmr::memory_resource* fragment_mr = nullptr,
                                       cetl::pmr::memory_resource* payload_mr  = nullptr)
            : TransportDelegate{MemoryResources{general_mr,
                                                makeUdpardMemoryResource(nullptr, general_mr),
                                                makeUdpardMemoryResource(fragment_mr, general_mr),
                                                makeUdpardMemoryDeleter(payload_mr, general_mr)}}
        {
        }

        // MARK: TransportDelegate

        MOCK_METHOD((cetl::optional<AnyFailure>),
                    sendAnyTransfer,
                    (const udp::detail::AnyUdpardTxMetadata::Variant& tx_metadata_var,
                     const PayloadFragments                           payload_fragments),
                    (override));

        MOCK_METHOD(void, onSessionEvent, (const SessionEvent::Variant& event_var), (noexcept, override));  // NOLINT

        MOCK_METHOD(udp::detail::IRxSessionDelegate*,
                    tryFindRxSessionDelegateFor,
                    (const ResponseRxParams& params),
                    (override));

    };  // TransportDelegateImpl

    void SetUp() override
    {
        cetl::pmr::set_default_resource(&general_mr_);
    }

    void TearDown() override
    {
        EXPECT_THAT(general_mr_.allocations, IsEmpty());
        EXPECT_THAT(general_mr_.total_allocated_bytes, general_mr_.total_deallocated_bytes);

        EXPECT_THAT(fragment_mr_.allocations, IsEmpty());
        EXPECT_THAT(fragment_mr_.total_allocated_bytes, fragment_mr_.total_deallocated_bytes);

        EXPECT_THAT(payload_mr_.allocations, IsEmpty());
        EXPECT_THAT(payload_mr_.total_allocated_bytes, payload_mr_.total_deallocated_bytes);
    }

    byte* allocateNewUdpardPayload(const std::size_t size)
    {
        return static_cast<byte*>(payload_mr_.allocate(size));
    }

    UdpardFragment* allocateNewUdpardFragment(const std::size_t size)
    {
        // This structure mimics internal Udpard `RxFragment` layout.
        // We need this to know its size, so that test teardown can check if all memory was deallocated.
        // @see `EXPECT_THAT(fragment_mr_.total_allocated_bytes, fragment_mr_.total_deallocated_bytes);`
        //
        struct RxFragment
        {
            struct UdpardFragment base;
            struct RxFragmentTreeNode
            {
                UdpardTreeNode     base;
                struct RxFragment* this_;
            } tree;
            std::uint32_t frame_index;
        };

        auto* const payload = allocateNewUdpardPayload(size);
        auto* const rx_frag = static_cast<RxFragment*>(fragment_mr_.allocate(sizeof(RxFragment)));
        rx_frag->base       = UdpardFragment{nullptr, {size, payload}, {size, payload}};
        return &rx_frag->base;
    }

    // MARK: Data members:

    // NOLINTBEGIN
    TrackingMemoryResource general_mr_;
    TrackingMemoryResource fragment_mr_;
    TrackingMemoryResource payload_mr_;
    // NOLINTEND
};

// MARK: - Tests:

TEST_F(TestUdpDelegate, UdpardMemory_copy)
{
    const TransportDelegateImpl delegate{general_mr_, &fragment_mr_, &payload_mr_};

    auto* const payload = allocateNewUdpardPayload(4);
    fillIotaBytes({payload, 4}, b('0'));

    constexpr std::size_t payload_size = 4;
    UdpardRxTransfer      rx_transfer{};
    rx_transfer.payload_size = payload_size;
    rx_transfer.payload      = UdpardFragment{nullptr, {payload_size, payload}, {payload_size, payload}};

    const UdpardMemory udpard_memory{delegate.memoryResources(), rx_transfer};
    EXPECT_THAT(udpard_memory.size(), payload_size);
    EXPECT_THAT(rx_transfer.payload_size, 0);
    EXPECT_THAT(rx_transfer.payload.next, nullptr);
    EXPECT_THAT(rx_transfer.payload.view.size, 0);
    EXPECT_THAT(rx_transfer.payload.view.data, nullptr);
    EXPECT_THAT(rx_transfer.payload.origin.size, 0);
    EXPECT_THAT(rx_transfer.payload.origin.data, nullptr);

    // Ask exactly as payload
    {
        constexpr std::size_t      ask_size = payload_size;
        std::array<byte, ask_size> buffer{};

        EXPECT_THAT(udpard_memory.copy(0, buffer.data(), ask_size), ask_size);
        EXPECT_THAT(buffer, ElementsAre(b('0'), b('1'), b('2'), b('3')));
    }

    // Ask more than payload
    {
        constexpr std::size_t      ask_size = payload_size + 2;
        std::array<byte, ask_size> buffer{};

        EXPECT_THAT(udpard_memory.copy(0, buffer.data(), ask_size), payload_size);
        EXPECT_THAT(buffer, ElementsAre(b('0'), b('1'), b('2'), b('3'), b('\0'), b('\0')));
    }

    // Ask less than payload (with different offsets)
    {
        constexpr std::size_t      ask_size = payload_size - 2;
        std::array<byte, ask_size> buffer{};

        EXPECT_THAT(udpard_memory.copy(0, buffer.data(), ask_size), ask_size);
        EXPECT_THAT(buffer, ElementsAre(b('0'), b('1')));

        EXPECT_THAT(udpard_memory.copy(3, buffer.data(), buffer.size()), 1);
        EXPECT_THAT(buffer, ElementsAre(b('3'), b('1')));

        EXPECT_THAT(udpard_memory.copy(2, buffer.data(), ask_size), ask_size);
        EXPECT_THAT(buffer, ElementsAre(b('2'), b('3')));

        EXPECT_THAT(udpard_memory.copy(payload_size, buffer.data(), ask_size), 0);
        EXPECT_THAT(buffer, ElementsAre(b('2'), b('3')));

        // Ask nothing
        EXPECT_THAT(udpard_memory.copy(0, buffer.data(), 0), 0);
        EXPECT_THAT(buffer, ElementsAre(b('2'), b('3')));

        // No output buffer
        EXPECT_THAT(udpard_memory.copy(0, nullptr, 0), 0);
    }
}

TEST_F(TestUdpDelegate, UdpardMemory_copy_on_moved)
{
    const TransportDelegateImpl delegate{general_mr_, &fragment_mr_, &payload_mr_};

    constexpr std::size_t payload_size = 4;
    auto* const           payload      = allocateNewUdpardPayload(payload_size);
    fillIotaBytes({payload, payload_size}, b('0'));

    UdpardRxTransfer rx_transfer{};
    rx_transfer.payload_size = payload_size;
    rx_transfer.payload      = UdpardFragment{nullptr, {payload_size, payload}, {payload_size, payload}};

    UdpardMemory old_udpard_memory{delegate.memoryResources(), rx_transfer};
    EXPECT_THAT(old_udpard_memory.size(), payload_size);

    const UdpardMemory new_udpard_memory{std::move(old_udpard_memory)};
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move,bugprone-use-after-move,hicpp-invalid-access-moved)
    EXPECT_THAT(old_udpard_memory.size(), 0);
    EXPECT_THAT(new_udpard_memory.size(), payload_size);

    // Try old one
    {
        std::array<byte, payload_size> buffer{};
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        EXPECT_THAT(old_udpard_memory.copy(0, buffer.data(), buffer.size()), 0);
        EXPECT_THAT(buffer, Each(b('\0')));
    }

    // Try a new one
    {
        std::array<byte, payload_size> buffer{};
        EXPECT_THAT(new_udpard_memory.copy(0, buffer.data(), buffer.size()), payload_size);
        EXPECT_THAT(buffer, ElementsAre(b('0'), b('1'), b('2'), b('3')));
    }
}

TEST_F(TestUdpDelegate, UdpardMemory_copy_multi_fragmented)
{
    const TransportDelegateImpl delegate{general_mr_, &fragment_mr_, &payload_mr_};

    auto* const payload0 = allocateNewUdpardPayload(7);

    UdpardRxTransfer rx_transfer{};
    rx_transfer.payload            = UdpardFragment{nullptr, {7, payload0}, {7, payload0}};
    rx_transfer.payload.next       = allocateNewUdpardFragment(8);
    rx_transfer.payload.next->next = allocateNewUdpardFragment(9);

    auto* const payload1 = static_cast<byte*>(rx_transfer.payload.next->origin.data);
    auto* const payload2 = static_cast<byte*>(rx_transfer.payload.next->next->origin.data);
    fillIotaBytes({payload0, 7}, b('0'));
    fillIotaBytes({payload1, 8}, b('A'));
    fillIotaBytes({payload2, 9}, b('a'));

    constexpr std::size_t payload_size   = 3 + 4 + 2;
    rx_transfer.payload_size             = payload_size;
    rx_transfer.payload.view             = {3, payload0 + 2};
    rx_transfer.payload.next->view       = {4, payload1 + 1};
    rx_transfer.payload.next->next->view = {2, payload2 + 3};

    const UdpardMemory udpard_memory{delegate.memoryResources(), rx_transfer};
    EXPECT_THAT(udpard_memory.size(), payload_size);

    // Ask exactly as payload
    {
        constexpr std::size_t      ask_size = payload_size;
        std::array<byte, ask_size> buffer{};

        EXPECT_THAT(udpard_memory.copy(0, buffer.data(), ask_size), ask_size);
        EXPECT_THAT(buffer, ElementsAre(b('2'), b('3'), b('4'), b('B'), b('C'), b('D'), b('E'), b('d'), b('e')));
    }

    // Ask more than payload
    {
        constexpr std::size_t      ask_size = payload_size + 2;
        std::array<byte, ask_size> buffer{};

        EXPECT_THAT(udpard_memory.copy(0, buffer.data(), ask_size), payload_size);
        EXPECT_THAT(buffer,
                    ElementsAre(b('2'), b('3'), b('4'), b('B'), b('C'), b('D'), b('E'), b('d'), b('e'), b(0), b(0)));
    }

    // Ask less than payload (with different offsets)
    {
        constexpr std::size_t      ask_size = payload_size - 2;
        std::array<byte, ask_size> buffer{};

        EXPECT_THAT(udpard_memory.copy(0, buffer.data(), ask_size), ask_size);
        EXPECT_THAT(buffer, ElementsAre(b('2'), b('3'), b('4'), b('B'), b('C'), b('D'), b('E')));

        EXPECT_THAT(udpard_memory.copy(3, buffer.data(), buffer.size()), 6);
        EXPECT_THAT(buffer, ElementsAre(b('B'), b('C'), b('D'), b('E'), b('d'), b('e'), b('E')));

        EXPECT_THAT(udpard_memory.copy(2, buffer.data(), ask_size), ask_size);
        EXPECT_THAT(buffer, ElementsAre(b('4'), b('B'), b('C'), b('D'), b('E'), b('d'), b('e')));

        EXPECT_THAT(udpard_memory.copy(4, buffer.data(), ask_size), 5);
        EXPECT_THAT(buffer, ElementsAre(b('C'), b('D'), b('E'), b('d'), b('e'), b('d'), b('e')));

        EXPECT_THAT(udpard_memory.copy(payload_size, buffer.data(), ask_size), 0);
        EXPECT_THAT(buffer, ElementsAre(b('C'), b('D'), b('E'), b('d'), b('e'), b('d'), b('e')));

        // Ask nothing
        EXPECT_THAT(udpard_memory.copy(0, buffer.data(), 0), 0);
        EXPECT_THAT(buffer, ElementsAre(b('C'), b('D'), b('E'), b('d'), b('e'), b('d'), b('e')));

        // No output buffer
        EXPECT_THAT(udpard_memory.copy(0, nullptr, 0), 0);
    }
}

TEST_F(TestUdpDelegate, UdpardMemory_copy_empty)
{
    const TransportDelegateImpl delegate{general_mr_, &fragment_mr_, &payload_mr_};

    UdpardRxTransfer rx_transfer{};
    rx_transfer.payload_size = 0;
    rx_transfer.payload      = UdpardFragment{nullptr, {0, nullptr}, {0, nullptr}};

    const UdpardMemory udpard_memory{delegate.memoryResources(), rx_transfer};
    EXPECT_THAT(udpard_memory.size(), 0);

    std::array<byte, 3> buffer{};
    EXPECT_THAT(udpard_memory.copy(0, buffer.data(), 3), 0);
    EXPECT_THAT(buffer, Each(b('\0')));
    EXPECT_THAT(udpard_memory.copy(1, buffer.data(), 3), 0);
}

TEST_F(TestUdpDelegate, optAnyFailureFromUdpard)
{
    EXPECT_THAT(TransportDelegate::optAnyFailureFromUdpard(-UDPARD_ERROR_MEMORY),
                Optional(VariantWith<MemoryError>(_)));

    EXPECT_THAT(TransportDelegate::optAnyFailureFromUdpard(-UDPARD_ERROR_ARGUMENT),
                Optional(VariantWith<libcyphal::ArgumentError>(_)));

    EXPECT_THAT(TransportDelegate::optAnyFailureFromUdpard(-UDPARD_ERROR_CAPACITY),
                Optional(VariantWith<CapacityError>(_)));

    EXPECT_THAT(TransportDelegate::optAnyFailureFromUdpard(-UDPARD_ERROR_ANONYMOUS),
                Optional(VariantWith<AnonymousError>(_)));

    EXPECT_THAT(TransportDelegate::optAnyFailureFromUdpard(0), Eq(cetl::nullopt));
    EXPECT_THAT(TransportDelegate::optAnyFailureFromUdpard(1), Eq(cetl::nullopt));
    EXPECT_THAT(TransportDelegate::optAnyFailureFromUdpard(-1), Eq(cetl::nullopt));
}

TEST_F(TestUdpDelegate, makeUdpardMemoryResource)
{
    const auto udp_mem_res1 = TransportDelegateImpl::makeUdpardMemoryResource(nullptr, general_mr_);
    EXPECT_THAT(udp_mem_res1.user_reference, &general_mr_);
    EXPECT_THAT(udp_mem_res1.allocate, NotNull());
    EXPECT_THAT(udp_mem_res1.deallocate, NotNull());

    StrictMock<MemoryResourceMock> mr_mock;

    const auto udp_mem_res2 = TransportDelegateImpl::makeUdpardMemoryResource(&mr_mock, general_mr_);
    EXPECT_THAT(udp_mem_res2.user_reference, &mr_mock);
    EXPECT_THAT(udp_mem_res2.allocate, NotNull());
    EXPECT_THAT(udp_mem_res2.deallocate, NotNull());
}

TEST_F(TestUdpDelegate, makeUdpardMemoryDeleter)
{
    const auto udp_mr_del1 = TransportDelegateImpl::makeUdpardMemoryDeleter(nullptr, general_mr_);
    EXPECT_THAT(udp_mr_del1.user_reference, &general_mr_);
    EXPECT_THAT(udp_mr_del1.deallocate, NotNull());

    StrictMock<MemoryResourceMock> mr_mock;

    const auto udp_mr_del2 = TransportDelegateImpl::makeUdpardMemoryDeleter(&mr_mock, general_mr_);
    EXPECT_THAT(udp_mr_del2.user_reference, &mr_mock);
    EXPECT_THAT(udp_mr_del2.deallocate, NotNull());
}

TEST_F(TestUdpDelegate, allocateMemoryForUdpard_deallocateMemoryForUdpard)
{
    StrictMock<MemoryResourceMock> mr_mock;
    mr_mock.redirectExpectedCallsTo(general_mr_);

    const TransportDelegateImpl delegate{mr_mock};

    const auto& fragment_mr = delegate.memoryResources().fragment;

    auto* mem_ptr = fragment_mr.allocate(fragment_mr.user_reference, 1);
    EXPECT_THAT(mem_ptr, NotNull());

    fragment_mr.deallocate(fragment_mr.user_reference, 1, mem_ptr);
}

TEST_F(TestUdpDelegate, allocateMemoryForUdpard_no_memory)
{
    StrictMock<MemoryResourceMock> mr_mock;

    const TransportDelegateImpl delegate{mr_mock};

    // Emulate that there is no memory at all.
    EXPECT_CALL(mr_mock, do_allocate(1, _))  //
        .WillOnce(Return(nullptr));

    const auto& session_mr = delegate.memoryResources().session;
    EXPECT_THAT(session_mr.allocate(session_mr.user_reference, 1), IsNull());
}

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

}  // namespace
