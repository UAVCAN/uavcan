/// @copyright Copyright Amazon.com Inc. and its affiliates. All Rights Reserved.
/// @file
/// Output session handler for publisher message and service sessions

#ifndef POSIX_LIBCYPHAL_TRANSPORT_UDP_OUTPUT_SESSION_HPP_INCLUDED
#define POSIX_LIBCYPHAL_TRANSPORT_UDP_OUTPUT_SESSION_HPP_INCLUDED

#include <cassert>
#include <cstdint>
#include <libcyphal/media/udp/frame.hpp>
#include <libcyphal/transport/id_types.hpp>
#include <libcyphal/transport/ip/v4/types.hpp>
#include "libcyphal/transport/udp/session/output_session.hpp"
#include <libcyphal/types/status.hpp>
#include "posix/libcyphal/transport/ip/v4/connection.hpp"
#include "posix/libcyphal/transport/ip/v4/properties.hpp"

namespace libcyphal
{
namespace transport
{
namespace udp
{
namespace session
{

/// @brief Used to store session information for UDP broadcasts
/// @todo Make this usable for service requests also
class PosixOutputSession final : public OutputSession
{
public:
    PosixOutputSession() = delete;

    /// @brief Constructor
    /// @param[in] node_id Node id of local host
    /// @param[in] local_address Local ip address
    PosixOutputSession(const NodeID node_id, const transport::ip::v4::Address local_address) noexcept
        : node_id_{node_id}
        , local_address_{local_address}
        , multicast_set_{false}
    {}

    /// @brief Destructor that cleans up posix socket connections
    ~PosixOutputSession()
    {
        if (socket_fd_ != ip::v4::ClosedSocket)
        {
            int result = close(socket_fd_);
            assert(result != ip::v4::SocketFunctionError);
            socket_fd_ = ip::v4::ClosedSocket;
        }
    }

    /// @brief Copy Constructor
    /// @param[in] other PosixOutputSession to copy from
    PosixOutputSession(const PosixOutputSession& other) noexcept
        : node_id_{other.node_id_}
        , local_address_{other.local_address_}
        , multicast_set_{other.multicast_set_}
        , socket_fd_{other.socket_fd_}
    {}

    /// @brief Move Constructor
    /// @param[in] other PosixOutputSession to move from
    PosixOutputSession(PosixOutputSession&& other) noexcept
        : node_id_{other.node_id_}
        , local_address_{other.local_address_}
        , multicast_set_{other.multicast_set_}
        , socket_fd_{other.socket_fd_}
    {
        other.multicast_set_ = false;
        other.socket_fd_     = ip::v4::ClosedSocket;
    }

    /// @brief Copy Assignment
    /// @param[in] other PosixOutputSession to copy from
    PosixOutputSession& operator=(const PosixOutputSession& other) noexcept
    {
        if (this != &other)
        {
            node_id_       = other.node_id_;
            local_address_ = other.local_address_;
            multicast_set_ = other.multicast_set_;
            socket_fd_     = other.socket_fd_;
        }
        return *this;
    }

    /// @brief Move Operator
    /// @param[in] other PosixOutputSession to move from
    PosixOutputSession& operator=(PosixOutputSession&& other) noexcept
    {
        if (this != &other)
        {
            node_id_             = other.node_id_;
            local_address_       = other.local_address_;
            multicast_set_       = other.multicast_set_;
            socket_fd_           = other.socket_fd_;
            other.multicast_set_ = false;
            other.socket_fd_     = ip::v4::ClosedSocket;
        }
        return *this;
    }

    /// @brief Initializes the Session handler
    Status initialize() override
    {
        socket_fd_ = ip::v4::createSocket();
        return (socket_fd_ != ip::v4::ClosedSocket) ? ResultCode::Success : ResultCode::Failure;
    }

    /// @brief Sets up everything needed to broadcast messages for a given subject id
    /// @note Current just uses one port and address to broadcast all messages
    /// @todo Investigate if multiple ports / address are desired per message / subject id
    /// @param[in] subject_id The subject id to broadcast on
    /// @param[in] frame The UDP Frame payload
    Status broadcast(PortID subject_id, const media::udp::Frame& frame) override
    {
        if (!multicast_set_)
        {
            multicast_set_ = ip::v4::setMulticast(socket_fd_, local_address_.asInteger()).isSuccess();
        }
        return ip::v4::sendBroadcast(socket_fd_,
                                     subject_id,
                                     frame.data_,
                                     frame.data_length_);
    }

    /// @brief Sets up everything needed to send a request or response transfer
    /// @note Currently just uses one port and address to sends all service transfers on
    /// @param[in] remote_node_id The Node who will receive the service transfer
    /// @param[in] frame The UDP Frame payload
    Status sendServiceTransfer(NodeID remote_node_id, const media::udp::Frame& frame) override
    {
        if (!multicast_set_)
        {
            multicast_set_ = ip::v4::setMulticast(socket_fd_, local_address_.asInteger()).isSuccess();
        }

        return ip::v4::sendServiceTransfer(
            socket_fd_,
            remote_node_id,
            frame.data_,
            frame.data_length_
        );
    }

private:
    NodeID          node_id_{UDPARD_NODE_ID_UNSET};
    ip::v4::Address local_address_{};
    bool            multicast_set_{false};
    ip::v4::Socket  socket_fd_{-1};
};

}  // namespace session
}  // namespace udp
}  // namespace transport
}  // namespace libcyphal

#endif  // POSIX_LIBCYPHAL_TRANSPORT_UDP_OUTPUT_SESSION_HPP_INCLUDED
