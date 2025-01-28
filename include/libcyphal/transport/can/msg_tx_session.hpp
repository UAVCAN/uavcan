/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_CAN_MSG_TX_SESSION_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_CAN_MSG_TX_SESSION_HPP_INCLUDED

#include "delegate.hpp"

#include "libcyphal/transport/errors.hpp"
#include "libcyphal/transport/msg_sessions.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <canard.h>
#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>

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

class MessageTxSession final : public IMessageTxSession
{
    /// @brief Defines private specification for making interface unique ptr.
    ///
    struct Spec : libcyphal::detail::UniquePtrSpec<IMessageTxSession, MessageTxSession>
    {
        // `explicit` here is in use to disable public construction of derived private `Spec` structs.
        // See https://seanmiddleditch.github.io/enabling-make-unique-with-private-constructors/
        explicit Spec() = default;
    };

public:
    CETL_NODISCARD static Expected<UniquePtr<IMessageTxSession>, AnyFailure> make(  //
        cetl::pmr::memory_resource& memory,
        TransportDelegate&          delegate,
        const MessageTxParams&      params)
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

    MessageTxSession(const Spec, TransportDelegate& delegate, const MessageTxParams& params)
        : delegate_{delegate}
        , params_{params}
    {
    }

private:
    // MARK: IMessageTxSession

    CETL_NODISCARD MessageTxParams getParams() const noexcept override
    {
        return params_;
    }

    CETL_NODISCARD cetl::optional<AnyFailure> send(const TransferTxMetadata& metadata,
                                                   const PayloadFragments    payload_fragments) override
    {
        const auto canard_metadata = CanardTransferMetadata{static_cast<CanardPriority>(metadata.base.priority),
                                                            CanardTransferKindMessage,
                                                            params_.subject_id,
                                                            CANARD_NODE_ID_UNSET,
                                                            static_cast<CanardTransferID>(metadata.base.transfer_id)};

        return delegate_.sendTransfer(metadata.deadline, canard_metadata, payload_fragments);
    }

    // MARK: Data members:

    TransportDelegate&    delegate_;
    const MessageTxParams params_;

};  // MessageTxSession

}  // namespace detail
}  // namespace can
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_CAN_MSG_TX_SESSION_HPP_INCLUDED
