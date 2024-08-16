/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_PRESENTATION_DELEGATE_HPP_INCLUDED
#define LIBCYPHAL_PRESENTATION_DELEGATE_HPP_INCLUDED

namespace libcyphal
{
namespace presentation
{

/// Internal implementation details of the Presentation layer.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

// Forward declaration.
class PublisherImpl;

// TODO: docs
class IPresentationDelegate
{
public:
    IPresentationDelegate(const IPresentationDelegate&)                = delete;
    IPresentationDelegate(IPresentationDelegate&&) noexcept            = delete;
    IPresentationDelegate& operator=(const IPresentationDelegate&)     = delete;
    IPresentationDelegate& operator=(IPresentationDelegate&&) noexcept = delete;

    virtual void releasePublisher(PublisherImpl* publisher_impl) noexcept = 0;

protected:
    IPresentationDelegate()  = default;
    ~IPresentationDelegate() = default;

};  // IPresentationDelegate

}  // namespace detail
}  // namespace presentation
}  // namespace libcyphal

#endif  // LIBCYPHAL_PRESENTATION_DELEGATE_HPP_INCLUDED