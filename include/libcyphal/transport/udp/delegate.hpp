/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_UDP_DELEGATE_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_UDP_DELEGATE_HPP_INCLUDED

#include "rx_session_tree_node.hpp"

#include "libcyphal/transport/errors.hpp"
#include "libcyphal/transport/msg_sessions.hpp"
#include "libcyphal/transport/scattered_buffer.hpp"
#include "libcyphal/transport/session_tree.hpp"
#include "libcyphal/transport/svc_sessions.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/transport/udp/tx_rx_sockets.hpp"

#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>
#include <cetl/pf20/cetlpf.hpp>
#include <udpard.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <tuple>
#include <utility>

namespace libcyphal
{
namespace transport
{
namespace udp
{

/// Internal implementation details of the UDP transport.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

struct AnyUdpardTxMetadata
{
    struct Publish
    {
        UdpardMicrosecond deadline_us;
        UdpardPriority    priority;
        UdpardPortID      subject_id;
        UdpardTransferID  transfer_id;
    };
    struct Request
    {
        UdpardMicrosecond deadline_us;
        UdpardPriority    priority;
        UdpardPortID      service_id;
        UdpardNodeID      server_node_id;
        UdpardTransferID  transfer_id;
    };
    struct Respond
    {
        UdpardMicrosecond deadline_us;
        UdpardPriority    priority;
        UdpardPortID      service_id;
        UdpardNodeID      client_node_id;
        UdpardTransferID  transfer_id;
    };

    /// Defines variant of all possible transient error reports.
    ///
    using Variant = cetl::variant<Publish, Request, Respond>;

};  // AnyUdpardTxMetadata

// MARK: -

/// @brief Defines an internal set of memory resources used by the UDP transport.
///
struct MemoryResources
{
    /// The general purpose memory resource is used to provide memory for the libcyphal library.
    /// It is NOT used for any Udpard TX or RX transfers, payload (de)fragmentation or transient handles,
    /// but only for the libcyphal internal needs (like `make*[Rx|Tx]Session` factory calls).
    cetl::pmr::memory_resource& general;

    /// The session memory resource is used to provide memory for the Udpard session instances.
    /// Each instance is fixed-size, so a trivial zero-fragmentation block allocator is enough.
    UdpardMemoryResource session;

    /// The fragment handles are allocated per payload fragment; each handle contains a pointer to its fragment.
    /// Each instance is of a very small fixed size, so a trivial zero-fragmentation block allocator is enough.
    UdpardMemoryResource fragment;

    /// The library never allocates payload buffers itself, as they are handed over by the application via
    /// reception calls. Once a buffer is handed over, the library may choose to keep it if it is deemed to be
    /// necessary to complete a transfer reassembly, or to discard it if it is deemed to be unnecessary.
    /// Discarded payload buffers are freed using this memory resource.
    UdpardMemoryDeleter payload;
};

/// @brief RAII class to manage memory allocated by Udpard library.
///
class UdpardMemory final : public ScatteredBuffer::IStorage
{
public:
    UdpardMemory(const MemoryResources& memory_resources, UdpardRxTransfer& transfer)
        : memory_resources_{memory_resources}
        , payload_size_{std::exchange(transfer.payload_size, 0)}
        , payload_{std::exchange(transfer.payload, {})}
    {
    }
    UdpardMemory(UdpardMemory&& other) noexcept
        : memory_resources_{other.memory_resources_}
        , payload_size_{std::exchange(other.payload_size_, 0)}
        , payload_{std::exchange(other.payload_, {})}
    {
    }

    ~UdpardMemory()
    {
        ::udpardRxFragmentFree(payload_, memory_resources_.fragment, memory_resources_.payload);
    }

    UdpardMemory(const UdpardMemory&)                = delete;
    UdpardMemory& operator=(const UdpardMemory&)     = delete;
    UdpardMemory& operator=(UdpardMemory&&) noexcept = delete;

    // MARK: ScatteredBuffer::IStorage

    CETL_NODISCARD std::size_t size() const noexcept override
    {
        return payload_size_;
    }

    CETL_NODISCARD std::size_t copy(const std::size_t offset_bytes,
                                    cetl::byte* const destination,
                                    const std::size_t length_bytes) const override
    {
        using FragSpan = const cetl::span<const cetl::byte>;

        // TODO: Use `udpardGather` function when it will be available with offset support.

        CETL_DEBUG_ASSERT((destination != nullptr) || (length_bytes == 0),
                          "Destination could be null only with zero bytes ask.");

        if ((destination == nullptr) || (payload_.view.data == nullptr) || (payload_size_ <= offset_bytes))
        {
            return 0;
        }

        // Find first fragment to start from (according to source `offset_bytes`).
        //
        std::size_t                  src_offset = 0;
        const struct UdpardFragment* frag       = &payload_;
        while ((nullptr != frag) && (offset_bytes >= (src_offset + frag->view.size)))
        {
            src_offset += frag->view.size;
            frag = frag->next;
        }

        std::size_t dst_offset         = 0;
        std::size_t total_bytes_copied = 0;

        CETL_DEBUG_ASSERT(offset_bytes >= src_offset, "");
        std::size_t view_offset = offset_bytes - src_offset;

        while ((nullptr != frag) && (dst_offset < length_bytes))
        {
            CETL_DEBUG_ASSERT(nullptr != frag->view.data, "");
            // Next nolint-s are unavoidable: we need offset from the beginning of the buffer.
            // No Sonar `cpp:S5356` b/c we integrate here with libcanard raw C buffers.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            FragSpan frag_span{static_cast<const cetl::byte*>(frag->view.data) + view_offset,  // NOSONAR cpp:S5356
                               std::min(frag->view.size - view_offset, length_bytes - dst_offset)};
            CETL_DEBUG_ASSERT(frag_span.size() <= (frag->view.size - view_offset), "");

            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            (void) std::memmove(destination + dst_offset, frag_span.data(), frag_span.size());  // NOSONAR cpp:S5356

            src_offset += frag_span.size();
            dst_offset += frag_span.size();
            total_bytes_copied += frag_span.size();
            CETL_DEBUG_ASSERT(dst_offset <= length_bytes, "");
            frag        = frag->next;
            view_offset = 0;
        }

        return total_bytes_copied;
    }

private:
    // MARK: Data members:

    const MemoryResources& memory_resources_;
    std::size_t            payload_size_;
    UdpardFragment         payload_;

};  // UdpardMemory

// MARK: -

/// This internal session delegate class serves the following purpose: it provides an interface (aka gateway)
/// to access RX session from transport (by casting udpard `user_reference` member to this class).
///
class IRxSessionDelegate
{
public:
    IRxSessionDelegate(const IRxSessionDelegate&)                = delete;
    IRxSessionDelegate(IRxSessionDelegate&&) noexcept            = delete;
    IRxSessionDelegate& operator=(const IRxSessionDelegate&)     = delete;
    IRxSessionDelegate& operator=(IRxSessionDelegate&&) noexcept = delete;

    /// @brief Accepts a received transfer from the transport dedicated to this RX session.
    ///
    virtual void acceptRxTransfer(UdpardMemory&&            udpard_memory,
                                  const TransferRxMetadata& rx_metadata,
                                  const NodeId              source_node_id) = 0;

protected:
    IRxSessionDelegate()  = default;
    ~IRxSessionDelegate() = default;

};  // IRxSessionDelegate

/// This internal session delegate class serves the following purpose:
/// it provides an interface (aka gateway) to access Message RX session from transport.
///
class IMsgRxSessionDelegate : public IRxSessionDelegate
{
public:
    IMsgRxSessionDelegate(const IMsgRxSessionDelegate&)                = delete;
    IMsgRxSessionDelegate(IMsgRxSessionDelegate&&) noexcept            = delete;
    IMsgRxSessionDelegate& operator=(const IMsgRxSessionDelegate&)     = delete;
    IMsgRxSessionDelegate& operator=(IMsgRxSessionDelegate&&) noexcept = delete;

    CETL_NODISCARD virtual UdpardRxSubscription& getSubscription() = 0;

protected:
    IMsgRxSessionDelegate()  = default;
    ~IMsgRxSessionDelegate() = default;

};  // IMsgRxSessionDelegate

// MARK: -

/// This internal transport delegate class serves the following purposes:
/// 1. It provides memory management functions for the Udpard library.
/// 2. It provides a way to convert Udpard error codes to `AnyFailure` type.
/// 3. It provides an interface to access the transport from various session classes.
///
class TransportDelegate
{
public:
    /// Umbrella type for all session-related events.
    ///
    /// These are passed to the `onSessionEvent` method of the transport implementation.
    ///
    struct SessionEvent
    {
        struct MsgDestroyed
        {
            MessageRxParams params;
        };

        struct SvcRequestDestroyed
        {
            RequestRxParams params;
        };

        struct SvcResponseDestroyed
        {
            ResponseRxParams params;
        };

        using Variant = cetl::variant<MsgDestroyed, SvcRequestDestroyed, SvcResponseDestroyed>;

    };  // SessionEvent

    TransportDelegate(const TransportDelegate&)                = delete;
    TransportDelegate(TransportDelegate&&) noexcept            = delete;
    TransportDelegate& operator=(const TransportDelegate&)     = delete;
    TransportDelegate& operator=(TransportDelegate&&) noexcept = delete;

    CETL_NODISCARD const NodeId& getNodeId() const noexcept
    {
        return udpard_node_id_;
    }

    CETL_NODISCARD IpEndpoint setNodeId(const NodeId node_id) noexcept
    {
        udpard_node_id_ = node_id;

        UdpardUDPIPEndpoint endpoint{};
        const std::int8_t   result = ::udpardRxRPCDispatcherStart(&rx_rpc_dispatcher_, node_id, &endpoint);
        (void) result;
        CETL_DEBUG_ASSERT(result == 0, "There is no way currently to get an error here.");

        return IpEndpoint::fromUdpardEndpoint(endpoint);
    }

    CETL_NODISCARD UdpardRxRPCDispatcher& getUdpardRpcDispatcher() noexcept
    {
        return rx_rpc_dispatcher_;
    }

    template <bool IsRequest, typename Params>
    void listenForRxRpcPort(UdpardRxRPCPort& rpc_port, const Params& params)
    {
        const std::int8_t result = ::udpardRxRPCDispatcherListen(&rx_rpc_dispatcher_,
                                                                 &rpc_port,
                                                                 params.service_id,
                                                                 IsRequest,
                                                                 params.extent_bytes);
        (void) result;
        CETL_DEBUG_ASSERT(result >= 0, "There is no way currently to get an error here.");
        CETL_DEBUG_ASSERT(result == 1, "A new registration has been expected to be created.");
    }

    void retainRxRpcPortFor(const ResponseRxParams& params)
    {
        const auto maybe_node = rx_rpc_port_demux_nodes_.ensureNodeFor(params, std::ref(*this));
        if (const auto* const node = cetl::get_if<RxRpcPortDemuxNode::RefWrapper>(&maybe_node))
        {
            node->get().retain();
        }
    }

    CETL_NODISCARD UdpardRxRPCPort* findRxRpcPortFor(const ResponseRxParams& params)
    {
        if (auto* const node = rx_rpc_port_demux_nodes_.tryFindNodeFor(params))
        {
            return &node->port();
        }
        return nullptr;
    }

    void releaseRxRpcPortFor(const ResponseRxParams& params) noexcept
    {
        if (auto* const node = rx_rpc_port_demux_nodes_.tryFindNodeFor(params))
        {
            if (node->release())
            {
                rx_rpc_port_demux_nodes_.removeNodeFor(params);
            }
        }
    }

    void cancelRxRpcPortFor(const UdpardRxRPCPort& rpc_port_, const bool is_request) noexcept
    {
        const std::int8_t result = ::udpardRxRPCDispatcherCancel(&rx_rpc_dispatcher_,
                                                                 rpc_port_.service_id,
                                                                 is_request);  // request
        (void) result;
        CETL_DEBUG_ASSERT(result >= 0, "There is no way currently to get an error here.");
        CETL_DEBUG_ASSERT(result == 1, "Existing registration has been expected to be cancelled.");
    }

    CETL_NODISCARD static cetl::optional<AnyFailure> optAnyFailureFromUdpard(const std::int32_t result)
    {
        // Udpard error results are negative, so we need to negate them to get the error code.
        const std::int32_t udpard_error = -result;

        if (udpard_error == UDPARD_ERROR_ARGUMENT)
        {
            return ArgumentError{};
        }
        if (udpard_error == UDPARD_ERROR_MEMORY)
        {
            return MemoryError{};
        }
        if (udpard_error == UDPARD_ERROR_CAPACITY)
        {
            return CapacityError{};
        }
        if (udpard_error == UDPARD_ERROR_ANONYMOUS)
        {
            return AnonymousError{};
        }

        return cetl::nullopt;
    }

    CETL_NODISCARD UdpardRxMemoryResources makeUdpardRxMemoryResources() const
    {
        return {memoryResources().session, memoryResources().fragment, memoryResources().payload};
    }

    /// Pops and frees Udpard TX queue item(s).
    ///
    /// @param tx_queue The TX queue from which the item should be popped.
    /// @param tx_item The TX queue item to be popped and freed.
    /// @param whole_transfer If `true` then whole transfer should be released from the queue.
    ///
    static void popAndFreeUdpardTxItem(UdpardTx* const tx_queue, UdpardTxItem* tx_item, const bool whole_transfer)
    {
        while (UdpardTxItem* const mut_tx_item = ::udpardTxPop(tx_queue, tx_item))
        {
            tx_item = tx_item->next_in_transfer;

            ::udpardTxFree(tx_queue->memory, mut_tx_item);

            if (!whole_transfer)
            {
                break;
            }
        }
    }

    /// @brief Sends transfer to each media udpard TX queue of the transport.
    ///
    /// Internal method which is in use by TX session implementations to delegate actual sending to transport.
    ///
    CETL_NODISCARD virtual cetl::optional<AnyFailure> sendAnyTransfer(
        const AnyUdpardTxMetadata::Variant& tx_metadata_var,
        const PayloadFragments              payload_fragments) = 0;

    /// @brief Called on a session event.
    ///
    /// @param event_var Describes variant of the session even has happened.
    ///
    virtual void onSessionEvent(const SessionEvent::Variant& event_var) noexcept = 0;

    /// @brief Tries to find a response RX session delegate for the given parameters.
    ///
    /// @return `nullptr` if no session delegate found for the given parameters.
    ///
    virtual IRxSessionDelegate* tryFindRxSessionDelegateFor(const ResponseRxParams& params) = 0;

protected:
    explicit TransportDelegate(const MemoryResources& memory_resources)
        : udpard_node_id_{UDPARD_NODE_ID_UNSET}
        , memory_resources_{memory_resources}
        , rx_rpc_dispatcher_{}
        , rx_rpc_port_demux_nodes_{memory_resources.general}
    {
        const std::int8_t result = ::udpardRxRPCDispatcherInit(&rx_rpc_dispatcher_, makeUdpardRxMemoryResources());
        (void) result;
        CETL_DEBUG_ASSERT(result == 0, "There is no way currently to get an error here.");
    }

    ~TransportDelegate() = default;

    CETL_NODISCARD const MemoryResources& memoryResources() const noexcept
    {
        return memory_resources_;
    }

    CETL_NODISCARD static UdpardMemoryResource makeUdpardMemoryResource(cetl::pmr::memory_resource* const custom,
                                                                        cetl::pmr::memory_resource&       general)
    {
        // No Sonar `cpp:S5356` b/c the raw `user_reference` is part of libudpard api.
        void* const user_reference = (custom != nullptr) ? custom : &general;  // NOSONAR cpp:S5356
        return {user_reference, deallocateMemoryForUdpard, allocateMemoryForUdpard};
    }

    CETL_NODISCARD static UdpardMemoryDeleter makeUdpardMemoryDeleter(cetl::pmr::memory_resource* const custom,
                                                                      cetl::pmr::memory_resource&       general)
    {
        // No Sonar `cpp:S5356` b/c the raw `user_reference` is part of libudpard api.
        void* const user_reference = (custom != nullptr) ? custom : &general;  // NOSONAR cpp:S5356
        return UdpardMemoryDeleter{user_reference, deallocateMemoryForUdpard};
    }

private:
    template <typename Node>
    using SessionTree = transport::detail::SessionTree<Node>;

    /// Accepts transfers from RX RPC port and forwards them to the appropriate session (according to source node id).
    /// Has reference counting so that it will be destroyed when no longer referenced by any RX session.
    ///
    class RxRpcPortDemuxNode final : public SessionTree<RxRpcPortDemuxNode>::NodeBase, public IRxSessionDelegate
    {
    public:
        RxRpcPortDemuxNode(const ResponseRxParams& params, std::tuple<TransportDelegate&> args_tuple)
            : transport_delegate_{std::get<0>(args_tuple)}
            , ref_count_{0}
            , port_{}
        {
            transport_delegate_.listenForRxRpcPort<false>(port_, params);

            // No Sonar `cpp:S5356` b/c we integrate here with C libudpard API.
            port_.user_reference = static_cast<IRxSessionDelegate*>(this);  // NOSONAR cpp:S5356
        }

        RxRpcPortDemuxNode(const RxRpcPortDemuxNode&)                = delete;
        RxRpcPortDemuxNode(RxRpcPortDemuxNode&&) noexcept            = delete;
        RxRpcPortDemuxNode& operator=(const RxRpcPortDemuxNode&)     = delete;
        RxRpcPortDemuxNode& operator=(RxRpcPortDemuxNode&&) noexcept = delete;

        ~RxRpcPortDemuxNode()
        {
            transport_delegate_.cancelRxRpcPortFor(port_, false);  // response
        }

        CETL_NODISCARD std::int32_t compareByParams(const ResponseRxParams& params) const
        {
            return static_cast<std::int32_t>(port_.service_id) - static_cast<std::int32_t>(params.service_id);
        }

        CETL_NODISCARD UdpardRxRPCPort& port() noexcept
        {
            return port_;
        }

        void retain() noexcept
        {
            ++ref_count_;
        }

        bool release() noexcept
        {
            CETL_DEBUG_ASSERT(ref_count_ > 0, "");
            --ref_count_;
            return ref_count_ == 0;
        }

    private:
        // IRxSessionDelegate

        void acceptRxTransfer(UdpardMemory&&            udpard_memory,
                              const TransferRxMetadata& rx_metadata,
                              const NodeId              source_node_id) override
        {
            // This is where de-multiplexing happens: the transfer is forwarded to the appropriate session.
            // It's ok not to find the session delegate here - we drop unsolicited transfers.
            //
            const ResponseRxParams params{0, port_.service_id, source_node_id};
            if (auto* const session_delegate = transport_delegate_.tryFindRxSessionDelegateFor(params))
            {
                session_delegate->acceptRxTransfer(std::move(udpard_memory), rx_metadata, source_node_id);
            }
        }

        // MARK: Data members:

        TransportDelegate& transport_delegate_;
        std::size_t        ref_count_;
        UdpardRxRPCPort    port_;

    };  // RxRpcPortDemuxNode

    /// @brief Allocates memory for udpard.
    ///
    /// NOSONAR cpp:S5008 is unavoidable: this is integration with Udpard C memory management.
    ///
    static void* allocateMemoryForUdpard(void* const user_reference, const size_t size)  // NOSONAR cpp:S5008
    {
        // No Sonar `cpp:S5356` and `cpp:S5357` b/c the raw `user_reference` is part of libudpard api,
        // and it was set by us at `makeUdpardMemoryResource` call.
        auto* const mr = static_cast<cetl::pmr::memory_resource*>(user_reference);  // NOSONAR cpp:S5356 cpp:S5357
        CETL_DEBUG_ASSERT(mr != nullptr, "Memory resource should not be null.");
        return mr->allocate(size);
    }

    /// @brief Releases memory allocated for udpard (by previous `allocateMemoryForUdpard` call).
    ///
    /// NOSONAR cpp:S5008 is unavoidable: this is integration with Udpard C memory management.
    ///
    static void deallocateMemoryForUdpard(void* const  user_reference,  // NOSONAR cpp:S5008
                                          const size_t size,
                                          void* const  pointer)  // NOSONAR cpp:S5008
    {
        CETL_DEBUG_ASSERT((pointer != nullptr) || (size == 0), "");
        if (nullptr == pointer)
        {
            return;
        }

        // No Sonar `cpp:S5356` and `cpp:S5357` b/c the raw `user_reference` is part of libudpard api,
        // and it was set by us at `makeUdpardMemoryResource` call.
        auto* const mr = static_cast<cetl::pmr::memory_resource*>(user_reference);  // NOSONAR cpp:S5356 cpp:S5357
        CETL_DEBUG_ASSERT(mr != nullptr, "Memory resource should not be null.");
        mr->deallocate(pointer, size);
    }

    // MARK: Data members:

    UdpardNodeID                    udpard_node_id_;
    const MemoryResources           memory_resources_;
    UdpardRxRPCDispatcher           rx_rpc_dispatcher_;
    SessionTree<RxRpcPortDemuxNode> rx_rpc_port_demux_nodes_;

};  // TransportDelegate

}  // namespace detail
}  // namespace udp
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_UDP_DELEGATE_HPP_INCLUDED
