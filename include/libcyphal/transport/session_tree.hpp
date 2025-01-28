/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_SESSION_TREE_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_SESSION_TREE_HPP_INCLUDED

#include "errors.hpp"
#include "libcyphal/common/cavl/cavl.hpp"
#include "libcyphal/types.hpp"
#include "svc_sessions.hpp"

#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>

#include <array>
#include <functional>
#include <tuple>

namespace libcyphal
{
namespace transport
{

/// Internal implementation details of a transport.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

/// @brief Defines a tree of sessions for a transport.
///
/// @tparam Node The type of the session node. Expected to be a subclass of `SessionTree::NodeBase`,
///              and to have a method `compareByParams` that compares nodes by its parameters.
///
template <typename Node>
class SessionTree final
{
public:
    /// Base class for the session tree node.
    ///
    class NodeBase : public common::cavl::Node<Node>
    {
    public:
        using common::cavl::Node<Node>::getChildNode;
        using RefWrapper = std::reference_wrapper<Node>;

        NodeBase(const NodeBase&)                = delete;
        NodeBase(NodeBase&&) noexcept            = delete;
        NodeBase& operator=(const NodeBase&)     = delete;
        NodeBase& operator=(NodeBase&&) noexcept = delete;

    protected:
        NodeBase()  = default;
        ~NodeBase() = default;

    };  // NodeBase

    explicit SessionTree(cetl::pmr::memory_resource& mr)
        : allocator_{&mr}
    {
    }

    SessionTree(const SessionTree&)                = delete;
    SessionTree(SessionTree&&) noexcept            = delete;
    SessionTree& operator=(const SessionTree&)     = delete;
    SessionTree& operator=(SessionTree&&) noexcept = delete;

    ~SessionTree()
    {
        nodes_.traversePostOrder([this](auto& node) { destroyNode(node); });
    }

    CETL_NODISCARD bool isEmpty() const noexcept
    {
        return nodes_.empty();
    }

    /// @brief Ensures that a node for the given parameters exists in the tree.
    ///
    /// @tparam ShouldBeNew If `true`, the function will return an error if node with given
    //                      parametes already exists (see also `Node::compareByParams` method).
    /// @tparam Params The type of the parameters to be used to find or create the node.
    /// @tparam Args The types of the arguments to be passed to the constructor of the node.
    /// @param params The parameters to be used to find or create the node.
    /// @param args The extra arguments to be forwarded to the constructor of the node (as a tuple).
    /// @return The reference to the node, or an error if the node could not be created.
    ///
    template <bool ShouldBeNew = false, typename Params, typename... Args>
    CETL_NODISCARD auto ensureNodeFor(const Params& params,
                                      Args&&... args) -> Expected<typename NodeBase::RefWrapper, AnyFailure>
    {
        // In c++14 we can't capture `args` with forwarding, so we pack them into a tuple.
        auto args_tuple = std::make_tuple(std::forward<Args>(args)...);

        const auto node_existing = nodes_.search(
            [&params](const Node& node) {  // predicate
                //
                return node.compareByParams(params);
            },
            [this, &params, args_tuple_ = std::move(args_tuple)] {
                //
                return constructNewNode(params, std::move(args_tuple_));
            });

        auto* const node = std::get<0>(node_existing);
        if (nullptr == node)
        {
            return MemoryError{};
        }
        if (ShouldBeNew && std::get<1>(node_existing))
        {
            return AlreadyExistsError{};
        }

        return *node;
    }

    template <typename Params>
    CETL_NODISCARD Node* tryFindNodeFor(const Params& params)
    {
        return nodes_.search([&params](const Node& node) {  // predicate
            //
            return node.compareByParams(params);
        });
    }

    template <typename Params>
    void removeNodeFor(const Params& params) noexcept
    {
        removeAndDestroyNode(tryFindNodeFor(params));
    }

    template <typename Action>
    CETL_NODISCARD cetl::optional<AnyFailure> forEachNode(Action&& action)
    {
        return nodes_.traverse(std::forward<Action>(action));
    }

private:
    template <typename Params, typename ArgsTuple>
    CETL_NODISCARD Node* constructNewNode(const Params& params, ArgsTuple&& args_tuple)
    {
        Node* const node = allocator_.allocate(1);
        if (nullptr != node)
        {
            allocator_.construct(node, params, std::forward<ArgsTuple>(args_tuple));
        }
        return node;
    }

    void removeAndDestroyNode(Node* node)
    {
        if (nullptr != node)
        {
            nodes_.remove(node);
            destroyNode(*node);
        }
    }

    void destroyNode(Node& node)
    {
        // No Sonar cpp:M23_329 b/c we do our own low-level PMR management here.
        node.~Node();  // NOSONAR cpp:M23_329
        allocator_.deallocate(&node, 1);
    }

    // MARK: Data members:

    common::cavl::Tree<Node>              nodes_;
    libcyphal::detail::PmrAllocator<Node> allocator_;

};  // SessionTree

// MARK: -

/// @brief Represents a service response RX session node.
///
template <typename RxSessionDelegate>
class ResponseRxSessionNode final : public SessionTree<ResponseRxSessionNode<RxSessionDelegate>>::NodeBase
{
public:
    // Empty tuple parameter is used to allow for the same constructor signature
    // as for other session nodes (see also `SessionTree::ensureNodeFor` method).
    //
    explicit ResponseRxSessionNode(const ResponseRxParams& params, const std::tuple<>&)
        : service_id_{params.service_id}
        , server_node_id{params.server_node_id}
        , delegate_{nullptr}
    {
    }

    CETL_NODISCARD std::int32_t compareByParams(const ResponseRxParams& params) const
    {
        if (service_id_ != params.service_id)
        {
            return static_cast<std::int32_t>(service_id_) - static_cast<std::int32_t>(params.service_id);
        }
        return static_cast<std::int32_t>(server_node_id) - static_cast<std::int32_t>(params.server_node_id);
    }

    CETL_NODISCARD RxSessionDelegate*& delegate() noexcept
    {
        return delegate_;
    }

private:
    // MARK: Data members:

    const PortId       service_id_;
    const NodeId       server_node_id;
    RxSessionDelegate* delegate_;

};  // ResponseRxSessionNode

}  // namespace detail
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_SESSION_TREE_HPP_INCLUDED
