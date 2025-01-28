/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_UDP_RX_SESSION_TREE_NODE_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_UDP_RX_SESSION_TREE_NODE_HPP_INCLUDED

#include "tx_rx_sockets.hpp"

#include "libcyphal/executor.hpp"
#include "libcyphal/transport/msg_sessions.hpp"
#include "libcyphal/transport/session_tree.hpp"
#include "libcyphal/transport/svc_sessions.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <cetl/cetl.hpp>
#include <udpard.h>

#include <array>
#include <cstdint>
#include <tuple>

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

template <typename SocketInterface>
struct SocketState
{
    UniquePtr<SocketInterface> interface;
    IExecutor::Callback::Any   callback;

};  // SocketState

class IRxSessionDelegate;
class IMsgRxSessionDelegate;

/// Umbrella type for various RX session tree nodes in use at the UDP transport.
///
struct RxSessionTreeNode
{
    /// @brief Represents a message RX session node.
    ///
    class Message final : public transport::detail::SessionTree<Message>::NodeBase
    {
    public:
        // Empty tuple parameter is used to allow for the same constructor signature
        // as for other session nodes (see also `SessionTree::ensureNodeFor` method).
        //
        explicit Message(const MessageRxParams& params, const std::tuple<>&)
            : subject_id_{params.subject_id}
        {
        }

        CETL_NODISCARD std::int32_t compareByParams(const MessageRxParams& params) const
        {
            return static_cast<std::int32_t>(subject_id_) - static_cast<std::int32_t>(params.subject_id);
        }

        CETL_NODISCARD IMsgRxSessionDelegate*& delegate() noexcept
        {
            return delegate_;
        }

        CETL_NODISCARD SocketState<IRxSocket>& socketState(const std::uint8_t media_index) noexcept
        {
            CETL_DEBUG_ASSERT(media_index < socket_states_.size(), "");

            // No lint b/c at transport constructor we made sure that the number of media interfaces is bound.
            return socket_states_[media_index];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        }

    private:
        // MARK: Data members:

        const PortId                                                           subject_id_;
        IMsgRxSessionDelegate*                                                 delegate_{nullptr};
        std::array<SocketState<IRxSocket>, UDPARD_NETWORK_INTERFACE_COUNT_MAX> socket_states_;

    };  // Message

    /// @brief Represents a service request RX session node.
    ///
    class Request final : public transport::detail::SessionTree<Request>::NodeBase
    {
    public:
        // Empty tuple parameter is used to allow for the same constructor signature
        // as for other session nodes (see also `SessionTree::ensureNodeFor` method).
        //
        explicit Request(const RequestRxParams& params, const std::tuple<>&)
            : service_id_{params.service_id}
        {
        }

        CETL_NODISCARD std::int32_t compareByParams(const RequestRxParams& params) const
        {
            return static_cast<std::int32_t>(service_id_) - static_cast<std::int32_t>(params.service_id);
        }

    private:
        // MARK: Data members:

        const PortId service_id_;

    };  // Request

    /// @brief Represents a service response RX session node.
    ///
    using Response = transport::detail::ResponseRxSessionNode<IRxSessionDelegate>;

};  // RxSessionTreeNode

}  // namespace detail
}  // namespace udp
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_UDP_RX_SESSION_TREE_NODE_HPP_INCLUDED
