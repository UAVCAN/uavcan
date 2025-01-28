/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#include "memory_resource_mock.hpp"
#include "tracking_memory_resource.hpp"

#include <cetl/pf17/cetlpf.hpp>
#include <libcyphal/common/cavl/cavl.hpp>
#include <libcyphal/errors.hpp>
#include <libcyphal/transport/errors.hpp>
#include <libcyphal/transport/session_tree.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <utility>

namespace
{

using libcyphal::MemoryError;
using namespace libcyphal::transport;  // NOLINT This our main concern here in the unit tests.

using testing::_;
using testing::StrEq;
using testing::IsNull;
using testing::Return;
using testing::IsEmpty;
using testing::NotNull;
using testing::Pointer;
using testing::StrictMock;
using testing::VariantWith;

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

class TestSessionTree : public testing::Test
{
protected:
    class MyNode final : public libcyphal::common::cavl::Node<MyNode>
    {
    public:
        using Params           = std::int32_t;
        using ReferenceWrapper = std::reference_wrapper<MyNode>;

        explicit MyNode(const Params& params, const std::tuple<const char*>& args_tuple)
            : params_{params}
            , extra_arg_{std::get<0>(args_tuple)}
        {
        }

        MyNode(const MyNode&)                = delete;
        MyNode(MyNode&&) noexcept            = delete;
        MyNode& operator=(const MyNode&)     = delete;
        MyNode& operator=(MyNode&&) noexcept = delete;

        ~MyNode()
        {
            if (notifier_)
            {
                notifier_("~");
            }
        }

        CETL_NODISCARD std::int32_t compareByParams(const Params& params) const
        {
            return params_ - params;
        }

        const char* getExtraArg() const noexcept
        {
            return extra_arg_;
        }

        void setNotifier(std::function<void(const std::string&)> notifier)
        {
            notifier_ = std::move(notifier);
        }

    private:
        const Params                            params_;
        const char* const                       extra_arg_;
        std::function<void(const std::string&)> notifier_;

    };  // MyNode

    void SetUp() override
    {
        cetl::pmr::set_default_resource(&mr_);
    }

    void TearDown() override
    {
        EXPECT_THAT(mr_.allocations, IsEmpty());
        EXPECT_THAT(mr_.total_allocated_bytes, mr_.total_deallocated_bytes);
    }

    // MARK: Data members:

    // NOLINTBEGIN
    TrackingMemoryResource mr_;
    // NOLINTEND

};  // TestSessionTree

// MARK: - Tests:

TEST_F(TestSessionTree, constructor_destructor_empty_tree)
{
    const detail::SessionTree<MyNode> tree{mr_};
    EXPECT_THAT(tree.isEmpty(), true);
}

TEST_F(TestSessionTree, ensureNodeFor_should_be_new)
{
    detail::SessionTree<MyNode> tree{mr_};

    EXPECT_THAT(tree.ensureNodeFor<true>(0, "0a"), VariantWith<MyNode::ReferenceWrapper>(_));
    EXPECT_THAT(tree.isEmpty(), false);

    EXPECT_THAT(tree.ensureNodeFor<true>(1, "1a"), VariantWith<MyNode::ReferenceWrapper>(_));
    EXPECT_THAT(tree.ensureNodeFor<true>(2, "2a"), VariantWith<MyNode::ReferenceWrapper>(_));
    EXPECT_THAT(tree.ensureNodeFor<true>(0, "0b"), VariantWith<AnyFailure>(VariantWith<AlreadyExistsError>(_)));
    EXPECT_THAT(tree.ensureNodeFor<true>(1, "1b"), VariantWith<AnyFailure>(VariantWith<AlreadyExistsError>(_)));
    EXPECT_THAT(tree.ensureNodeFor<true>(2, "2b"), VariantWith<AnyFailure>(VariantWith<AlreadyExistsError>(_)));

    EXPECT_THAT(tree.tryFindNodeFor(0)->getExtraArg(), StrEq("0a"));
    EXPECT_THAT(tree.tryFindNodeFor(1)->getExtraArg(), StrEq("1a"));
    EXPECT_THAT(tree.tryFindNodeFor(2)->getExtraArg(), StrEq("2a"));
    EXPECT_THAT(tree.tryFindNodeFor(3), IsNull());
}

TEST_F(TestSessionTree, ensureNodeFor_existing_is_fine)
{
    detail::SessionTree<MyNode> tree{mr_};

    auto maybe_node_0a = tree.ensureNodeFor(0, "0a");
    ASSERT_THAT(maybe_node_0a, VariantWith<MyNode::ReferenceWrapper>(_));
    const auto node_0a = cetl::get<MyNode::ReferenceWrapper>(maybe_node_0a);

    EXPECT_THAT(tree.isEmpty(), false);

    auto maybe_node_1a = tree.ensureNodeFor(1, "1a");
    EXPECT_THAT(maybe_node_1a, VariantWith<MyNode::ReferenceWrapper>(_));
    const auto node_1a = cetl::get<MyNode::ReferenceWrapper>(maybe_node_1a);

    EXPECT_THAT(tree.ensureNodeFor(2, "2a"), VariantWith<MyNode::ReferenceWrapper>(_));

    auto maybe_node_0b = tree.ensureNodeFor(0, "0b");
    EXPECT_THAT(maybe_node_0b, VariantWith<MyNode::ReferenceWrapper>(Pointer(&node_0a.get())));
    EXPECT_THAT(tree.tryFindNodeFor(0)->getExtraArg(), StrEq("0a"));

    auto maybe_node_1b = tree.ensureNodeFor(1, "1b");
    EXPECT_THAT(maybe_node_1b, VariantWith<MyNode::ReferenceWrapper>(Pointer(&node_1a.get())));
    EXPECT_THAT(tree.tryFindNodeFor(1)->getExtraArg(), StrEq("1a"));

    EXPECT_THAT(tree.ensureNodeFor(2, "2b"), VariantWith<MyNode::ReferenceWrapper>(_));
    EXPECT_THAT(tree.tryFindNodeFor(2)->getExtraArg(), StrEq("2a"));
}

TEST_F(TestSessionTree, ensureNodeFor_no_memory)
{
    StrictMock<MemoryResourceMock> mr_mock;
    mr_mock.redirectExpectedCallsTo(mr_);

    detail::SessionTree<MyNode> tree{mr_mock};

    // Emulate that there is no memory available for the message session.
    EXPECT_CALL(mr_mock, do_allocate(sizeof(MyNode), _))  //
        .WillOnce(Return(nullptr));

    EXPECT_THAT(tree.ensureNodeFor(0, "0a"), VariantWith<AnyFailure>(VariantWith<MemoryError>(_)));
    EXPECT_THAT(tree.tryFindNodeFor(0), IsNull());
}

TEST_F(TestSessionTree, removeNodeFor)
{
    detail::SessionTree<MyNode> tree{mr_};

    tree.removeNodeFor(13);

    auto maybe_node = tree.ensureNodeFor<true>(42, "42a");
    ASSERT_THAT(maybe_node, VariantWith<MyNode::ReferenceWrapper>(_));
    EXPECT_THAT(tree.tryFindNodeFor(42), NotNull());
    EXPECT_THAT(tree.isEmpty(), false);

    const auto node_ref = cetl::get<MyNode::ReferenceWrapper>(maybe_node);

    std::string side_effects;
    node_ref.get().setNotifier([&](const std::string& msg) { side_effects += msg; });

    tree.removeNodeFor(42);
    EXPECT_THAT(side_effects, "~");

    EXPECT_THAT(tree.isEmpty(), true);
    EXPECT_THAT(tree.tryFindNodeFor(42), IsNull());
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

}  // namespace
