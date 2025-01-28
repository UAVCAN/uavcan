/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_CAN_RX_SESSION_TREE_NODE_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_CAN_RX_SESSION_TREE_NODE_HPP_INCLUDED

#include "libcyphal/transport/session_tree.hpp"

namespace libcyphal
{
namespace transport
{
namespace can
{

/// Internal implementation details of the UDP transport.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

class IRxSessionDelegate;

/// Umbrella type for various RX session tree nodes in use at the CAN transport.
///
/// Currently, it contains only one `Response` subtype,
/// but still kept nested to match the UDP transport approach (where there are several subtypes).
///
struct RxSessionTreeNode
{
    /// @brief Represents a service response RX session node.
    ///
    using Response = transport::detail::ResponseRxSessionNode<IRxSessionDelegate>;

};  // RxSessionTreeNode

}  // namespace detail
}  // namespace can
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_CAN_RX_SESSION_TREE_NODE_HPP_INCLUDED
