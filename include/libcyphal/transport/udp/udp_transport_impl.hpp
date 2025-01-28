/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_UDP_TRANSPORT_IMPL_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_UDP_TRANSPORT_IMPL_HPP_INCLUDED

#include "delegate.hpp"
#include "media.hpp"
#include "msg_rx_session.hpp"
#include "msg_tx_session.hpp"
#include "rx_session_tree_node.hpp"
#include "svc_rx_sessions.hpp"
#include "svc_tx_sessions.hpp"
#include "tx_rx_sockets.hpp"
#include "udp_transport.hpp"

#include "libcyphal/executor.hpp"
#include "libcyphal/transport/contiguous_payload.hpp"
#include "libcyphal/transport/errors.hpp"
#include "libcyphal/transport/lizard_helpers.hpp"
#include "libcyphal/transport/msg_sessions.hpp"
#include "libcyphal/transport/session_tree.hpp"
#include "libcyphal/transport/svc_sessions.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>
#include <cetl/pf20/cetlpf.hpp>
#include <cetl/visit_helpers.hpp>
#include <udpard.h>

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
namespace udp
{

/// Internal implementation details of the UDP transport.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

/// @brief Represents final implementation class of the UDP transport.
///
class TransportImpl final : private TransportDelegate, public IUdpTransport
{
    /// @brief Defines private specification for making interface unique ptr.
    ///
    struct Spec : libcyphal::detail::UniquePtrSpec<IUdpTransport, TransportImpl>
    {
        // `explicit` here is in use to disable public construction of derived private `Spec` structs.
        // See https://seanmiddleditch.github.io/enabling-make-unique-with-private-constructors/
        explicit Spec() = default;
    };

    /// @brief Defines private storage of a media index, its interface, TX queue and socket.
    ///
    struct Media final
    {
    public:
        Media(const UdpardMemoryResource fragments_mr,
              const std::size_t          index,
              IMedia&                    interface,
              const UdpardNodeID* const  local_node_id,
              const std::size_t          tx_capacity)
            : index_{static_cast<std::uint8_t>(index)}
            , interface_{interface}
            , udpard_tx_{}
        {
            const UdpardTxMemoryResources tx_memory_resources = {fragments_mr, makeTxMemoryResource(interface)};
            const std::int8_t result = ::udpardTxInit(&udpard_tx_, local_node_id, tx_capacity, tx_memory_resources);
            CETL_DEBUG_ASSERT(result == 0, "There should be no path for an error here.");
            (void) result;
        }

        std::uint8_t index() const
        {
            return index_;
        }

        IMedia& interface() const
        {
            return interface_;
        }

        UdpardTx& udpard_tx()
        {
            return udpard_tx_;
        }

        SocketState<ITxSocket>& txSocketState()
        {
            return tx_socket_state_;
        }

        SocketState<IRxSocket>& svcRxSocketState()
        {
            return svc_rx_socket_state_;
        }

        std::size_t getTxSocketMtu() const noexcept
        {
            return tx_socket_state_.interface ? tx_socket_state_.interface->getMtu() : ITxSocket::DefaultMtu;
        }

    private:
        CETL_NODISCARD static UdpardMemoryResource makeTxMemoryResource(IMedia& media_interface)
        {
            using LizardHelpers = libcyphal::transport::detail::LizardHelpers;

            // TX memory resource is used for raw bytes block allocations only.
            // So it has no alignment requirements.
            constexpr std::size_t Alignment = 1;

            return LizardHelpers::makeMemoryResource<UdpardMemoryResource, Alignment>(
                media_interface.getTxMemoryResource());
        }

        const std::uint8_t     index_;
        IMedia&                interface_;
        UdpardTx               udpard_tx_;
        SocketState<ITxSocket> tx_socket_state_;
        SocketState<IRxSocket> svc_rx_socket_state_;

    };  // Media
    using MediaArray = libcyphal::detail::VarArray<Media>;

public:
    CETL_NODISCARD static Expected<UniquePtr<IUdpTransport>, FactoryFailure> make(  //
        const MemoryResourcesSpec& mem_res_spec,
        IExecutor&                 executor,
        const cetl::span<IMedia*>  media,
        const std::size_t          tx_capacity)
    {
        // Verify input arguments:
        // - At least one media interface must be provided, but no more than the maximum allowed (3).
        //
        const auto media_count = static_cast<std::size_t>(
            std::count_if(media.begin(), media.end(), [](const IMedia* const media_ptr) -> bool {
                return media_ptr != nullptr;
            }));
        if ((media_count == 0) || (media_count > UDPARD_NETWORK_INTERFACE_COUNT_MAX))
        {
            return ArgumentError{};
        }

        const MemoryResources memory_resources{mem_res_spec.general,
                                               makeUdpardMemoryResource(mem_res_spec.session, mem_res_spec.general),
                                               makeUdpardMemoryResource(mem_res_spec.fragment, mem_res_spec.general),
                                               makeUdpardMemoryDeleter(mem_res_spec.payload, mem_res_spec.general)};

        const UdpardNodeID unset_node_id = UDPARD_NODE_ID_UNSET;

        // False positive of clang-tidy - we move `media_array` to the `transport` instance, so can't make it const.
        // NOLINTNEXTLINE(misc-const-correctness)
        MediaArray media_array = makeMediaArray(memory_resources, media_count, media, &unset_node_id, tx_capacity);
        if (media_array.size() != media_count)
        {
            return MemoryError{};
        }

        auto transport = libcyphal::detail::makeUniquePtr<Spec>(memory_resources.general,
                                                                Spec{},
                                                                memory_resources,
                                                                executor,
                                                                std::move(media_array));
        if (transport == nullptr)
        {
            return MemoryError{};
        }

        return transport;
    }

    TransportImpl(const Spec, const MemoryResources& memory_resources, IExecutor& executor, MediaArray&& media_array)
        : TransportDelegate{memory_resources}
        , executor_{executor}
        , media_array_{std::move(media_array)}
        , msg_rx_session_nodes_{memory_resources.general}
        , svc_request_rx_session_nodes_{memory_resources.general}
        , svc_response_rx_session_nodes_{memory_resources.general}
    {
        for (auto& media : media_array_)
        {
            media.udpard_tx().local_node_id = &getNodeId();
        }
    }

    TransportImpl(const TransportImpl&)                = delete;
    TransportImpl(TransportImpl&&) noexcept            = delete;
    TransportImpl& operator=(const TransportImpl&)     = delete;
    TransportImpl& operator=(TransportImpl&&) noexcept = delete;

    ~TransportImpl()
    {
        for (Media& media : media_array_)
        {
            flushUdpardTxQueue(media.udpard_tx());
        }

        CETL_DEBUG_ASSERT(msg_rx_session_nodes_.isEmpty(),  //
                          "Message sessions must be destroyed before transport.");
        CETL_DEBUG_ASSERT(svc_request_rx_session_nodes_.isEmpty(),
                          "Service sessions must be destroyed before transport.");
        CETL_DEBUG_ASSERT(svc_response_rx_session_nodes_.isEmpty(),
                          "Service sessions must be destroyed before transport.");
    }

private:
    // MARK: IUdpTransport

    void setTransientErrorHandler(TransientErrorHandler handler) override
    {
        transient_error_handler_ = std::move(handler);
    }

    // MARK: ITransport

    CETL_NODISCARD cetl::optional<NodeId> getLocalNodeId() const noexcept override
    {
        if (getNodeId() > UDPARD_NODE_ID_MAX)
        {
            return cetl::nullopt;
        }

        return cetl::make_optional(getNodeId());
    }

    CETL_NODISCARD cetl::optional<ArgumentError> setLocalNodeId(const NodeId new_node_id) noexcept override
    {
        if (new_node_id > UDPARD_NODE_ID_MAX)
        {
            return ArgumentError{};
        }

        // Allow setting the same node ID multiple times, but only once otherwise.
        //
        if (getNodeId() == new_node_id)
        {
            return cetl::nullopt;
        }
        if (getNodeId() != UDPARD_NODE_ID_UNSET)
        {
            return ArgumentError{};
        }

        svc_rx_sockets_endpoint_ = setNodeId(new_node_id);

        return cetl::nullopt;
    }

    CETL_NODISCARD ProtocolParams getProtocolParams() const noexcept override
    {
        std::size_t min_mtu = std::numeric_limits<std::size_t>::max();
        for (const Media& media : media_array_)
        {
            min_mtu = std::min(min_mtu, media.getTxSocketMtu());
        }

        return ProtocolParams{std::numeric_limits<TransferId>::max(), min_mtu, UDPARD_NODE_ID_MAX + 1};
    }

    CETL_NODISCARD Expected<UniquePtr<IMessageRxSession>, AnyFailure> makeMessageRxSession(
        const MessageRxParams& params) override
    {
        return makeMsgRxSessionImpl(params, msg_rx_session_nodes_);
    }

    CETL_NODISCARD Expected<UniquePtr<IMessageTxSession>, AnyFailure> makeMessageTxSession(
        const MessageTxParams& params) override
    {
        auto failure = ensureMediaTxSockets();
        if (failure.has_value())
        {
            return std::move(failure.value());
        }

        return MessageTxSession::make(memoryResources().general, asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IRequestRxSession>, AnyFailure> makeRequestRxSession(
        const RequestRxParams& params) override
    {
        return makeSvcRxSessionImpl<IRequestRxSession, SvcRequestRxSession>(  //
            params,
            svc_request_rx_session_nodes_);
    }

    CETL_NODISCARD Expected<UniquePtr<IRequestTxSession>, AnyFailure> makeRequestTxSession(
        const RequestTxParams& params) override
    {
        auto failure = ensureMediaTxSockets();
        if (failure.has_value())
        {
            return std::move(failure.value());
        }

        return SvcRequestTxSession::make(memoryResources().general, asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IResponseRxSession>, AnyFailure> makeResponseRxSession(
        const ResponseRxParams& params) override
    {
        return makeSvcRxSessionImpl<IResponseRxSession, SvcResponseRxSession>(  //
            params,
            svc_response_rx_session_nodes_);
    }

    CETL_NODISCARD Expected<UniquePtr<IResponseTxSession>, AnyFailure> makeResponseTxSession(
        const ResponseTxParams& params) override
    {
        auto failure = ensureMediaTxSockets();
        if (failure.has_value())
        {
            return std::move(failure.value());
        }

        return SvcResponseTxSession::make(memoryResources().general, asDelegate(), params);
    }

    // MARK: TransportDelegate

    CETL_NODISCARD TransportDelegate& asDelegate()
    {
        return *this;
    }

    CETL_NODISCARD cetl::optional<AnyFailure> sendAnyTransfer(const AnyUdpardTxMetadata::Variant& tx_metadata_var,
                                                              const PayloadFragments payload_fragments) override
    {
        // Udpard currently does not support fragmented payloads (at `udpardTx[Publish|Request|Respond]`).
        // so we need to concatenate them when there are more than one non-empty fragment.
        // TODO: Make similar issue but for Udpard repo.
        // See https://github.com/OpenCyphal/libcanard/issues/223
        //
        const ContiguousPayload payload{memoryResources().general, payload_fragments};
        if ((payload.data() == nullptr) && (payload.size() > 0))
        {
            return MemoryError{};
        }

        for (Media& some_media : media_array_)
        {
            cetl::optional<AnyFailure> failure = withEnsureMediaTxSocket(  //
                some_media,
                [this, &tx_metadata_var, &payload](auto& media, auto& tx_socket) -> cetl::optional<AnyFailure> {
                    //
                    media.udpard_tx().mtu = tx_socket.getMtu();

                    const TxTransferHandler transfer_handler{*this, media, payload};
                    auto                    tx_failure = cetl::visit(transfer_handler, tx_metadata_var);
                    if (tx_failure.has_value())
                    {
                        return tx_failure;
                    }

                    // No need to try to send next frame when previous one hasn't finished yet.
                    if (!media.txSocketState().callback)
                    {
                        sendNextFrameToMediaTxSocket(media, tx_socket);
                    }
                    return cetl::nullopt;
                });
            if (failure.has_value())
            {
                // The handler (if any) just said that it's NOT fine to continue with transferring to
                // other media TX queues, and the error should not be ignored but propagated outside.
                return failure;
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
                            [this](const SessionEvent::MsgDestroyed& msg_session_destroyed) noexcept {
                                //
                                msg_rx_session_nodes_.removeNodeFor(msg_session_destroyed.params);
                            },
                            [this](const SessionEvent::SvcRequestDestroyed& req_session_destroyed) noexcept {
                                //
                                svc_request_rx_session_nodes_.removeNodeFor(req_session_destroyed.params);
                                cancelRxCallbacksIfNoSvcLeft();
                            },
                            [this](const SessionEvent::SvcResponseDestroyed& res_session_destroyed) noexcept {
                                //
                                svc_response_rx_session_nodes_.removeNodeFor(res_session_destroyed.params);
                                cancelRxCallbacksIfNoSvcLeft();
                            }),
                        event_var);
        });
        (void) result;
        CETL_DEBUG_ASSERT(result, "");
    }

    IRxSessionDelegate* tryFindRxSessionDelegateFor(const ResponseRxParams& params) override
    {
        if (auto* const node = svc_response_rx_session_nodes_.tryFindNodeFor(params))
        {
            return node->delegate();
        }
        return nullptr;
    }

    // MARK: Privates:

    using Self              = TransportImpl;
    using ContiguousPayload = transport::detail::ContiguousPayload;

    template <typename Node>
    using SessionTree = transport::detail::SessionTree<Node>;

    struct TxTransferHandler
    {
        // No Sonar `cpp:S5356` b/c we integrate here with libudpard raw C buffers.
        TxTransferHandler(const Self& self, Media& media, const ContiguousPayload& cont_payload)
            : self_{self}
            , media_{media}
            , payload_{cont_payload.size(), cont_payload.data()}  // NOSONAR cpp:S5356
        {
        }

        CETL_NODISCARD cetl::optional<AnyFailure> operator()(const AnyUdpardTxMetadata::Publish& tx_metadata) const
        {
            const std::int32_t result = ::udpardTxPublish(&media_.udpard_tx(),
                                                          tx_metadata.deadline_us,
                                                          tx_metadata.priority,
                                                          tx_metadata.subject_id,
                                                          tx_metadata.transfer_id,
                                                          payload_,
                                                          nullptr);

            return self_.tryHandleTransientUdpardResult<TransientErrorReport::UdpardTxPublish>(media_,
                                                                                               result,
                                                                                               media_.udpard_tx());
        }

        CETL_NODISCARD cetl::optional<AnyFailure> operator()(const AnyUdpardTxMetadata::Request& tx_metadata) const
        {
            const std::int32_t result = ::udpardTxRequest(&media_.udpard_tx(),
                                                          tx_metadata.deadline_us,
                                                          tx_metadata.priority,
                                                          tx_metadata.service_id,
                                                          tx_metadata.server_node_id,
                                                          tx_metadata.transfer_id,
                                                          payload_,
                                                          nullptr);

            return self_.tryHandleTransientUdpardResult<TransientErrorReport::UdpardTxRequest>(media_,
                                                                                               result,
                                                                                               media_.udpard_tx());
        }

        CETL_NODISCARD cetl::optional<AnyFailure> operator()(const AnyUdpardTxMetadata::Respond& tx_metadata) const
        {
            const std::int32_t result = ::udpardTxRespond(&media_.udpard_tx(),
                                                          tx_metadata.deadline_us,
                                                          tx_metadata.priority,
                                                          tx_metadata.service_id,
                                                          tx_metadata.client_node_id,
                                                          tx_metadata.transfer_id,
                                                          payload_,
                                                          nullptr);

            return self_.tryHandleTransientUdpardResult<TransientErrorReport::UdpardTxRespond>(media_,
                                                                                               result,
                                                                                               media_.udpard_tx());
        }

    private:
        const Self&                self_;
        Media&                     media_;
        const struct UdpardPayload payload_;

    };  // TxTransferHandler

    CETL_NODISCARD auto makeMsgRxSessionImpl(  //
        const MessageRxParams&                   params,
        SessionTree<RxSessionTreeNode::Message>& tree_nodes) -> Expected<UniquePtr<IMessageRxSession>, AnyFailure>
    {
        // Make sure that session is unique per given parameters.
        // For message sessions, the uniqueness is based on the subject ID.
        //
        auto node_result = tree_nodes.ensureNodeFor<true>(params);  // should be new
        if (auto* const failure = cetl::get_if<AnyFailure>(&node_result))
        {
            return std::move(*failure);
        }
        auto& new_msg_node = cetl::get<RxSessionTreeNode::Message::RefWrapper>(node_result).get();

        auto session_result = MessageRxSession::make(memoryResources().general, asDelegate(), params, new_msg_node);
        if (auto* const failure = cetl::get_if<AnyFailure>(&session_result))
        {
            tree_nodes.removeNodeFor(params);
            return std::move(*failure);
        }

        // Try to create all (per each media) RX sockets for message subscription.
        // For now, we're just creating them, without any attempt to use them yet - hence the "do nothing" action.
        //
        auto media_failure = withMediaMsgRxSockets(  //
            new_msg_node,
            [this](const auto& media, auto& socket_state, auto& subscription, auto& session_delegate)
                -> cetl::optional<AnyFailure> {
                //
                if (!socket_state.callback)
                {
                    socket_state.callback = socket_state.interface->registerCallback(
                        [this, &media, &socket_state, &subscription, &session_delegate](const auto&) {
                            //
                            receiveNextMessageFrame(media, socket_state, subscription, session_delegate);
                        });
                }
                return cetl::nullopt;
            });
        if (media_failure.has_value())
        {
            return std::move(media_failure.value());
        }

        return session_result;
    }

    template <typename Interface, typename Concrete, typename Params, typename Tree>
    CETL_NODISCARD auto makeSvcRxSessionImpl(  //
        const Params& params,
        Tree&         tree_nodes) -> Expected<UniquePtr<Interface>, AnyFailure>
    {
        // Try to create all (per each media) shared RX sockets for services.
        // For now, we're just creating them, without any attempt to use them yet - hence the "do nothing" action.
        //
        auto media_failure = withMediaSvcRxSockets([this](auto& media,
                                                          auto& socket_state) -> cetl::optional<AnyFailure> {  //
            if (!socket_state.callback)
            {
                socket_state.callback = socket_state.interface->registerCallback([this, &media, &socket_state](auto) {
                    //
                    receiveNextServiceFrame(media, socket_state);
                });
            }
            return cetl::nullopt;
        });
        if (media_failure.has_value())
        {
            return std::move(media_failure.value());
        }

        // Make sure that session is unique per given parameters.
        // For request sessions, the uniqueness is based on the service ID.
        // For response sessions, the uniqueness is based on the service ID and the server node ID.
        //
        auto node_result = tree_nodes.template ensureNodeFor<true>(params);  // should be new
        if (auto* const failure = cetl::get_if<AnyFailure>(&node_result))
        {
            return std::move(*failure);
        }
        auto& new_svc_node = cetl::get<typename Tree::NodeBase::RefWrapper>(node_result).get();

        auto session_result = Concrete::make(memoryResources().general, asDelegate(), params, new_svc_node);
        if (nullptr != cetl::get_if<AnyFailure>(&session_result))
        {
            // We failed to create the session, so we need to release the unique node.
            // The sockets we made earlier will be released in the destructor of whole transport.
            tree_nodes.removeNodeFor(params);
        }

        return session_result;
    }

    template <typename Report, typename ErrorVariant, typename Culprit>
    cetl::optional<AnyFailure> tryHandleTransientMediaError(const Media&   media,
                                                            ErrorVariant&& error_var,
                                                            Culprit&&      culprit)
    {
        auto failure = libcyphal::detail::upcastVariant<AnyFailure>(std::forward<ErrorVariant>(error_var));
        if (!transient_error_handler_)
        {
            return failure;
        }

        TransientErrorReport::Variant report_var{
            Report{std::move(failure), media.index(), std::forward<Culprit>(culprit)}};

        return transient_error_handler_(report_var);
    }

    template <typename Report, typename Culprit>
    CETL_NODISCARD cetl::optional<AnyFailure> tryHandleTransientUdpardResult(const Media&       media,
                                                                             const std::int32_t result,
                                                                             Culprit&&          culprit) const
    {
        cetl::optional<AnyFailure> failure = optAnyFailureFromUdpard(result);
        if (failure.has_value() && transient_error_handler_)
        {
            TransientErrorReport::Variant report_var{
                Report{std::move(failure.value()), media.index(), std::forward<Culprit>(culprit)}};

            failure = transient_error_handler_(report_var);
        }
        return failure;
    }

    CETL_NODISCARD static MediaArray makeMediaArray(const MemoryResources&    memory,
                                                    const std::size_t         media_count,
                                                    const cetl::span<IMedia*> media_interfaces,
                                                    const UdpardNodeID* const local_node_id_,
                                                    const std::size_t         tx_capacity)
    {
        MediaArray media_array{media_count, &memory.general};

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
                    media_array.emplace_back(memory.fragment, index, media, local_node_id_, tx_capacity);
                    index++;
                }
            }
            CETL_DEBUG_ASSERT(index == media_count, "");
            CETL_DEBUG_ASSERT(media_array.size() == media_count, "");
        }

        return media_array;
    }

    /// @brief Tries to run an action with media and its TX socket (the latter one is made on demand if necessary).
    ///
    template <typename Action>
    CETL_NODISCARD cetl::optional<AnyFailure> withEnsureMediaTxSocket(Media& media, Action&& action)
    {
        if (!media.txSocketState().interface)
        {
            using ErrorReport = TransientErrorReport::MediaMakeTxSocket;

            auto tx_socket_result = media.interface().makeTxSocket();
            if (auto* const failure = cetl::get_if<IMedia::MakeTxSocketResult::Failure>(&tx_socket_result))
            {
                return tryHandleTransientMediaError<ErrorReport>(media, std::move(*failure), media.interface());
            }

            media.txSocketState().interface =
                cetl::get<IMedia::MakeTxSocketResult::Success>(std::move(tx_socket_result));
            if (!media.txSocketState().interface)
            {
                return tryHandleTransientMediaError<ErrorReport, cetl::variant<MemoryError>>(media,
                                                                                             MemoryError{},
                                                                                             media.interface());
            }
        }

        return std::forward<Action>(action)(media, *(media.txSocketState().interface));
    }

    CETL_NODISCARD cetl::optional<AnyFailure> ensureMediaTxSockets()
    {
        for (Media& media : media_array_)
        {
            cetl::optional<AnyFailure> failure =
                withEnsureMediaTxSocket(media, [](auto&, auto&) -> cetl::nullopt_t { return cetl::nullopt; });
            if (failure.has_value())
            {
                return failure;
            }
        }

        return cetl::nullopt;
    }

    static void flushUdpardTxQueue(UdpardTx& udpard_tx)
    {
        while (UdpardTxItem* const maybe_item = ::udpardTxPeek(&udpard_tx))
        {
            UdpardTxItem* const item = ::udpardTxPop(&udpard_tx, maybe_item);
            ::udpardTxFree(udpard_tx.memory, item);
        }
    }

    /// @brief Tries to send next frame from media TX queue to socket.
    ///
    void sendNextFrameToMediaTxSocket(Media& media, ITxSocket& tx_socket)
    {
        using PayloadFragment = cetl::span<const cetl::byte>;

        TimePoint tx_deadline;
        while (UdpardTxItem* const tx_item = peekFirstValidTxItem(media.udpard_tx(), tx_deadline))
        {
            // No Sonar `cpp:S5356` and `cpp:S5357` b/c we integrate here with C libudpard API.
            const auto* const buffer =
                static_cast<const cetl::byte*>(tx_item->datagram_payload.data);  // NOSONAR cpp:S5356 cpp:S5357
            const std::array<PayloadFragment, 1> single_payload_fragment{
                PayloadFragment{buffer, tx_item->datagram_payload.size}};

            ITxSocket::SendResult::Type send_result =
                tx_socket.send(tx_deadline,
                               {tx_item->destination.ip_address, tx_item->destination.udp_port},
                               tx_item->dscp,
                               single_payload_fragment);

            // In case of socket send error we are going to drop this problematic frame
            // (b/c it looks like media TX socket can't handle this frame),
            // but we will continue to try process other transfer frame.
            // Note that socket not being ready/able to send a frame just yet (aka temporary)
            // is not reported as an error (see `is_accepted` below).
            //
            auto* const send_failure = cetl::get_if<ITxSocket::SendResult::Failure>(&send_result);
            if (nullptr == send_failure)
            {
                const auto sent = cetl::get<ITxSocket::SendResult::Success>(send_result);
                if (sent.is_accepted)
                {
                    popAndFreeUdpardTxItem(&media.udpard_tx(), tx_item, false /* single frame */);
                }

                // If needed schedule (recursively!) next frame for sending.
                // Already existing callback will be called by executor when TX socket is ready to send more.
                //
                if (!media.txSocketState().callback)
                {
                    media.txSocketState().callback =
                        tx_socket.registerCallback([this, &media, &tx_socket](const auto&) {
                            //
                            sendNextFrameToMediaTxSocket(media, tx_socket);
                        });
                }
                return;
            }

            // Release whole problematic transfer from the TX queue,
            // so that other transfers in TX queue have their chance.
            // Otherwise, we would be stuck in an execution loop trying to send the same frame.
            popAndFreeUdpardTxItem(&media.udpard_tx(), tx_item, true /* whole transfer */);

            using Report = TransientErrorReport::MediaTxSocketSend;
            (void) tryHandleTransientMediaError<Report>(media, std::move(*send_failure), tx_socket);

        }  // for a valid tx item

        // There is nothing to send anymore, so we are done with this media TX socket - no more callbacks for now.
        media.txSocketState().callback.reset();
    }

    /// @brief Tries to peek the first TX item from the media TX queue which is not expired.
    ///
    /// While searching, any of already expired TX items are pop from the queue and freed (aka dropped).
    /// If there is no still valid TX items in the queue, returns `nullptr`.
    ///
    CETL_NODISCARD UdpardTxItem* peekFirstValidTxItem(UdpardTx& udpard_tx, TimePoint& out_deadline) const
    {
        const TimePoint now = executor_.now();

        while (UdpardTxItem* const tx_item = ::udpardTxPeek(&udpard_tx))
        {
            // We are dropping any TX item that has expired.
            // Otherwise, we would send it to the media TX socket interface.
            // We use strictly `<` (instead of `<=`) to give this frame a chance (one extra 1us) at the socket.
            //
            const auto deadline = TimePoint{std::chrono::microseconds{tx_item->deadline_usec}};
            if (now < deadline)
            {
                out_deadline = deadline;
                return tx_item;
            }

            // Release whole expired transfer b/c possible next frames of the same transfer are also expired.
            popAndFreeUdpardTxItem(&udpard_tx, tx_item, true /* whole transfer */);
        }
        return nullptr;
    }

    /// @brief Tries to call an action with a media and its RX socket.
    ///
    /// The RX socket is made on demand if necessary (but `endpoint` parameter should have a value).
    ///
    template <typename Action, typename... Args>
    CETL_NODISCARD cetl::optional<AnyFailure> withEnsureMediaRxSocket(Media&                           media,
                                                                      const cetl::optional<IpEndpoint> endpoint,
                                                                      SocketState<IRxSocket>&          socket_state,
                                                                      Action&&                         action,
                                                                      Args&&... args)
    {
        if (nullptr == socket_state.interface)
        {
            // Missing endpoint for service RX sockets means that the local node ID is not set yet,
            // So, node can't be as a destination for any incoming frames - hence nothing to receive.
            //
            if (!endpoint.has_value())
            {
                return cetl::nullopt;
            }

            using ErrorReport = TransientErrorReport::MediaMakeRxSocket;

            auto rx_socket_result = media.interface().makeRxSocket(endpoint.value());
            if (auto* const failure = cetl::get_if<IMedia::MakeRxSocketResult::Failure>(&rx_socket_result))
            {
                return tryHandleTransientMediaError<ErrorReport>(media, std::move(*failure), media.interface());
            }

            socket_state.interface = cetl::get<IMedia::MakeRxSocketResult::Success>(std::move(rx_socket_result));
            if (nullptr == socket_state.interface)
            {
                return tryHandleTransientMediaError<ErrorReport, cetl::variant<MemoryError>>(media,
                                                                                             MemoryError{},
                                                                                             media.interface());
            }
        }

        return std::forward<Action>(action)(media, socket_state, std::forward<Args>(args)...);
    }

    template <typename Action>
    CETL_NODISCARD cetl::optional<AnyFailure> withMediaMsgRxSockets(RxSessionTreeNode::Message& msg_rx_node,
                                                                    const Action&               action)
    {
        IMsgRxSessionDelegate* const session_delegate = msg_rx_node.delegate();
        if (nullptr != session_delegate)
        {
            auto&      subscription = session_delegate->getSubscription();
            const auto endpoint =
                cetl::optional<IpEndpoint>{IpEndpoint::fromUdpardEndpoint(subscription.udp_ip_endpoint)};

            for (Media& media : media_array_)
            {
                cetl::optional<AnyFailure> failure = withEnsureMediaRxSocket(media,
                                                                             endpoint,
                                                                             msg_rx_node.socketState(media.index()),
                                                                             action,
                                                                             subscription,
                                                                             *session_delegate);
                if (failure.has_value())
                {
                    return failure;
                }
            }
        }

        return cetl::nullopt;
    }

    template <typename Action>
    CETL_NODISCARD cetl::optional<AnyFailure> withMediaSvcRxSockets(const Action& action)
    {
        for (Media& media : media_array_)
        {
            cetl::optional<AnyFailure> failure =
                withEnsureMediaRxSocket(media, svc_rx_sockets_endpoint_, media.svcRxSocketState(), action);
            if (failure.has_value())
            {
                return failure;
            }
        }

        return cetl::nullopt;
    }

    CETL_NODISCARD IRxSocket::ReceiveResult::Success tryReceiveFromRxSocket(const Media&            media,
                                                                            SocketState<IRxSocket>& socket_state)
    {
        if (!socket_state.interface)
        {
            return cetl::nullopt;
        }
        auto& rx_socket = *socket_state.interface;

        IRxSocket::ReceiveResult::Type receive_result = rx_socket.receive();
        if (auto* const failure = cetl::get_if<IRxSocket::ReceiveResult::Failure>(&receive_result))
        {
            using RxSocketReport = TransientErrorReport::MediaRxSocketReceive;
            (void) tryHandleTransientMediaError<RxSocketReport>(media, std::move(*failure), rx_socket);
            return cetl::nullopt;
        }
        return cetl::get<IRxSocket::ReceiveResult::Success>(std::move(receive_result));
    }

    void receiveNextServiceFrame(const Media& media, SocketState<IRxSocket>& socket_state)
    {
        // 1. Try to receive a frame from the media RX socket.
        //
        auto opt_rx_meta = tryReceiveFromRxSocket(media, socket_state);
        if (!opt_rx_meta)
        {
            return;
        }
        auto& rx_meta = *opt_rx_meta;

        // 2. We've got a new frame from the media RX socket, so let's try to pass it into libudpard RPC dispatcher.

        const auto timestamp_us =
            std::chrono::duration_cast<std::chrono::microseconds>(rx_meta.timestamp.time_since_epoch());

        const auto payload_deleter = rx_meta.payload_ptr.get_deleter();

        // TODO: Currently we expect that user allocates payload memory from the specific PMR (the "payload" one).
        // Later we will pass users deleter into lizards (along with moved buffer),
        // so such requirement will be lifted (see https://github.com/OpenCyphal-Garage/libcyphal/issues/352).
        //
        CETL_DEBUG_ASSERT(payload_deleter.resource() == memoryResources().payload.user_reference,
                          "PMR of deleter is expected to be the same as the payload memory resource.");

        UdpardRxRPCTransfer out_transfer{};
        UdpardRxRPCPort*    out_port{nullptr};

        const std::int8_t result =
            ::udpardRxRPCDispatcherReceive(&getUdpardRpcDispatcher(),
                                           static_cast<UdpardMicrosecond>(timestamp_us.count()),
                                           // Udpard takes ownership of the payload buffer,
                                           // regardless of the result of the operation - hence the `.release()`.
                                           {payload_deleter.size(), rx_meta.payload_ptr.release()},
                                           media.index(),
                                           &out_port,
                                           &out_transfer);

        // 3. We might have result RX transfer (built from fragments by libudpard).
        //    If so, we need to pass it to the session delegate for storing.
        //
        using DispatcherReport = TransientErrorReport::UdpardRxSvcReceive;
        const auto failure = tryHandleTransientUdpardResult<DispatcherReport>(media, result, getUdpardRpcDispatcher());
        if ((!failure.has_value()) && (result > 0))
        {
            CETL_DEBUG_ASSERT(out_port != nullptr, "Expected subscription.");
            CETL_DEBUG_ASSERT(out_port->user_reference != nullptr, "Expected session delegate.");

            // No Sonar `cpp:S5357` b/c the raw `user_reference` is part of libudpard api,
            // and it was set by us at a RX session constructor (see f.e. `MessageRxSession` ctor).
            auto* const session_delegate =
                static_cast<IRxSessionDelegate*>(out_port->user_reference);  // NOSONAR cpp:S5357

            const auto transfer_id = out_transfer.base.transfer_id;
            const auto priority    = static_cast<Priority>(out_transfer.base.priority);
            const auto timestamp   = TimePoint{std::chrono::microseconds{out_transfer.base.timestamp_usec}};

            session_delegate->acceptRxTransfer(UdpardMemory{memoryResources(), out_transfer.base},
                                               TransferRxMetadata{{transfer_id, priority}, timestamp},
                                               out_transfer.base.source_node_id);
        }
    }

    void receiveNextMessageFrame(const Media&            media,
                                 SocketState<IRxSocket>& socket_state,
                                 UdpardRxSubscription&   subscription,
                                 IRxSessionDelegate&     session_delegate)
    {
        // 1. Try to receive a frame from the media RX socket.
        //
        auto opt_rx_meta = tryReceiveFromRxSocket(media, socket_state);
        if (!opt_rx_meta)
        {
            return;
        }
        auto& rx_meta = *opt_rx_meta;

        // 2. We've got a new frame from the media RX socket, so let's try to pass it into libudpard subscription.

        const auto timestamp_us =
            std::chrono::duration_cast<std::chrono::microseconds>(rx_meta.timestamp.time_since_epoch());

        const auto payload_deleter = rx_meta.payload_ptr.get_deleter();

        // TODO: Currently we expect that user allocates payload memory from the specific PMR (the "payload" one).
        // Later we will pass users deleter into lizards (along with moved buffer),
        // so such requirement will be lifted (see https://github.com/OpenCyphal-Garage/libcyphal/issues/352).
        //
        CETL_DEBUG_ASSERT(payload_deleter.resource() == memoryResources().payload.user_reference,
                          "PMR of deleter is expected to be the same as the payload memory resource.");

        UdpardRxTransfer out_transfer{};

        const std::int8_t result =
            ::udpardRxSubscriptionReceive(&subscription,
                                          static_cast<UdpardMicrosecond>(timestamp_us.count()),
                                          // Udpard takes ownership of the payload buffer,
                                          // regardless of the result of the operation - hence the `.release()`.
                                          {payload_deleter.size(), rx_meta.payload_ptr.release()},
                                          media.index(),
                                          &out_transfer);

        // 3. We might have result RX transfer (built from fragments by libudpard).
        //    If so, we need to pass it to the session delegate for storing.
        //
        using SubscriptionReport = TransientErrorReport::UdpardRxMsgReceive;
        const auto failure       = tryHandleTransientUdpardResult<SubscriptionReport>(media, result, subscription);
        if ((!failure.has_value()) && (result > 0))
        {
            const auto transfer_id = out_transfer.transfer_id;
            const auto priority    = static_cast<Priority>(out_transfer.priority);
            const auto timestamp   = TimePoint{std::chrono::microseconds{out_transfer.timestamp_usec}};

            session_delegate.acceptRxTransfer(UdpardMemory{memoryResources(), out_transfer},
                                              TransferRxMetadata{{transfer_id, priority}, timestamp},
                                              out_transfer.source_node_id);
        }
    }

    void cancelRxCallbacksIfNoSvcLeft() noexcept
    {
        if (svc_request_rx_session_nodes_.isEmpty() && svc_response_rx_session_nodes_.isEmpty())
        {
            for (Media& media : media_array_)
            {
                media.svcRxSocketState().callback.reset();
            }
        }
    }

    // MARK: Data members:

    IExecutor&                               executor_;
    MediaArray                               media_array_;
    TransientErrorHandler                    transient_error_handler_;
    SessionTree<RxSessionTreeNode::Message>  msg_rx_session_nodes_;
    SessionTree<RxSessionTreeNode::Request>  svc_request_rx_session_nodes_;
    SessionTree<RxSessionTreeNode::Response> svc_response_rx_session_nodes_;
    cetl::optional<IpEndpoint>               svc_rx_sockets_endpoint_;

};  // TransportImpl

}  // namespace detail

/// @brief Makes a new UDP transport instance.
///
/// NB! Lifetime of the transport instance must never outlive memory resources, `media` and `multiplexer` instances.
///
/// @param mem_res_spec Specification of polymorphic memory resources to use for all allocations.
/// @param executor Interface of the executor to use.
/// @param media Collection of redundant media interfaces to use.
/// @param tx_capacity Total number of frames that can be queued for transmission per `IMedia` instance.
/// @return Unique pointer to the new UDP transport instance or a failure.
///
inline Expected<UniquePtr<IUdpTransport>, FactoryFailure> makeTransport(const MemoryResourcesSpec& mem_res_spec,
                                                                        IExecutor&                 executor,
                                                                        const cetl::span<IMedia*>  media,
                                                                        const std::size_t          tx_capacity)
{
    return detail::TransportImpl::make(mem_res_spec, executor, media, tx_capacity);
}

}  // namespace udp
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_UDP_TRANSPORT_IMPL_HPP_INCLUDED
