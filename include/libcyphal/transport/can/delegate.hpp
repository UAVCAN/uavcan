/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_CAN_DELEGATE_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_CAN_DELEGATE_HPP_INCLUDED

#include "media.hpp"
#include "rx_session_tree_node.hpp"

#include "libcyphal/transport/errors.hpp"
#include "libcyphal/transport/msg_sessions.hpp"
#include "libcyphal/transport/scattered_buffer.hpp"
#include "libcyphal/transport/session_tree.hpp"
#include "libcyphal/transport/svc_sessions.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <canard.h>
#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace libcyphal
{
namespace transport
{
namespace can
{

/// Internal implementation details of the CAN transport.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

/// @brief RAII class to manage memory allocated by Canard library.
///
class CanardMemory final : public ScatteredBuffer::IStorage
{
public:
    // No Sonar `cpp:S5356` and `cpp:S5357` b/c we need to pass raw data from C libcanard api.
    CanardMemory(cetl::pmr::memory_resource& memory, CanardMutablePayload& payload)
        : memory_{memory}
        , allocated_size_{std::exchange(payload.allocated_size, 0)}
        , buffer_{static_cast<cetl::byte*>(std::exchange(payload.data, nullptr))}  // NOSONAR cpp:S5356 cpp:S5357
        , payload_size_{std::exchange(payload.size, 0)}
    {
    }
    CanardMemory(CanardMemory&& other) noexcept
        : memory_{other.memory_}
        , allocated_size_{std::exchange(other.allocated_size_, 0)}
        , buffer_{std::exchange(other.buffer_, nullptr)}
        , payload_size_{std::exchange(other.payload_size_, 0)}
    {
    }
    CanardMemory(const CanardMemory&) = delete;

    ~CanardMemory()
    {
        if (buffer_ != nullptr)
        {
            // No Sonar `cpp:S5356` b/c we integrate here with C libcanard memory management.
            memory_.deallocate(buffer_, allocated_size_);  // NOSONAR cpp:S5356
        }
    }

    CanardMemory& operator=(const CanardMemory&)     = delete;
    CanardMemory& operator=(CanardMemory&&) noexcept = delete;

    // MARK: ScatteredBuffer::IStorage

    CETL_NODISCARD std::size_t size() const noexcept override
    {
        return payload_size_;
    }

    CETL_NODISCARD std::size_t copy(const std::size_t offset_bytes,
                                    cetl::byte* const destination,
                                    const std::size_t length_bytes) const override
    {
        CETL_DEBUG_ASSERT((destination != nullptr) || (length_bytes == 0),
                          "Destination could be null only with zero bytes ask.");

        if ((destination == nullptr) || (buffer_ == nullptr) || (payload_size_ <= offset_bytes))
        {
            return 0;
        }

        const std::size_t bytes_to_copy = std::min(length_bytes, payload_size_ - offset_bytes);
        // Next nolint is unavoidable: we need to offset from the beginning of the buffer.
        // No Sonar `cpp:S5356` b/c we integrate here with libcanard raw C buffers.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        (void) std::memmove(destination, buffer_ + offset_bytes, bytes_to_copy);  // NOSONAR cpp:S5356
        return bytes_to_copy;
    }

private:
    // MARK: Data members:

    cetl::pmr::memory_resource& memory_;
    std::size_t                 allocated_size_;
    cetl::byte*                 buffer_;
    std::size_t                 payload_size_;

};  // CanardMemory

// MARK: -

/// This internal session delegate class serves the following purpose: it provides an interface (aka gateway)
/// to access RX session from transport (by casting canard's `user_reference` member to this class).
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
    virtual void acceptRxTransfer(CanardMemory&&            canard_memory,
                                  const TransferRxMetadata& rx_metadata,
                                  const NodeId              source_node_id) = 0;

protected:
    IRxSessionDelegate()  = default;
    ~IRxSessionDelegate() = default;

};  // IRxSessionDelegate

// MARK: -

/// This internal transport delegate class serves the following purposes:
/// 1. It provides memory management functions for the Canard library.
/// 2. It provides a way to convert Canard error codes to `AnyFailure` type.
/// 3. It provides an interface to access the transport from various session classes.
///
class TransportDelegate
{
public:
    /// @brief Utility type with various helpers related to Canard AVL trees.
    ///
    /// @tparam Node Type of concrete AVL tree node.
    ///
    template <typename Node>
    struct CanardConcreteTree
    {
        /// @brief Visits in-order each node of the AVL tree, counting the total number of nodes visited.
        ///
        /// @tparam Visitor Type of the visitor callable.
        /// @param root The root node of the AVL tree. Could be `nullptr`.
        /// @param visitor The callable to be invoked for each non-null node.
        /// @return Total count of visited nodes (including the `root` one). `0` if `root` is `nullptr`.
        ///
        /// No Sonar cpp:S134 b/c this is usual in-order traversal - maintainability is not a concern here.
        ///
        template <typename Visitor>
        static std::size_t visitCounting(CanardTreeNode* const root, const Visitor& visitor)
        {
            std::size_t           count = 0;
            CanardTreeNode*       node  = root;
            const CanardTreeNode* prev  = nullptr;

            while (nullptr != node)
            {
                CanardTreeNode* next = node->up;

                if (prev == node->up)
                {
                    // We came down to this node from `prev`.

                    if (auto* const left = node->lr[0])
                    {
                        next = left;
                    }
                    else
                    {
                        ++count;
                        visitor(down(*node));

                        if (auto* const right = node->lr[1])  // NOSONAR cpp:S134
                        {
                            next = right;
                        }
                    }
                }
                else if (prev == node->lr[0])
                {
                    // We came up to this node from the left child.

                    ++count;
                    visitor(down(*node));

                    if (auto* const right = node->lr[1])
                    {
                        next = right;
                    }
                }
                else
                {
                    // next has already been set to the parent node.
                }

                prev = std::exchange(node, next);
            }

            return count;
        }

    private:
        static Node& down(CanardTreeNode& node)
        {
            // Following nolint & NOSONAR are unavoidable: this is integration with Canard C AVL trees.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            return reinterpret_cast<Node&>(node);  // NOSONAR cpp:S3630
        }

    };  // CanardConcreteTree

    /// Umbrella type for all session-related events.
    ///
    /// These are passed to the `onSessionEvent` method of the transport implementation.
    ///
    struct SessionEvent
    {
        struct MsgCreated
        {};
        struct MsgDestroyed
        {};
        struct SvcRequestCreated
        {};
        struct SvcRequestDestroyed
        {};
        struct SvcResponseCreated
        {};
        struct SvcResponseDestroyed
        {
            ResponseRxParams params;
        };

        using Variant = cetl::variant<  //
            MsgCreated,
            MsgDestroyed,
            SvcRequestCreated,
            SvcRequestDestroyed,
            SvcResponseCreated,
            SvcResponseDestroyed>;

    };  // SessionEvent

    TransportDelegate(const TransportDelegate&)                = delete;
    TransportDelegate(TransportDelegate&&) noexcept            = delete;
    TransportDelegate& operator=(const TransportDelegate&)     = delete;
    TransportDelegate& operator=(TransportDelegate&&) noexcept = delete;

    CETL_NODISCARD NodeId getNodeId() const noexcept
    {
        return canard_instance_.node_id;
    }

    void setNodeId(const NodeId node_id) noexcept
    {
        canard_instance_.node_id = static_cast<CanardNodeID>(node_id);
    }

    CETL_NODISCARD CanardInstance& canardInstance() noexcept
    {
        return canard_instance_;
    }

    CETL_NODISCARD const CanardInstance& canardInstance() const noexcept
    {
        return canard_instance_;
    }

    CETL_NODISCARD cetl::pmr::memory_resource& memory() const noexcept
    {
        return memory_;
    }

    void listenForRxSubscription(CanardRxSubscription& subscription, const MessageRxParams& params)
    {
        listenForRxSubscriptionImpl(subscription, CanardTransferKindMessage, params.subject_id, params.extent_bytes);
    }

    void listenForRxSubscription(CanardRxSubscription& subscription, const RequestRxParams& params)
    {
        listenForRxSubscriptionImpl(subscription, CanardTransferKindRequest, params.service_id, params.extent_bytes);
    }

    void listenForRxSubscription(CanardRxSubscription& subscription, const ResponseRxParams& params)
    {
        listenForRxSubscriptionImpl(subscription, CanardTransferKindResponse, params.service_id, params.extent_bytes);
    }

    void retainRxSubscriptionFor(const ResponseRxParams& params)
    {
        const auto maybe_node = rx_subs_demux_nodes_.ensureNodeFor(params, std::ref(*this));
        if (const auto* const node = cetl::get_if<RxSubsDemuxNode::RefWrapper>(&maybe_node))
        {
            node->get().retain();
        }
    }

    CETL_NODISCARD CanardRxSubscription* findRxSubscriptionFor(const ResponseRxParams& params)
    {
        if (auto* const node = rx_subs_demux_nodes_.tryFindNodeFor(params))
        {
            return &node->subscription();
        }
        return nullptr;
    }

    void releaseRxSubscriptionFor(const ResponseRxParams& params)
    {
        if (auto* const node = rx_subs_demux_nodes_.tryFindNodeFor(params))
        {
            if (node->release())
            {
                rx_subs_demux_nodes_.removeNodeFor(params);
            }
        }
    }

    void cancelRxSubscriptionFor(const CanardRxSubscription& subscription,
                                 const CanardTransferKind    transfer_kind) noexcept
    {
        const std::int8_t result = ::canardRxUnsubscribe(&canard_instance_, transfer_kind, subscription.port_id);
        (void) result;
        CETL_DEBUG_ASSERT(result >= 0, "There is no way currently to get an error here.");
        CETL_DEBUG_ASSERT(result > 0, "Subscription supposed to be made at node constructor.");

        subs_stats_.total_msg_rx_ports -= (transfer_kind == CanardTransferKindMessage) ? 1 : 0;
        subs_stats_.total_svc_rx_ports -= (transfer_kind != CanardTransferKindMessage) ? 1 : 0;
    }

    CETL_NODISCARD static cetl::optional<AnyFailure> optAnyFailureFromCanard(const std::int32_t result)
    {
        // Canard error results are negative, so we need to negate them to get the error code.
        const std::int32_t canard_error = -result;

        if (canard_error == CANARD_ERROR_INVALID_ARGUMENT)
        {
            return ArgumentError{};
        }
        if (canard_error == CANARD_ERROR_OUT_OF_MEMORY)
        {
            return MemoryError{};
        }

        return cetl::nullopt;
    }

    /// @brief Releases memory allocated for canard (by previous `allocateMemoryForCanard` call).
    ///
    /// No Sonar `cpp:S5008` and `cpp:S5356` b/c they are unavoidable -
    /// this is integration with low-level Canard C memory management.
    ///
    void freeCanardMemory(void* const pointer, const std::size_t amount) const  // NOSONAR cpp:S5008
    {
        if (pointer == nullptr)
        {
            return;
        }

        memory_.deallocate(pointer, amount);
    }

    /// Pops and frees Canard TX queue item(s).
    ///
    /// @param tx_queue The TX queue from which the item should be popped.
    /// @param canard_instance The Canard instance to be used for the item deallocation.
    /// @param tx_item The TX queue item to be popped and freed.
    /// @param whole_transfer If `true` then the whole transfer should be released from the queue.
    ///
    static void popAndFreeCanardTxQueueItem(CanardTxQueue&        tx_queue,
                                            const CanardInstance& canard_instance,
                                            CanardTxQueueItem*    tx_item,
                                            const bool            whole_transfer)
    {
        while (CanardTxQueueItem* const mut_tx_item = ::canardTxPop(&tx_queue, tx_item))
        {
            tx_item = tx_item->next_in_transfer;
            ::canardTxFree(&tx_queue, &canard_instance, mut_tx_item);

            if (!whole_transfer)
            {
                break;
            }
        }
    }

    /// @brief Sends transfer to each media canard TX queue of the transport.
    ///
    /// Internal method which is in use by TX session implementations to delegate actual sending to transport.
    ///
    CETL_NODISCARD virtual cetl::optional<AnyFailure> sendTransfer(const TimePoint               deadline,
                                                                   const CanardTransferMetadata& metadata,
                                                                   const PayloadFragments        payload_fragments) = 0;

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
    struct SubscriptionStats
    {
        std::size_t total_msg_rx_ports;
        std::size_t total_svc_rx_ports;
    };

    explicit TransportDelegate(cetl::pmr::memory_resource& memory)
        : memory_{memory}
        , canard_instance_{::canardInit(makeCanardMemoryResource())}
        , rx_subs_demux_nodes_{memory}
        , subs_stats_{}
    {
        // No Sonar `cpp:S5356` b/c we integrate here with C libcanard API.
        canardInstance().user_reference = this;  // NOSONAR cpp:S5356
    }

    ~TransportDelegate()
    {
        CETL_DEBUG_ASSERT(subs_stats_.total_msg_rx_ports == 0,  //
                          "Message subscriptions must be destroyed before transport.");
        CETL_DEBUG_ASSERT(subs_stats_.total_svc_rx_ports == 0,  //
                          "Service subscriptions must be destroyed before transport.");
    }

    const SubscriptionStats& getSubscriptionStats() const
    {
        return subs_stats_;
    }

    /// @brief Fills an array with filters for each active RX port.
    ///
    CETL_NODISCARD bool fillMediaFiltersArray(libcyphal::detail::VarArray<Filter>& filters)
    {
        using RxSubscription     = const CanardRxSubscription;
        using RxSubscriptionTree = CanardConcreteTree<RxSubscription>;

        // Total "active" RX ports depends on the local node ID. For anonymous nodes,
        // we don't account for service ports (b/c they don't work while being anonymous).
        //
        const auto        local_node_id      = canard_instance_.node_id;
        const auto        is_anonymous       = local_node_id > CANARD_NODE_ID_MAX;
        const std::size_t total_active_ports = subs_stats_.total_msg_rx_ports  //
                                               + (is_anonymous ? 0 : subs_stats_.total_svc_rx_ports);
        if (total_active_ports == 0)
        {
            // No need to allocate memory for zero filters.
            return true;
        }

        // Now we know that we have at least one active port,
        // so we need preallocate temp memory for the total number of active ports.
        //
        filters.reserve(total_active_ports);
        if (filters.capacity() < total_active_ports)
        {
            // This is out of memory situation.
            return false;
        }

        // `ports_count` counting is just for the sake of debug verification.
        std::size_t ports_count = 0;

        const auto& subs_trees = canardInstance().rx_subscriptions;

        if (subs_stats_.total_msg_rx_ports > 0)
        {
            const auto msg_visitor = [&filters](RxSubscription& rx_subscription) {
                //
                // Make and store a single message filter.
                const auto flt = ::canardMakeFilterForSubject(rx_subscription.port_id);
                filters.emplace_back(Filter{flt.extended_can_id, flt.extended_mask});
            };
            ports_count += RxSubscriptionTree::visitCounting(subs_trees[CanardTransferKindMessage], msg_visitor);
        }

        // No need to make service filters if we don't have a local node ID.
        //
        if ((subs_stats_.total_svc_rx_ports > 0) && (!is_anonymous))
        {
            const auto svc_visitor = [&filters, local_node_id](RxSubscription& rx_subscription) {
                //
                // Make and store a single service filter.
                const auto flt = ::canardMakeFilterForService(rx_subscription.port_id, local_node_id);
                filters.emplace_back(Filter{flt.extended_can_id, flt.extended_mask});
            };
            ports_count += RxSubscriptionTree::visitCounting(subs_trees[CanardTransferKindRequest], svc_visitor);
            ports_count += RxSubscriptionTree::visitCounting(subs_trees[CanardTransferKindResponse], svc_visitor);
        }

        (void) ports_count;
        CETL_DEBUG_ASSERT(ports_count == total_active_ports, "");
        return true;
    }

private:
    template <typename Node>
    using SessionTree = transport::detail::SessionTree<Node>;

    /// Accepts transfers from RX subscription and forwards them to the appropriate session (according to source node
    /// id). Has reference counting so that it will be destroyed when no longer referenced by any RX session.
    ///
    class RxSubsDemuxNode final : public SessionTree<RxSubsDemuxNode>::NodeBase, public IRxSessionDelegate
    {
    public:
        RxSubsDemuxNode(const ResponseRxParams& params, std::tuple<TransportDelegate&> args_tuple)
            : transport_delegate_{std::get<0>(args_tuple)}
            , ref_count_{0}
            , subscription_{}
        {
            transport_delegate_.listenForRxSubscription(subscription_, params);

            // No Sonar `cpp:S5356` b/c we integrate here with C libudpard API.
            subscription_.user_reference = static_cast<IRxSessionDelegate*>(this);  // NOSONAR cpp:S5356
        }

        RxSubsDemuxNode(const RxSubsDemuxNode&)                = delete;
        RxSubsDemuxNode(RxSubsDemuxNode&&) noexcept            = delete;
        RxSubsDemuxNode& operator=(const RxSubsDemuxNode&)     = delete;
        RxSubsDemuxNode& operator=(RxSubsDemuxNode&&) noexcept = delete;

        ~RxSubsDemuxNode()
        {
            transport_delegate_.cancelRxSubscriptionFor(subscription_, CanardTransferKindResponse);
        }

        CETL_NODISCARD std::int32_t compareByParams(const ResponseRxParams& params) const
        {
            return static_cast<std::int32_t>(subscription_.port_id) - static_cast<std::int32_t>(params.service_id);
        }

        CETL_NODISCARD CanardRxSubscription& subscription() noexcept
        {
            return subscription_;
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

        void acceptRxTransfer(CanardMemory&&            canard_memory,
                              const TransferRxMetadata& rx_metadata,
                              const NodeId              source_node_id) override
        {
            // This is where de-multiplexing happens: the transfer is forwarded to the appropriate session.
            // It's ok not to find the session delegate here - we drop unsolicited transfers.
            //
            const ResponseRxParams params{0, subscription_.port_id, source_node_id};
            if (auto* const session_delegate = transport_delegate_.tryFindRxSessionDelegateFor(params))
            {
                session_delegate->acceptRxTransfer(std::move(canard_memory), rx_metadata, source_node_id);
            }
        }

        // MARK: Data members:

        TransportDelegate&   transport_delegate_;
        std::size_t          ref_count_;
        CanardRxSubscription subscription_;

    };  // RxSubsDemuxNode

    /// @brief Converts Canard instance to the transport delegate.
    ///
    /// In use to bridge two worlds: canard library and transport entities.
    /// NOSONAR  cpp:S5008 is unavoidable: this is integration with low-level Canard C memory management.
    ///
    CETL_NODISCARD static TransportDelegate& getSelfFrom(void* const user_reference)  // NOSONAR cpp:S5008
    {
        CETL_DEBUG_ASSERT(user_reference != nullptr, "Expected `this` transport as user reference.");

        // No Sonar `cpp:S5357` b/c the raw `user_reference` is part of libcanard api,
        // and it was set by us at this delegate constructor (see `TransportDelegate` ctor).
        // NOLINTNEXTLINE
        return *static_cast<TransportDelegate*>(user_reference);  // NOSONAR cpp:S5357
    }

    /// @brief Allocates memory for canard instance.
    ///
    /// NOSONAR  cpp:S5008 is unavoidable: this is integration with low-level Canard C memory management.
    ///
    CETL_NODISCARD static void* allocateMemoryForCanard(void* const       user_reference,  // NOSONAR cpp:S5008
                                                        const std::size_t amount)
    {
        const TransportDelegate& self = getSelfFrom(user_reference);
        return self.memory_.allocate(amount);
    }

    /// @brief Releases memory allocated for canard instance (by previous `allocateMemoryForCanard` call).
    ///
    /// NOSONAR  cpp:S5008 is unavoidable: this is integration with low-level Canard C memory management.
    ///
    static void freeCanardMemory(void* const       user_reference,  // NOSONAR cpp:S5008
                                 const std::size_t amount,
                                 void* const       pointer)  // NOSONAR cpp:S5008
    {
        const TransportDelegate& self = getSelfFrom(user_reference);
        self.freeCanardMemory(pointer, amount);
    }

    CETL_NODISCARD CanardMemoryResource makeCanardMemoryResource()
    {
        // No Sonar `cpp:S5356` b/c we integrate here with C libcanard memory management.
        return {this, freeCanardMemory, allocateMemoryForCanard};  // NOSONAR cpp:S5356
    }

    void listenForRxSubscriptionImpl(CanardRxSubscription&    subscription,
                                     const CanardTransferKind transfer_kind,
                                     const CanardPortID       port_id,
                                     const size_t             extent_bytes)
    {
        const std::int8_t result = ::canardRxSubscribe(&canard_instance_,
                                                       transfer_kind,
                                                       port_id,
                                                       extent_bytes,
                                                       CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
                                                       &subscription);
        (void) result;
        CETL_DEBUG_ASSERT(result >= 0, "There is no way currently to get an error here.");
        CETL_DEBUG_ASSERT(result > 0, "New subscription supposed to be made.");

        subs_stats_.total_msg_rx_ports += (transfer_kind == CanardTransferKindMessage) ? 1 : 0;
        subs_stats_.total_svc_rx_ports += (transfer_kind != CanardTransferKindMessage) ? 1 : 0;
    }

    // MARK: Data members:

    cetl::pmr::memory_resource&  memory_;
    CanardInstance               canard_instance_;
    SessionTree<RxSubsDemuxNode> rx_subs_demux_nodes_;
    SubscriptionStats            subs_stats_;

};  // TransportDelegate

}  // namespace detail
}  // namespace can
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_CAN_DELEGATE_HPP_INCLUDED
