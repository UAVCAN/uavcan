/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_UDP_SVC_TX_SESSIONS_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_UDP_SVC_TX_SESSIONS_HPP_INCLUDED

#include "delegate.hpp"

#include "libcyphal/transport/errors.hpp"
#include "libcyphal/transport/svc_sessions.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>
#include <udpard.h>

#include <chrono>

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

/// @brief A class to represent a service request TX session (aka client side).
///
class SvcRequestTxSession final : public IRequestTxSession
{
    /// @brief Defines private specification for making interface unique ptr.
    ///
    struct Spec : libcyphal::detail::UniquePtrSpec<IRequestTxSession, SvcRequestTxSession>
    {
        // `explicit` here is in use to disable public construction of derived private `Spec` structs.
        // See https://seanmiddleditch.github.io/enabling-make-unique-with-private-constructors/
        explicit Spec() = default;
    };

public:
    CETL_NODISCARD static Expected<UniquePtr<IRequestTxSession>, AnyFailure> make(  //
        cetl::pmr::memory_resource& memory,
        TransportDelegate&          delegate,
        const RequestTxParams&      params)
    {
        if ((params.service_id > UDPARD_SERVICE_ID_MAX) || (params.server_node_id > UDPARD_NODE_ID_MAX))
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

    SvcRequestTxSession(const Spec, TransportDelegate& delegate, const RequestTxParams& params)
        : delegate_{delegate}
        , params_{params}
    {
    }

private:
    // MARK: IRequestTxSession

    CETL_NODISCARD RequestTxParams getParams() const noexcept override
    {
        return params_;
    }

    CETL_NODISCARD cetl::optional<AnyFailure> send(const TransferTxMetadata& metadata,
                                                   const PayloadFragments    payload_fragments) override
    {
        // Before delegating to transport it makes sense to do some sanity checks.
        // Otherwise, transport may do some work (like possible payload allocation/copying,
        // media enumeration and pushing into their TX queues) doomed to fail with argument error.
        //
        if (delegate_.getNodeId() > UDPARD_NODE_ID_MAX)
        {
            return ArgumentError{};
        }

        const auto deadline_us =
            std::chrono::duration_cast<std::chrono::microseconds>(metadata.deadline.time_since_epoch());

        const auto tx_metadata = AnyUdpardTxMetadata::Request{static_cast<UdpardMicrosecond>(deadline_us.count()),
                                                              static_cast<UdpardPriority>(metadata.base.priority),
                                                              params_.service_id,
                                                              params_.server_node_id,
                                                              metadata.base.transfer_id};

        return delegate_.sendAnyTransfer(tx_metadata, payload_fragments);
    }

    // MARK: Data members:

    TransportDelegate&    delegate_;
    const RequestTxParams params_;

};  // SvcRequestTxSession

// MARK: -

/// @brief A class to represent a service response TX session (aka server side).
///
class SvcResponseTxSession final : public IResponseTxSession
{
    /// @brief Defines private specification for making interface unique ptr.
    ///
    struct Spec : libcyphal::detail::UniquePtrSpec<IResponseTxSession, SvcResponseTxSession>
    {
        // `explicit` here is in use to disable public construction of derived private `Spec` structs.
        // See https://seanmiddleditch.github.io/enabling-make-unique-with-private-constructors/
        explicit Spec() = default;
    };

public:
    CETL_NODISCARD static Expected<UniquePtr<IResponseTxSession>, AnyFailure> make(  //
        cetl::pmr::memory_resource& memory,
        TransportDelegate&          delegate,
        const ResponseTxParams&     params)
    {
        if (params.service_id > UDPARD_SERVICE_ID_MAX)
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

    SvcResponseTxSession(const Spec, TransportDelegate& delegate, const ResponseTxParams& params)
        : delegate_{delegate}
        , params_{params}
    {
    }

private:
    // MARK: IResponseTxSession

    CETL_NODISCARD ResponseTxParams getParams() const noexcept override
    {
        return params_;
    }

    CETL_NODISCARD cetl::optional<AnyFailure> send(const ServiceTxMetadata& metadata,
                                                   const PayloadFragments   payload_fragments) override
    {
        // Before delegating to transport it makes sense to do some sanity checks.
        // Otherwise, transport may do some work (like possible payload allocation/copying,
        // media enumeration and pushing into their TX queues) doomed to fail with argument error.
        //
        if ((delegate_.getNodeId() > UDPARD_NODE_ID_MAX) || (metadata.remote_node_id > UDPARD_NODE_ID_MAX))
        {
            return ArgumentError{};
        }

        const auto deadline_us =
            std::chrono::duration_cast<std::chrono::microseconds>(metadata.tx_meta.deadline.time_since_epoch());

        const auto tx_metadata =
            AnyUdpardTxMetadata::Respond{static_cast<UdpardMicrosecond>(deadline_us.count()),
                                         static_cast<UdpardPriority>(metadata.tx_meta.base.priority),
                                         params_.service_id,
                                         metadata.remote_node_id,
                                         metadata.tx_meta.base.transfer_id};

        return delegate_.sendAnyTransfer(tx_metadata, payload_fragments);
    }

    // MARK: Data members:

    TransportDelegate&     delegate_;
    const ResponseTxParams params_;

};  // SvcResponseTxSession

}  // namespace detail
}  // namespace udp
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_UDP_SVC_TX_SESSIONS_HPP_INCLUDED
