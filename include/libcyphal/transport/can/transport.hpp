/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_CAN_TRANSPORT_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_CAN_TRANSPORT_HPP_INCLUDED

#include "delegate.hpp"
#include "media.hpp"
#include "msg_rx_session.hpp"
#include "msg_tx_session.hpp"
#include "svc_rx_sessions.hpp"
#include "svc_tx_sessions.hpp"

#include "libcyphal/transport/contiguous_payload.hpp"
#include "libcyphal/transport/errors.hpp"
#include "libcyphal/transport/msg_sessions.hpp"
#include "libcyphal/transport/multiplexer.hpp"
#include "libcyphal/transport/svc_sessions.hpp"
#include "libcyphal/transport/transport.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <canard.h>
#include <cetl/pf17/attribute.hpp>
#include <cetl/pf17/cetlpf.hpp>
#include <cetl/pf20/cetlpf.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace libcyphal
{
namespace transport
{
namespace can
{

class ICanTransport : public ITransport
{};

/// Internal implementation details of the CAN transport.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

class TransportImpl final : public ICanTransport, private TransportDelegate
{
    /// @brief Defines specification for making interface unique ptr.
    ///
    struct Spec
    {
        using Interface = ICanTransport;
        using Concrete  = TransportImpl;

        // In use to disable public construction.
        // See https://seanmiddleditch.github.io/enabling-make-unique-with-private-constructors/
        explicit Spec() = default;
    };

    /// @brief Internal (private) storage of a media index, its interface and TX queue.
    ///
    struct Media final
    {
    public:
        Media(const std::size_t index, IMedia& interface, const std::size_t tx_capacity)
            : index_{static_cast<std::uint8_t>(index)}
            , interface_{interface}
            , canard_tx_queue_{::canardTxInit(tx_capacity, interface.getMtu())}
        {
        }

        std::uint8_t index() const
        {
            return index_;
        }

        IMedia& interface() const
        {
            return interface_;
        }

        CanardTxQueue& canard_tx_queue()
        {
            return canard_tx_queue_;
        }

        void propagateMtuToTxQueue()
        {
            canard_tx_queue_.mtu_bytes = interface_.getMtu();
        }

    private:
        const std::uint8_t index_;
        IMedia&            interface_;
        CanardTxQueue      canard_tx_queue_;
    };
    using MediaArray = libcyphal::detail::VarArray<Media>;

public:
    CETL_NODISCARD static Expected<UniquePtr<ICanTransport>, FactoryError> make(
        cetl::pmr::memory_resource&  memory,
        IMultiplexer&                multiplexer,
        const cetl::span<IMedia*>    media,
        const std::size_t            tx_capacity,
        const cetl::optional<NodeId> local_node_id)
    {
        // Verify input arguments:
        // - At least one media interface must be provided, but no more than the maximum allowed (255).
        // - If a local node ID is provided, it must be within the valid range.
        //
        const auto media_count =
            static_cast<std::size_t>(std::count_if(media.begin(), media.end(), [](const IMedia* const media_ptr) {
                return media_ptr != nullptr;
            }));
        if ((media_count == 0) || (media_count > std::numeric_limits<std::uint8_t>::max()))
        {
            return ArgumentError{};
        }
        if (local_node_id.has_value() && (local_node_id.value() > CANARD_NODE_ID_MAX))
        {
            return ArgumentError{};
        }

        const MediaArray media_array{make_media_array(memory, media_count, media, tx_capacity)};
        if (media_array.size() != media_count)
        {
            return MemoryError{};
        }

        const auto canard_node_id = static_cast<CanardNodeID>(local_node_id.value_or(CANARD_NODE_ID_UNSET));

        auto transport =
            libcyphal::detail::makeUniquePtr<Spec>(memory, Spec{}, memory, multiplexer, media_array, canard_node_id);
        if (transport == nullptr)
        {
            return MemoryError{};
        }

        return transport;
    }

    TransportImpl(Spec,
                  cetl::pmr::memory_resource& memory,
                  IMultiplexer&               multiplexer,
                  MediaArray                  media_array,
                  const CanardNodeID          canard_node_id)
        : TransportDelegate{memory}
        , media_array_{std::move(media_array)}
        , should_reconfigure_filters_{false}
        , total_message_ports_{0}
        , total_service_ports_{0}
    {
        // TODO: Use it!
        (void) multiplexer;

        canard_instance().node_id = canard_node_id;
    }

    TransportImpl(const TransportImpl&)                = delete;
    TransportImpl(TransportImpl&&) noexcept            = delete;
    TransportImpl& operator=(const TransportImpl&)     = delete;
    TransportImpl& operator=(TransportImpl&&) noexcept = delete;

    ~TransportImpl() override
    {
        for (Media& media : media_array_)
        {
            flushCanardTxQueue(media.canard_tx_queue());
        }

        CETL_DEBUG_ASSERT(total_message_ports_ == 0, "Message sessions must be destroyed before transport.");
        CETL_DEBUG_ASSERT(total_service_ports_ == 0, "Service sessions must be destroyed before transport.");
    }

private:
    // MARK: ITransport

    CETL_NODISCARD cetl::optional<NodeId> getLocalNodeId() const noexcept override
    {
        if (canard_instance().node_id > CANARD_NODE_ID_MAX)
        {
            return cetl::nullopt;
        }

        return cetl::make_optional(static_cast<NodeId>(canard_instance().node_id));
    }

    CETL_NODISCARD cetl::optional<ArgumentError> setLocalNodeId(const NodeId node_id) noexcept override
    {
        if (node_id > CANARD_NODE_ID_MAX)
        {
            return ArgumentError{};
        }

        // Allow setting the same node ID multiple times, but only once otherwise.
        //
        CanardInstance& ins = canard_instance();
        if (ins.node_id == node_id)
        {
            return cetl::nullopt;
        }
        if (ins.node_id != CANARD_NODE_ID_UNSET)
        {
            return ArgumentError{};
        }

        ins.node_id = static_cast<CanardNodeID>(node_id);

        // We just became non-anonymous node, so we might need to reconfigure media filters
        // in case we have at least one service RX subscription.
        //
        // @see runMediaFilters
        //
        if (total_service_ports_ > 0)
        {
            should_reconfigure_filters_ = true;
        }

        return cetl::nullopt;
    }

    CETL_NODISCARD ProtocolParams getProtocolParams() const noexcept override
    {
        std::size_t min_mtu = std::numeric_limits<std::size_t>::max();
        for (const Media& media : media_array_)
        {
            min_mtu = std::min(min_mtu, media.interface().getMtu());
        }

        return ProtocolParams{static_cast<TransferId>(1) << CANARD_TRANSFER_ID_BIT_LENGTH,
                              min_mtu,
                              CANARD_NODE_ID_MAX + 1};
    }

    CETL_NODISCARD Expected<UniquePtr<IMessageRxSession>, AnyError> makeMessageRxSession(
        const MessageRxParams& params) override
    {
        const cetl::optional<AnyError> any_error = ensureNewSessionFor(CanardTransferKindMessage, params.subject_id);
        if (any_error.has_value())
        {
            return any_error.value();
        }

        return MessageRxSession::make(asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IMessageTxSession>, AnyError> makeMessageTxSession(
        const MessageTxParams& params) override
    {
        return MessageTxSession::make(asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IRequestRxSession>, AnyError> makeRequestRxSession(
        const RequestRxParams& params) override
    {
        const cetl::optional<AnyError> any_error = ensureNewSessionFor(CanardTransferKindRequest, params.service_id);
        if (any_error.has_value())
        {
            return any_error.value();
        }

        return SvcRequestRxSession::make(asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IRequestTxSession>, AnyError> makeRequestTxSession(
        const RequestTxParams& params) override
    {
        return SvcRequestTxSession::make(asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IResponseRxSession>, AnyError> makeResponseRxSession(
        const ResponseRxParams& params) override
    {
        const cetl::optional<AnyError> any_error = ensureNewSessionFor(CanardTransferKindResponse, params.service_id);
        if (any_error.has_value())
        {
            return any_error.value();
        }

        return SvcResponseRxSession::make(asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IResponseTxSession>, AnyError> makeResponseTxSession(
        const ResponseTxParams& params) override
    {
        return SvcResponseTxSession::make(asDelegate(), params);
    }

    // MARK: IRunnable

    void run(const TimePoint now) override
    {
        runMediaTransmit(now);
        runMediaReceive();
        runMediaFilters();
    }

    // MARK: TransportDelegate

    CETL_NODISCARD TransportDelegate& asDelegate()
    {
        return *this;
    }

    CETL_NODISCARD cetl::optional<AnyError> sendTransfer(const TimePoint               deadline,
                                                         const CanardTransferMetadata& metadata,
                                                         const PayloadFragments        payload_fragments) override
    {
        // libcanard currently does not support fragmented payloads (at `canardTxPush`).
        // so we need to concatenate them when there are more than one non-empty fragment.
        // See https://github.com/OpenCyphal/libcanard/issues/223
        //
        const transport::detail::ContiguousPayload payload{memory(), payload_fragments};
        if ((payload.data() == nullptr) && (payload.size() > 0))
        {
            return MemoryError{};
        }

        const auto deadline_us = std::chrono::duration_cast<std::chrono::microseconds>(deadline.time_since_epoch());

        // TODO: Rework error handling strategy.
        //       Currently, we return the last error encountered, but we should consider all errors somehow.
        //
        cetl::optional<AnyError> maybe_error{};

        for (Media& media : media_array_)
        {
            media.propagateMtuToTxQueue();

            const std::int32_t result = ::canardTxPush(&media.canard_tx_queue(),
                                                       &canard_instance(),
                                                       static_cast<CanardMicrosecond>(deadline_us.count()),
                                                       &metadata,
                                                       payload.size(),
                                                       payload.data());
            if (result < 0)
            {
                maybe_error = TransportDelegate::anyErrorFromCanard(result);
            }
        }

        return maybe_error;
    }

    void triggerUpdateOfFilters(const FiltersUpdateCondition condition) noexcept override
    {
        switch (condition)
        {
        case FiltersUpdateCondition::SubjectPortAdded: {
            ++total_message_ports_;
            break;
        }
        case FiltersUpdateCondition::SubjectPortRemoved: {
            // We are not going to allow negative number of ports.
            CETL_DEBUG_ASSERT(total_message_ports_ > 0, "");
            total_message_ports_ -= std::min(static_cast<std::size_t>(1), total_message_ports_);
            break;
        }
        case FiltersUpdateCondition::ServicePortAdded: {
            ++total_service_ports_;
            break;
        }
        case FiltersUpdateCondition::ServicePortRemoved: {
            // We are not going to allow negative number of ports.
            CETL_DEBUG_ASSERT(total_service_ports_ > 0, "");
            total_service_ports_ -= std::min(static_cast<std::size_t>(1), total_service_ports_);
            break;
        }
        default: {
            // NOLINTNEXTLINE(cert-dcl03-c,hicpp-static-assert,misc-static-assert)
            CETL_DEBUG_ASSERT(false, "Unexpected condition.");
            return;
        }
        }

        should_reconfigure_filters_ = true;
    }

    // MARK: Privates:

    CETL_NODISCARD cetl::optional<AnyError> ensureNewSessionFor(const CanardTransferKind transfer_kind,
                                                                const PortId             port_id) noexcept
    {
        const std::int8_t hasSubscription =
            ::canardRxGetSubscription(&canard_instance(), transfer_kind, port_id, nullptr);
        CETL_DEBUG_ASSERT(hasSubscription >= 0, "There is no way currently to get an error here.");
        if (hasSubscription > 0)
        {
            return AlreadyExistsError{};
        }

        return {};
    }

    static MediaArray make_media_array(cetl::pmr::memory_resource& memory,
                                       const std::size_t           media_count,
                                       const cetl::span<IMedia*>   media_interfaces,
                                       const std::size_t           tx_capacity)
    {
        MediaArray media_array{media_count, &memory};

        // Reserve the space for the whole array (to avoid reallocations).
        // Capacity will be less than requested in case of out of memory.
        media_array.reserve(media_count);
        if (media_array.capacity() >= media_count)
        {
            std::size_t index = 0;
            for (IMedia* const media_interface : media_interfaces)
            {
                if (media_interface != nullptr)
                {
                    IMedia& media = *media_interface;
                    media_array.emplace_back(index, media, tx_capacity);
                    index++;
                }
            }
            CETL_DEBUG_ASSERT(index == media_count, "");
            CETL_DEBUG_ASSERT(media_array.size() == media_count, "");
        }

        return media_array;
    }

    void flushCanardTxQueue(CanardTxQueue& canard_tx_queue)
    {
        while (const CanardTxQueueItem* const maybe_item = ::canardTxPeek(&canard_tx_queue))
        {
            CanardTxQueueItem* const item = ::canardTxPop(&canard_tx_queue, maybe_item);
            freeCanardMemory(item);
        }
    }

    void runMediaReceive()
    {
        std::array<cetl::byte, CANARD_MTU_MAX> payload{};

        for (const Media& media : media_array_)
        {
            // TODO: Handle errors.
            const Expected<cetl::optional<RxMetadata>, MediaError> pop_result = media.interface().pop(payload);
            if (const auto* const opt_rx_meta = cetl::get_if<cetl::optional<RxMetadata>>(&pop_result))
            {
                if (opt_rx_meta->has_value())
                {
                    const RxMetadata& rx_meta = opt_rx_meta->value();

                    const auto timestamp_us =
                        std::chrono::duration_cast<std::chrono::microseconds>(rx_meta.timestamp.time_since_epoch());
                    const CanardFrame canard_frame{rx_meta.can_id, rx_meta.payload_size, payload.cbegin()};

                    CanardRxTransfer      out_transfer{};
                    CanardRxSubscription* out_subscription{};

                    // TODO: Handle errors.
                    const std::int8_t result = ::canardRxAccept(&canard_instance(),
                                                                static_cast<CanardMicrosecond>(timestamp_us.count()),
                                                                &canard_frame,
                                                                media.index(),
                                                                &out_transfer,
                                                                &out_subscription);
                    if (result > 0)
                    {
                        CETL_DEBUG_ASSERT(out_subscription != nullptr, "Expected subscription.");
                        CETL_DEBUG_ASSERT(out_subscription->user_reference != nullptr, "Expected session delegate.");

                        auto* const delegate = static_cast<IRxSessionDelegate*>(out_subscription->user_reference);
                        delegate->acceptRxTransfer(out_transfer);
                    }
                }
            }

        }  // for each media
    }

    void runMediaTransmit(const TimePoint now)
    {
        for (Media& media : media_array_)
        {
            while (const CanardTxQueueItem* const tx_item = ::canardTxPeek(&media.canard_tx_queue()))
            {
                // We are dropping any TX item that has expired.
                // Otherwise, we would send it to the media interface.
                // We use strictly `<` (instead of `<=`) to give this frame a chance (one extra 1us) at media level.
                //
                const auto deadline = TimePoint{std::chrono::microseconds{tx_item->tx_deadline_usec}};
                if (now < deadline)
                {
                    const cetl::span<const cetl::byte> payload{static_cast<const cetl::byte*>(tx_item->frame.payload),
                                                               tx_item->frame.payload_size};
                    const Expected<bool, MediaError>   maybe_pushed =
                        media.interface().push(deadline, tx_item->frame.extended_can_id, payload);
                    if (const auto* const is_pushed = cetl::get_if<bool>(&maybe_pushed))
                    {
                        if (!*is_pushed)
                        {
                            // Media interface is busy, so we will try again with it later (on next `run`).
                            break;
                        }
                    }
                    else
                    {
                        // In case of media error we are going to drop this frame
                        // (b/c it looks like media can't handle this frame),
                        // but we will continue to process other frames.

                        // TODO: Add error reporting somehow.
                    }
                }

                freeCanardMemory(::canardTxPop(&media.canard_tx_queue(), tx_item));

            }  // for each frame

        }  // for each media
    }

    /// \brief Runs (if needed) reconfiguration of media filters based on the currently active subscriptions.
    ///
    /// Temporary allocates memory buffers for all filters, one per each active subscription (message or service).
    /// In case of redundant media, each media interface will be called with the same span of filters.
    /// In case of zero subscriptions, we still need to call media interfaces to clear their filters,
    /// though there will be no memory allocation for the empty buffer.
    ///
    /// @note Service RX subscriptions are not considered as active ones for \b anonymous nodes.
    ///
    /// @note If \b whole reconfiguration process was successful,
    /// `should_reconfigure_filters_` will be reset to `false`, so that next time the run won't do any work.
    /// But in case of any failure (memory allocation or media error),
    /// `should_reconfigure_filters_` will stay engaged (`true`), so that we will try again on next run.
    ///
    void runMediaFilters()
    {
        using RxSubscription     = const CanardRxSubscription;
        using RxSubscriptionTree = CanardConcreteTree<RxSubscription>;

        if (!should_reconfigure_filters_)
        {
            return;
        }

        // Total "active" RX ports depends on the local node ID. For anonymous nodes,
        // we don't account for service ports (b/c they don't work while being anonymous).
        //
        const CanardNodeID local_node_id      = canard_instance().node_id;
        const auto         is_anonymous       = local_node_id > CANARD_NODE_ID_MAX;
        const std::size_t  total_active_ports = total_message_ports_ + (is_anonymous ? 0 : total_service_ports_);

        // There is no memory allocation here yet - just empty span.
        //
        libcyphal::detail::VarArray<Filter> filters{total_active_ports, &memory()};
        if (total_active_ports > 0)
        {
            // Now we know that we have at least one active port,
            // so we need preallocate temp memory for total number of active ports.
            //
            filters.reserve(total_active_ports);
            if (filters.capacity() < total_active_ports)
            {
                // This is out of memory situation. We will just leave this run,
                // but `should_reconfigure_filters_` will stay engaged, so we will try again on next run.
                return;
            }

            // `subs_count` counting is just for the sake of debug verification.
            std::size_t ports_count = 0;

            const auto& subs_trees = canard_instance().rx_subscriptions;

            if (total_message_ports_ > 0)
            {
                const auto msg_visitor = [&filters](RxSubscription& rx_subscription) {
                    const auto msb_flt = ::canardMakeFilterForSubject(rx_subscription.port_id);
                    filters.emplace_back(Filter{msb_flt.extended_can_id, msb_flt.extended_mask});
                };
                ports_count += RxSubscriptionTree::visitCounting(subs_trees[CanardTransferKindMessage], msg_visitor);
            }

            // No need to build service filters if we don't have a local node ID.
            //
            if ((total_service_ports_ > 0) && !is_anonymous)
            {
                const auto svc_visitor = [&filters, local_node_id](RxSubscription& rx_subscription) {
                    const auto flt = ::canardMakeFilterForService(rx_subscription.port_id, local_node_id);
                    filters.emplace_back(Filter{flt.extended_can_id, flt.extended_mask});
                };
                ports_count += RxSubscriptionTree::visitCounting(subs_trees[CanardTransferKindRequest], svc_visitor);
                ports_count += RxSubscriptionTree::visitCounting(subs_trees[CanardTransferKindResponse], svc_visitor);
            }

            (void) ports_count;
            CETL_DEBUG_ASSERT(ports_count == total_active_ports, "");
        }

        // Let each media interface know about the new filters (tracking the fact of possible media error).
        //
        bool was_error = false;
        for (const Media& media : media_array_)
        {
            const cetl::optional<MediaError> maybe_error =
                media.interface().setFilters({filters.data(), filters.size()});
            if (maybe_error.has_value())
            {
                was_error = true;
                // TODO: report error somehow.
            }
        }

        if (!was_error)
        {
            should_reconfigure_filters_ = false;
        }
    }

    // MARK: Data members:

    // Below nolint is to comply with AUTOSAR A11-0-2: in this class we do ALL initialization in the constructor.
    // NOLINTBEGIN(cppcoreguidelines-use-default-member-init,modernize-use-default-member-init)
    MediaArray  media_array_;
    bool        should_reconfigure_filters_;
    std::size_t total_message_ports_;
    std::size_t total_service_ports_;
    // NOLINTEND(cppcoreguidelines-use-default-member-init,modernize-use-default-member-init)

};  // TransportImpl

}  // namespace detail

CETL_NODISCARD inline Expected<UniquePtr<ICanTransport>, FactoryError> makeTransport(
    cetl::pmr::memory_resource&  memory,
    IMultiplexer&                multiplexer,
    const cetl::span<IMedia*>    media,
    const std::size_t            tx_capacity,
    const cetl::optional<NodeId> local_node_id)
{
    return detail::TransportImpl::make(memory, multiplexer, media, tx_capacity, local_node_id);
}

}  // namespace can
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_CAN_TRANSPORT_HPP_INCLUDED
