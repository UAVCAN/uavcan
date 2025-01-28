/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_CAN_TRANSPORT_IMPL_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_CAN_TRANSPORT_IMPL_HPP_INCLUDED

#include "can_transport.hpp"
#include "delegate.hpp"
#include "media.hpp"
#include "msg_rx_session.hpp"
#include "msg_tx_session.hpp"
#include "rx_session_tree_node.hpp"
#include "svc_rx_sessions.hpp"
#include "svc_tx_sessions.hpp"

#include "libcyphal/executor.hpp"
#include "libcyphal/transport/contiguous_payload.hpp"
#include "libcyphal/transport/errors.hpp"
#include "libcyphal/transport/lizard_helpers.hpp"
#include "libcyphal/transport/msg_sessions.hpp"
#include "libcyphal/transport/session_tree.hpp"
#include "libcyphal/transport/svc_sessions.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <canard.h>
#include <cetl/cetl.hpp>
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

/// Internal implementation details of the CAN transport.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

/// @brief Represents final implementation class of the CAN transport.
///
class TransportImpl final : private TransportDelegate, public ICanTransport
{
    /// @brief Defines private specification for making interface unique ptr.
    ///
    struct Spec : libcyphal::detail::UniquePtrSpec<ICanTransport, TransportImpl>
    {
        // `explicit` here is in use to disable public construction of derived private `Spec` structs.
        // See https://seanmiddleditch.github.io/enabling-make-unique-with-private-constructors/
        explicit Spec() = default;
    };

    /// @brief Defines private storage of a media index, its interface and TX queue.
    ///
    struct Media final
    {
    public:
        Media(const std::size_t index, IMedia& interface, const std::size_t tx_capacity)
            : index_{static_cast<std::uint8_t>(index)}
            , interface_{interface}
            , canard_tx_queue_{::canardTxInit(tx_capacity, interface.getMtu(), makeTxMemoryResource(interface))}
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

        IExecutor::Callback::Any& tx_callback()
        {
            return tx_callback_;
        }

        IExecutor::Callback::Any& rx_callback()
        {
            return rx_callback_;
        }

        void propagateMtuToTxQueue()
        {
            canard_tx_queue_.mtu_bytes = interface_.getMtu();
        }

    private:
        CETL_NODISCARD static CanardMemoryResource makeTxMemoryResource(IMedia& media_interface)
        {
            using LizardHelpers = libcyphal::transport::detail::LizardHelpers;

            // TX memory resource is used for raw bytes block allocations only.
            // So it has no alignment requirements.
            constexpr std::size_t Alignment = 1;

            return LizardHelpers::makeMemoryResource<CanardMemoryResource, Alignment>(
                media_interface.getTxMemoryResource());
        }

        const std::uint8_t       index_;
        IMedia&                  interface_;
        CanardTxQueue            canard_tx_queue_;
        IExecutor::Callback::Any rx_callback_;
        IExecutor::Callback::Any tx_callback_;

    };  // Media
    using MediaArray = libcyphal::detail::VarArray<Media>;

public:
    CETL_NODISCARD static Expected<UniquePtr<ICanTransport>, FactoryFailure> make(  //
        cetl::pmr::memory_resource& memory,
        IExecutor&                  executor,
        const cetl::span<IMedia*>   media,
        const std::size_t           tx_capacity)
    {
        // Verify input arguments:
        // - At least one media interface must be provided, but no more than the maximum allowed (255).
        //
        const auto media_count = static_cast<std::size_t>(
            std::count_if(media.begin(), media.end(), [](const IMedia* const media_ptr) -> bool {
                return media_ptr != nullptr;
            }));
        if ((media_count == 0) || (media_count > std::numeric_limits<std::uint8_t>::max()))
        {
            return ArgumentError{};
        }

        // False positive of clang-tidy - we move `media_array` to the `transport` instance, so can't make it const.
        // NOLINTNEXTLINE(misc-const-correctness)
        MediaArray media_array = makeMediaArray(memory, media_count, media, tx_capacity);
        if (media_array.size() != media_count)
        {
            return MemoryError{};
        }

        auto transport =
            libcyphal::detail::makeUniquePtr<Spec>(memory, Spec{}, memory, executor, std::move(media_array));
        if (transport == nullptr)
        {
            return MemoryError{};
        }

        return transport;
    }

    TransportImpl(const Spec, cetl::pmr::memory_resource& memory, IExecutor& executor, MediaArray&& media_array)
        : TransportDelegate{memory}
        , executor_{executor}
        , media_array_{std::move(media_array)}
        , svc_response_rx_session_nodes_{memory}
    {
        scheduleConfigOfFilters();
    }

    TransportImpl(const TransportImpl&)                = delete;
    TransportImpl(TransportImpl&&) noexcept            = delete;
    TransportImpl& operator=(const TransportImpl&)     = delete;
    TransportImpl& operator=(TransportImpl&&) noexcept = delete;

    ~TransportImpl()
    {
        configure_filters_callback_.reset();

        for (Media& media : media_array_)
        {
            flushCanardTxQueue(media.canard_tx_queue(), canardInstance());
        }

        CETL_DEBUG_ASSERT(svc_response_rx_session_nodes_.isEmpty(),
                          "Service sessions must be destroyed before transport.");
    }

    // In use (public) for unit tests only.
    CETL_NODISCARD TransportDelegate& asDelegate()
    {
        return *this;
    }

private:
    using Callback = IExecutor::Callback;

    // MARK: ICanTransport

    void setTransientErrorHandler(TransientErrorHandler handler) override
    {
        transient_error_handler_ = std::move(handler);
    }

    // MARK: ITransport

    CETL_NODISCARD cetl::optional<NodeId> getLocalNodeId() const noexcept override
    {
        if (getNodeId() > CANARD_NODE_ID_MAX)
        {
            return cetl::nullopt;
        }

        return cetl::make_optional(getNodeId());
    }

    CETL_NODISCARD cetl::optional<ArgumentError> setLocalNodeId(const NodeId new_node_id) noexcept override
    {
        if (new_node_id > CANARD_NODE_ID_MAX)
        {
            return ArgumentError{};
        }

        // Allow setting the same node ID multiple times, but only once otherwise.
        //
        if (getNodeId() == new_node_id)
        {
            return cetl::nullopt;
        }
        if (getNodeId() != CANARD_NODE_ID_UNSET)
        {
            return ArgumentError{};
        }
        setNodeId(new_node_id);

        // We just became non-anonymous node, so we might need to reconfigure media filters
        // in case we have at least one service RX port.
        //
        // @see scheduleConfigOfFilters
        //
        const auto& subs_stats = getSubscriptionStats();
        if (subs_stats.total_svc_rx_ports > 0)
        {
            const bool result = configure_filters_callback_.schedule(Callback::Schedule::Once{executor_.now()});
            (void) result;
            CETL_DEBUG_ASSERT(result, "Unexpected failure to schedule filter configuration.");
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

    CETL_NODISCARD Expected<UniquePtr<IMessageRxSession>, AnyFailure> makeMessageRxSession(
        const MessageRxParams& params) override
    {
        return makeRxSessionImpl<IMessageRxSession, MessageRxSession>(  //
            CanardTransferKindMessage,
            params.subject_id,
            params);
    }

    CETL_NODISCARD Expected<UniquePtr<IMessageTxSession>, AnyFailure> makeMessageTxSession(
        const MessageTxParams& params) override
    {
        return MessageTxSession::make(memory(), asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IRequestRxSession>, AnyFailure> makeRequestRxSession(
        const RequestRxParams& params) override
    {
        return makeRxSessionImpl<IRequestRxSession, SvcRequestRxSession>(  //
            CanardTransferKindRequest,
            params.service_id,
            params);
    }

    CETL_NODISCARD Expected<UniquePtr<IRequestTxSession>, AnyFailure> makeRequestTxSession(
        const RequestTxParams& params) override
    {
        return SvcRequestTxSession::make(memory(), asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IResponseRxSession>, AnyFailure> makeResponseRxSession(
        const ResponseRxParams& params) override
    {
        return makeResponseRxSessionImpl(params);
    }

    CETL_NODISCARD Expected<UniquePtr<IResponseTxSession>, AnyFailure> makeResponseTxSession(
        const ResponseTxParams& params) override
    {
        return SvcResponseTxSession::make(memory(), asDelegate(), params);
    }

    // MARK: TransportDelegate

    CETL_NODISCARD cetl::optional<AnyFailure> sendTransfer(const TimePoint               deadline,
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

        const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(executor_.now().time_since_epoch());
        const auto deadline_us = std::chrono::duration_cast<std::chrono::microseconds>(deadline.time_since_epoch());

        for (Media& media : media_array_)
        {
            media.propagateMtuToTxQueue();

            // No Sonar `cpp:S5356` b/c we need to pass payload as a raw data to the libcanard.
            const std::int32_t result = ::canardTxPush(&media.canard_tx_queue(),
                                                       &canardInstance(),
                                                       static_cast<CanardMicrosecond>(deadline_us.count()),
                                                       &metadata,
                                                       {payload.size(), payload.data()},  // NOSONAR cpp:S5356
                                                       static_cast<CanardMicrosecond>(now_us.count()));

            cetl::optional<AnyFailure> failure =
                tryHandleTransientCanardResult<TransientErrorReport::CanardTxPush>(media, result);
            if (failure.has_value())
            {
                // The handler (if any) just said that it's NOT fine to continue with pushing to other media TX queues,
                // and the failure should not be ignored but propagated outside.
                return failure;
            }

            // No need to try to push next frame when previous one hasn't finished yet.
            if (!media.tx_callback())
            {
                pushNextFrameToMedia(media);
            }
        }

        return cetl::nullopt;
    }

    void onSessionEvent(const SessionEvent::Variant& event_var) noexcept override
    {
        // `visit` might hypothetically throw, so we need to catch it.
        const auto result = libcyphal::detail::performWithoutThrowing([this, &event_var] {
            //
            cetl::visit(cetl::make_overloaded(  //
                            [this](const SessionEvent::SvcResponseDestroyed& event) noexcept {
                                //
                                svc_response_rx_session_nodes_.removeNodeFor(event.params);
                            },
                            [](const auto&) noexcept {
                                // No specific action needed for other events.
                                // But we still might need to reconfigure filters (see below after `visit`).
                            }),
                        event_var);
        });
        (void) result;
        CETL_DEBUG_ASSERT(result, "");

        cancelRxCallbacksIfNoPortsLeft();
        scheduleConfigOfFilters();
    }

    IRxSessionDelegate* tryFindRxSessionDelegateFor(const ResponseRxParams& params) override
    {
        if (auto* const node = svc_response_rx_session_nodes_.tryFindNodeFor(params))
        {
            return node->delegate();
        }
        return nullptr;
    }

    void scheduleConfigOfFilters() noexcept
    {
        if (!configure_filters_callback_)
        {
            configure_filters_callback_ = executor_.registerCallback([this](const auto&) {
                //
                configureMediaFilters();
            });
        }

        const bool result = configure_filters_callback_.schedule(Callback::Schedule::Once{executor_.now()});
        (void) result;
        CETL_DEBUG_ASSERT(result, "Unexpected failure to schedule filter configuration.");
    }

    // MARK: Privates:

    using Self = TransportImpl;

    template <typename Node>
    using SessionTree = transport::detail::SessionTree<Node>;

    template <typename Interface, typename Factory, typename Params>
    CETL_NODISCARD auto makeRxSessionImpl(  //
        const CanardTransferKind transfer_kind,
        const PortId             port_id,
        const Params&            params) -> Expected<UniquePtr<Interface>, AnyFailure>
    {
        const std::int8_t has_port = ::canardRxGetSubscription(&canardInstance(), transfer_kind, port_id, nullptr);
        CETL_DEBUG_ASSERT(has_port >= 0, "There is no way currently to get an error here.");
        if (has_port > 0)
        {
            return AlreadyExistsError{};
        }

        auto session_result = Factory::make(memory(), asDelegate(), params);
        if (auto* const make_failure = cetl::get_if<AnyFailure>(&session_result))
        {
            return std::move(*make_failure);
        }

        for (Media& media : media_array_)
        {
            if (!media.rx_callback())
            {
                media.rx_callback() = media.interface().registerPopCallback([this, &media](const auto&) {  //
                    //
                    receiveNextFrame(media);
                });
            }
        }

        return session_result;
    }

    CETL_NODISCARD auto makeResponseRxSessionImpl(  //
        const ResponseRxParams& params) -> Expected<UniquePtr<IResponseRxSession>, AnyFailure>
    {
        // Make sure that session is unique per given parameters.
        // For response sessions, the uniqueness is based on the service ID and the server node ID.
        //
        auto node_result = svc_response_rx_session_nodes_.ensureNodeFor<true>(params);  // should be new
        if (auto* const failure = cetl::get_if<AnyFailure>(&node_result))
        {
            return std::move(*failure);
        }
        auto& new_svc_node = cetl::get<RxSessionTreeNode::Response::RefWrapper>(node_result).get();

        auto session_result = SvcResponseRxSession::make(memory(), asDelegate(), params, new_svc_node);
        if (auto* const make_failure = cetl::get_if<AnyFailure>(&session_result))
        {
            // We failed to create the session, so we need to release the unique node.
            // The sockets we made earlier will be released in the destructor of whole transport.
            svc_response_rx_session_nodes_.removeNodeFor(params);
            return std::move(*make_failure);
        }

        for (Media& media : media_array_)
        {
            if (!media.rx_callback())
            {
                media.rx_callback() = media.interface().registerPopCallback([this, &media](const auto&) {  //
                    //
                    receiveNextFrame(media);
                });
            }
        }

        return session_result;
    }

    template <typename Report, typename... Args>
    cetl::optional<AnyFailure> tryHandleTransientFailure(AnyFailure&& failure, Args&&... args)
    {
        if (transient_error_handler_)
        {
            TransientErrorReport::Variant report_var{Report{std::move(failure), std::forward<Args>(args)...}};
            return transient_error_handler_(report_var);
        }

        return std::move(failure);
    }

    template <typename Report>
    void tryHandleTransientMediaFailure(const Media& media, MediaFailure&& media_failure)
    {
        auto failure = libcyphal::detail::upcastVariant<AnyFailure>(std::move(media_failure));
        (void) tryHandleTransientFailure<Report>(std::move(failure), media.index(), media.interface());
    }

    template <typename Report>
    cetl::optional<AnyFailure> tryHandleTransientCanardResult(const Media& media, const std::int32_t result)
    {
        cetl::optional<AnyFailure> failure = optAnyFailureFromCanard(result);
        if (!failure)
        {
            return cetl::nullopt;
        }

        return tryHandleTransientFailure<Report>(std::move(*failure), media.index(), canardInstance());
    }

    CETL_NODISCARD static MediaArray makeMediaArray(cetl::pmr::memory_resource& memory,
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

    static void flushCanardTxQueue(CanardTxQueue& canard_tx_queue, const CanardInstance& canard_instance)
    {
        while (CanardTxQueueItem* const maybe_item = ::canardTxPeek(&canard_tx_queue))
        {
            CanardTxQueueItem* const item = ::canardTxPop(&canard_tx_queue, maybe_item);
            ::canardTxFree(&canard_tx_queue, &canard_instance, item);
        }
    }

    void receiveNextFrame(const Media& media)
    {
        std::array<cetl::byte, CANARD_MTU_MAX> payload{};

        IMedia::PopResult::Type pop_result = media.interface().pop(payload);
        if (auto* const failure = cetl::get_if<MediaFailure>(&pop_result))
        {
            using Report = TransientErrorReport::MediaPop;
            tryHandleTransientMediaFailure<Report>(media, std::move(*failure));
            return;
        }
        const auto& pop_success = cetl::get<IMedia::PopResult::Success>(pop_result);
        if (!pop_success.has_value())
        {
            return;
        }

        const IMedia::PopResult::Metadata& pop_meta = pop_success.value();

        const auto timestamp_us =
            std::chrono::duration_cast<std::chrono::microseconds>(pop_meta.timestamp.time_since_epoch());
        const CanardFrame canard_frame{pop_meta.can_id, {pop_meta.payload_size, payload.data()}};

        CanardRxTransfer      out_transfer{};
        CanardRxSubscription* out_subscription{};

        const std::int8_t result = ::canardRxAccept(&canardInstance(),
                                                    static_cast<CanardMicrosecond>(timestamp_us.count()),
                                                    &canard_frame,
                                                    media.index(),
                                                    &out_transfer,
                                                    &out_subscription);

        (void) tryHandleTransientCanardResult<TransientErrorReport::CanardRxAccept>(media, result);
        if (result > 0)
        {
            CETL_DEBUG_ASSERT(out_subscription != nullptr, "Expected subscription.");
            CETL_DEBUG_ASSERT(out_subscription->user_reference != nullptr, "Expected session delegate.");

            // No Sonar `cpp:S5357` b/c the raw `user_reference` is part of libcanard api,
            // and it was set by us at a RX session constructor (see f.e. `MessageRxSession` ctor).
            auto* const session_delegate =
                static_cast<IRxSessionDelegate*>(out_subscription->user_reference);  // NOSONAR cpp:S5357

            const auto transfer_id = static_cast<TransferId>(out_transfer.metadata.transfer_id);
            const auto priority    = static_cast<Priority>(out_transfer.metadata.priority);
            const auto timestamp   = TimePoint{std::chrono::microseconds{out_transfer.timestamp_usec}};

            session_delegate->acceptRxTransfer(CanardMemory{memory(), out_transfer.payload},
                                               TransferRxMetadata{{transfer_id, priority}, timestamp},
                                               out_transfer.metadata.remote_node_id);
        }
    }

    std::int8_t handleMediaTxFrame(Media& media, const CanardMicrosecond deadline, CanardMutableFrame& frame)
    {
        //
        // Move the payload from the frame to the media payload - `media.push` might take ownership of it.
        // No Sonar `cpp:S5356` and `cpp:S5357` b/c we integrate here with C libcanard API.
        //
        MediaPayload payload{frame.payload.size,
                             static_cast<cetl::byte*>(frame.payload.data),  // NOSONAR cpp:S5356 cpp:S5357
                             frame.payload.allocated_size,
                             &media.interface().getTxMemoryResource()};
        frame.payload = {0, nullptr, 0};

        auto push_result = media.interface().push(TimePoint{std::chrono::microseconds{deadline}},  //
                                                  frame.extended_can_id,
                                                  payload);

        if (const auto* const push = cetl::get_if<IMedia::PushResult::Success>(&push_result))
        {
            if (!push->is_accepted)
            {
                // Media has not accepted the frame, so we need return original payload back to the item,
                // so that in the future potential retry could try to push it again.
                // No Sonar `cpp:S5356` b/c we need to pass payload as a raw data to the libcanard.
                const auto org_payload       = payload.release();
                frame.payload.size           = org_payload.size;
                frame.payload.data           = org_payload.data;  // NOSONAR cpp:S5356
                frame.payload.allocated_size = org_payload.allocated_size;
            }

            // If needed schedule (recursively!) next frame to push.
            // Already existing callback will be called by executor when media TX is ready to push more.
            //
            if (!media.tx_callback())
            {
                media.tx_callback() = media.interface().registerPushCallback([this, &media](const auto&) {
                    //
                    pushNextFrameToMedia(media);
                });
            }
            return push->is_accepted ? 1 : 0;
        }

        using Report = TransientErrorReport::MediaPush;
        tryHandleTransientMediaFailure<Report>(media, cetl::get<IMedia::PushResult::Failure>(std::move(push_result)));
        return -1;
    }

    /// @brief Tries to push next frame from TX queue to media.
    ///
    void pushNextFrameToMedia(Media& media)
    {
        auto frame_handler = [this, &media](const CanardMicrosecond deadline,
                                            CanardMutableFrame&     frame) -> std::int8_t {
            //
            return handleMediaTxFrame(media, deadline, frame);
        };

        // In case of a media failure we gonna try to push another frame from the next transfer in the queue, so
        // that at least (and at most) one new frame will be succesfully attempted to be pushed in the end.
        // Everytime we poll the queue, its size surely decrements (when `result != 0`),
        // so there is no risk of infinite loop here.
        //
        std::int8_t result = -1;
        while (result < 0)
        {
            // No Sonar `cpp:S5356` & `cpp:S5356` b/c we integrate with Canard C api.
            result = ::canardTxPoll(  //
                &media.canard_tx_queue(),
                &canardInstance(),
                static_cast<CanardMicrosecond>(executor_.now().time_since_epoch().count()),
                &frame_handler,  // NOSONAR cpp:S5356
                [](auto* const user_reference, const auto deadline, auto* frame) {
                    //
                    auto* const frame_handler_ptr =
                        static_cast<decltype(frame_handler)*>(user_reference);  // NOSONAR cpp:S5356, cpp:S5357
                    return (*frame_handler_ptr)(deadline, *frame);
                });
        }
    }

    /// @brief Tries to peek the first TX item from the media TX queue which is not expired.
    ///
    /// While searching, any of already expired TX items are pop from the queue and freed (aka dropped).
    /// If there is no still valid TX items in the queue, returns `nullptr`.
    ///
    CETL_NODISCARD CanardTxQueueItem* peekFirstValidTxItem(CanardTxQueue& canard_tx, TimePoint& out_deadline) const
    {
        const TimePoint now = executor_.now();

        while (CanardTxQueueItem* const tx_item = ::canardTxPeek(&canard_tx))
        {
            // We are dropping any TX item that has expired.
            // Otherwise, we would push it to the media interface.
            // We use strictly `<` (instead of `<=`) to give this frame a chance (one extra 1us) at the media.
            //
            const auto deadline = TimePoint{std::chrono::microseconds{tx_item->tx_deadline_usec}};
            if (now < deadline)
            {
                out_deadline = deadline;
                return tx_item;
            }

            // Release whole expired transfer b/c possible next frames of the same transfer are also expired.
            popAndFreeCanardTxQueueItem(canard_tx, canardInstance(), tx_item, true /* whole transfer */);
        }
        return nullptr;
    }

    /// @brief Configures media filters based on the currently active RX ports.
    ///
    /// Temporary allocates memory buffers for all filters, one per each active port (message or service).
    /// In case of redundant media, each media interface will be called with the same span of filters.
    /// In case of zero ports, we still need to call media interfaces to clear their filters,
    /// though there will be no memory allocation for the empty buffer.
    ///
    /// @note Service RX ports are not considered as active ones for \b anonymous nodes.
    ///
    void configureMediaFilters()
    {
        libcyphal::detail::VarArray<Filter> filters{&memory()};
        if (!fillMediaFiltersArray(filters))
        {
            using Report = TransientErrorReport::ConfigureMedia;
            (void) tryHandleTransientFailure<Report>(MemoryError{});
            return;
        }

        for (const Media& media : media_array_)
        {
            auto media_failure = media.interface().setFilters({filters.data(), filters.size()});
            if (media_failure)
            {
                using Report = TransientErrorReport::MediaConfig;
                tryHandleTransientMediaFailure<Report>(media, std::move(*media_failure));
            }
        }
    }

    void cancelRxCallbacksIfNoPortsLeft() noexcept
    {
        const auto& subs_stats = getSubscriptionStats();
        if (0 == (subs_stats.total_msg_rx_ports + subs_stats.total_svc_rx_ports))
        {
            for (Media& media : media_array_)
            {
                media.rx_callback().reset();
            }
        }
    }

    // MARK: Data members:

    IExecutor&                               executor_;
    MediaArray                               media_array_;
    TransientErrorHandler                    transient_error_handler_;
    Callback::Any                            configure_filters_callback_;
    SessionTree<RxSessionTreeNode::Response> svc_response_rx_session_nodes_;

};  // TransportImpl

}  // namespace detail

/// @brief Makes a new CAN transport instance.
///
/// NB! Lifetime of the transport instance must never outlive `memory` and `media` instances.
///
/// @param memory Reference to a polymorphic memory resource to use for all allocations.
/// @param executor Interface of the executor to use.
/// @param media Collection of redundant media interfaces to use.
/// @param tx_capacity Total number of frames that can be queued for transmission per `IMedia` instance.
/// @return Unique pointer to the new CAN transport instance or an error.
///
inline Expected<UniquePtr<ICanTransport>, FactoryFailure> makeTransport(cetl::pmr::memory_resource& memory,
                                                                        IExecutor&                  executor,
                                                                        const cetl::span<IMedia*>   media,
                                                                        const std::size_t           tx_capacity)
{
    return detail::TransportImpl::make(memory, executor, media, tx_capacity);
}

}  // namespace can
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_CAN_TRANSPORT_IMPL_HPP_INCLUDED
