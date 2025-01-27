/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_CAN_SVC_RX_SESSIONS_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_CAN_SVC_RX_SESSIONS_HPP_INCLUDED

#include "delegate.hpp"

#include "libcyphal/errors.hpp"
#include "libcyphal/transport/errors.hpp"
#include "libcyphal/transport/svc_sessions.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <canard.h>
#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>

#include <chrono>
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

/// @brief A template class to represent a service request/response RX session (both for server and client sides).
///
/// @tparam Interface_ Type of the session interface.
///                    Could be either `IRequestRxSession` or `IResponseRxSession`.
/// @tparam Params Type of the session parameters.
///                Could be either `RequestRxParams` or `ResponseRxParams`.
/// @tparam TransferKind Kind of the service transfer.
///                      Could be either `CanardTransferKindRequest` or `CanardTransferKindResponse`.
///
template <typename Interface_, typename Params, CanardTransferKind TransferKind>
class SvcRxSession final : private IRxSessionDelegate, public Interface_
{
    /// @brief Defines private specification for making interface unique ptr.
    ///
    struct Spec : libcyphal::detail::UniquePtrSpec<Interface_, SvcRxSession>
    {
        // `explicit` here is in use to disable public construction of derived private `Spec` structs.
        // See https://seanmiddleditch.github.io/enabling-make-unique-with-private-constructors/
        explicit Spec() = default;
    };

public:
    CETL_NODISCARD static Expected<UniquePtr<Interface_>, AnyFailure> make(TransportDelegate& delegate,
                                                                           const Params&      params)
    {
        if (params.service_id > CANARD_SERVICE_ID_MAX)
        {
            return ArgumentError{};
        }

        auto session = libcyphal::detail::makeUniquePtr<Spec>(delegate.memory(), Spec{}, delegate, params);
        if (session == nullptr)
        {
            return MemoryError{};
        }

        return session;
    }

    SvcRxSession(const Spec, TransportDelegate& delegate, const Params& params)
        : delegate_{delegate}
        , params_{params}
        , subscription_{}
    {
        const std::int8_t result = ::canardRxSubscribe(&delegate.canardInstance(),
                                                       TransferKind,
                                                       params_.service_id,
                                                       params_.extent_bytes,
                                                       CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
                                                       &subscription_);
        (void) result;
        CETL_DEBUG_ASSERT(result >= 0, "There is no way currently to get an error here.");
        CETL_DEBUG_ASSERT(result > 0, "New subscription supposed to be made.");

        // No Sonar `cpp:S5356` b/c we integrate here with C libcanard API.
        subscription_.user_reference = static_cast<IRxSessionDelegate*>(this);  // NOSONAR cpp:S5356

        delegate_.onSessionEvent(TransportDelegate::SessionEvent::SvcRxLifetime{true /* is_added */});
    }

    SvcRxSession(const SvcRxSession&)                = delete;
    SvcRxSession(SvcRxSession&&) noexcept            = delete;
    SvcRxSession& operator=(const SvcRxSession&)     = delete;
    SvcRxSession& operator=(SvcRxSession&&) noexcept = delete;

    ~SvcRxSession()
    {
        const std::int8_t result = ::canardRxUnsubscribe(&delegate_.canardInstance(), TransferKind, params_.service_id);
        (void) result;
        CETL_DEBUG_ASSERT(result >= 0, "There is no way currently to get an error here.");
        CETL_DEBUG_ASSERT(result > 0, "Subscription supposed to be made at constructor.");

        delegate_.onSessionEvent(TransportDelegate::SessionEvent::SvcRxLifetime{false /* is_added */});
    }

private:
    // MARK: Interface

    CETL_NODISCARD Params getParams() const noexcept override
    {
        return params_;
    }

    CETL_NODISCARD cetl::optional<ServiceRxTransfer> receive() override
    {
        if (last_rx_transfer_)
        {
            auto transfer = std::move(*last_rx_transfer_);
            last_rx_transfer_.reset();
            return transfer;
        }
        return cetl::nullopt;
    }

    void setOnReceiveCallback(ISvcRxSession::OnReceiveCallback::Function&& function) override
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

    void acceptRxTransfer(const CanardRxTransfer& transfer) override
    {
        const auto priority       = static_cast<Priority>(transfer.metadata.priority);
        const auto remote_node_id = static_cast<NodeId>(transfer.metadata.remote_node_id);
        const auto transfer_id    = static_cast<TransferId>(transfer.metadata.transfer_id);
        const auto timestamp      = TimePoint{std::chrono::microseconds{transfer.timestamp_usec}};

        // No Sonar `cpp:S5356` and `cpp:S5357` b/c we need to pass raw data from C libcanard api.
        auto* const buffer = static_cast<cetl::byte*>(transfer.payload.data);  // NOSONAR cpp:S5356 cpp:S5357
        TransportDelegate::CanardMemory canard_memory{delegate_,
                                                      transfer.payload.allocated_size,
                                                      buffer,
                                                      transfer.payload.size};

        const ServiceRxMetadata meta{{{transfer_id, priority}, timestamp}, remote_node_id};
        ServiceRxTransfer       svc_rx_transfer{meta, ScatteredBuffer{std::move(canard_memory)}};
        if (on_receive_cb_fn_)
        {
            on_receive_cb_fn_(ISvcRxSession::OnReceiveCallback::Arg{svc_rx_transfer});
            return;
        }
        (void) last_rx_transfer_.emplace(std::move(svc_rx_transfer));
    }

    // MARK: Data members:

    TransportDelegate&                         delegate_;
    const Params                               params_;
    CanardRxSubscription                       subscription_;
    cetl::optional<ServiceRxTransfer>          last_rx_transfer_;
    ISvcRxSession::OnReceiveCallback::Function on_receive_cb_fn_;

};  // SvcRxSession

// MARK: -

/// @brief A concrete class to represent a service request RX session (aka server side).
///
using SvcRequestRxSession = SvcRxSession<IRequestRxSession, RequestRxParams, CanardTransferKindRequest>;

/// @brief A concrete class to represent a service response RX session (aka client side).
///
using SvcResponseRxSession = SvcRxSession<IResponseRxSession, ResponseRxParams, CanardTransferKindResponse>;

}  // namespace detail
}  // namespace can
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_CAN_SVC_RX_SESSIONS_HPP_INCLUDED
