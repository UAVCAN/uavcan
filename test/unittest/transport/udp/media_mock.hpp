/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_UDP_MEDIA_MOCK_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_UDP_MEDIA_MOCK_HPP_INCLUDED

#include <cetl/pf17/cetlpf.hpp>
#include <libcyphal/transport/errors.hpp>
#include <libcyphal/transport/udp/media.hpp>
#include <libcyphal/transport/udp/tx_rx_sockets.hpp>

#include <gmock/gmock.h>

#include <cstddef>

namespace libcyphal
{
namespace transport
{
namespace udp
{

class MediaMock : public IMedia
{
public:
    MediaMock()          = default;
    virtual ~MediaMock() = default;

    MediaMock(const MediaMock&)                = delete;
    MediaMock(MediaMock&&) noexcept            = delete;
    MediaMock& operator=(const MediaMock&)     = delete;
    MediaMock& operator=(MediaMock&&) noexcept = delete;

    MOCK_METHOD(MakeTxSocketResult::Type, makeTxSocket, (), (override));
    MOCK_METHOD(MakeRxSocketResult::Type, makeRxSocket, (const IpEndpoint& multicast_endpoint), (override));
    MOCK_METHOD(cetl::pmr::memory_resource&, getTxMemoryResource, (), (override));

};  // MediaMock

}  // namespace udp
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_UDP_MEDIA_MOCK_HPP_INCLUDED
