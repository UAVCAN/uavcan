/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_CAN_MSG_RX_SESSION_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_CAN_MSG_RX_SESSION_HPP_INCLUDED

#include "delegate.hpp"

#include "libcyphal/errors.hpp"
#include "libcyphal/transport/errors.hpp"
#include "libcyphal/transport/msg_sessions.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <canard.h>
#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
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

/// @brief A class to represent a message subscriber RX session.
///
class MessageRxSession final : private IRxSessionDelegate, public IMessageRxSession
{
    /// @brief Defines private specification for making interface unique ptr.
    ///
    struct Spec : libcyphal::detail::UniquePtrSpec<IMessageRxSession, MessageRxSession>
    {
        // `explicit` here is in use to disable public construction of derived private `Spec` structs.
        // See https://seanmiddleditch.github.io/enabling-make-unique-with-private-constructors/
        explicit Spec() = default;
    };

public:
    CETL_NODISCARD static Expected<UniquePtr<IMessageRxSession>, AnyFailure> make(  //
        cetl::pmr::memory_resource& memory,
        TransportDelegate&          delegate,
        const MessageRxParams&      params)
    {
        if (params.subject_id > CANARD_SUBJECT_ID_MAX)
        {
            return ArgumentError{};
        }

        auto session = libcyphal::detail::makeUniquePtr<Spec>(memory, Spec{}, delegate, params);
        if (session == nullptr)
        {
            return MemoryError{};
        }

        return session;
    }

    MessageRxSession(const Spec, TransportDelegate& delegate, const MessageRxParams& params)
        : delegate_{delegate}
        , params_{params}
        , subscription_{}
    {
        delegate.listenForRxSubscription(subscription_, params);

        // No Sonar `cpp:S5356` b/c we integrate here with C libcanard API.
        subscription_.user_reference = static_cast<IRxSessionDelegate*>(this);  // NOSONAR cpp:S5356

        delegate_.onSessionEvent(TransportDelegate::SessionEvent::MsgCreated{});
    }

    MessageRxSession(const MessageRxSession&)                = delete;
    MessageRxSession(MessageRxSession&&) noexcept            = delete;
    MessageRxSession& operator=(const MessageRxSession&)     = delete;
    MessageRxSession& operator=(MessageRxSession&&) noexcept = delete;

    ~MessageRxSession()
    {
        delegate_.cancelRxSubscriptionFor(subscription_, CanardTransferKindMessage);
        delegate_.onSessionEvent(TransportDelegate::SessionEvent::MsgDestroyed{});
    }

private:
    // MARK: IMessageRxSession

    CETL_NODISCARD MessageRxParams getParams() const noexcept override
    {
        return params_;
    }

    CETL_NODISCARD cetl::optional<MessageRxTransfer> receive() override
    {
        if (last_rx_transfer_)
        {
            auto transfer = std::move(*last_rx_transfer_);
            last_rx_transfer_.reset();
            return transfer;
        }
        return cetl::nullopt;
    }

    void setOnReceiveCallback(OnReceiveCallback::Function&& function) override
    {
        on_receive_cb_fn_ = std::move(function);
    }

    // MARK: IRxSession

    void setTransferIdTimeout(const Duration timeout) override
    {
        const auto timeout_us = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
        if (timeout_us >= Duration::zero())
        {
            subscription_.transfer_id_timeout_usec = static_cast<CanardMicrosecond>(timeout_us.count());
        }
    }

    // MARK: IRxSessionDelegate

    void acceptRxTransfer(CanardMemory&&            canard_memory,
                          const TransferRxMetadata& rx_metadata,
                          const NodeId              source_node_id) override
    {
        const cetl::optional<NodeId> publisher_node_id =
            source_node_id > CANARD_NODE_ID_MAX ? cetl::nullopt : cetl::make_optional(source_node_id);

        const MessageRxMetadata meta{rx_metadata, publisher_node_id};
        MessageRxTransfer       msg_rx_transfer{meta, ScatteredBuffer{std::move(canard_memory)}};
        if (on_receive_cb_fn_)
        {
            on_receive_cb_fn_(OnReceiveCallback::Arg{msg_rx_transfer});
            return;
        }
        (void) last_rx_transfer_.emplace(std::move(msg_rx_transfer));
    }

    // MARK: Data members:

    TransportDelegate&                delegate_;
    const MessageRxParams             params_;
    CanardRxSubscription              subscription_;
    cetl::optional<MessageRxTransfer> last_rx_transfer_;
    OnReceiveCallback::Function       on_receive_cb_fn_;

};  // MessageRxSession

}  // namespace detail
}  // namespace can
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_CAN_MSG_RX_SESSION_HPP_INCLUDED
