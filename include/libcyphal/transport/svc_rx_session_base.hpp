/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_SVC_RX_SESSION_BASE_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_SVC_RX_SESSION_BASE_HPP_INCLUDED

#include "scattered_buffer.hpp"

#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>

#include <utility>

namespace libcyphal
{
namespace transport
{

/// Internal implementation details of a transport.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

/// @brief A base template class to represent a service RX session.
///
/// Should be suitable for any transport.
///
template <typename Interface,
          typename IRxSessionDelegate,
          typename TransportDelegate,
          typename Params,
          typename LizardMemory>
class SvcRxSessionBase : public IRxSessionDelegate, public Interface
{
public:
    SvcRxSessionBase(TransportDelegate& delegate, const Params& params)
        : delegate_{delegate}
        , params_{params}
    {
    }

    SvcRxSessionBase(const SvcRxSessionBase&)                = delete;
    SvcRxSessionBase(SvcRxSessionBase&&) noexcept            = delete;
    SvcRxSessionBase& operator=(const SvcRxSessionBase&)     = delete;
    SvcRxSessionBase& operator=(SvcRxSessionBase&&) noexcept = delete;

protected:
    ~SvcRxSessionBase() = default;

    TransportDelegate& delegate()
    {
        return delegate_;
    }

    // MARK: Interface

    CETL_NODISCARD Params getParams() const noexcept final
    {
        return params_;
    }

    CETL_NODISCARD cetl::optional<ServiceRxTransfer> receive() final
    {
        if (last_rx_transfer_)
        {
            auto transfer = std::move(*last_rx_transfer_);
            last_rx_transfer_.reset();
            return transfer;
        }
        return cetl::nullopt;
    }

    void setOnReceiveCallback(ISvcRxSession::OnReceiveCallback::Function&& function) final
    {
        on_receive_cb_fn_ = std::move(function);
    }

    // MARK: IRxSessionDelegate

    void acceptRxTransfer(LizardMemory&&            lizard_memory,
                          const TransferRxMetadata& rx_metadata,
                          const NodeId              source_node_id) final
    {
        const ServiceRxMetadata meta{rx_metadata, source_node_id};
        ServiceRxTransfer       svc_rx_transfer{meta, ScatteredBuffer{std::move(lizard_memory)}};
        if (on_receive_cb_fn_)
        {
            on_receive_cb_fn_(ISvcRxSession::OnReceiveCallback::Arg{svc_rx_transfer});
            return;
        }
        (void) last_rx_transfer_.emplace(std::move(svc_rx_transfer));
    }

private:
    // MARK: Data members:

    TransportDelegate&                         delegate_;
    const Params                               params_;
    cetl::optional<ServiceRxTransfer>          last_rx_transfer_;
    ISvcRxSession::OnReceiveCallback::Function on_receive_cb_fn_;

};  // SvcRxSessionBase

}  // namespace detail
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_SVC_RX_SESSION_BASE_HPP_INCLUDED
