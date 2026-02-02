#pragma once

#include <coco/ip.hpp>
#include <coco/BufferDevice.hpp>


namespace coco {

/// @brief TCP server socket.
/// Used to listen for client connections on a port
class TcpServer {
public:
    virtual ~TcpServer() {}

    /// @brief Listen on a port
    /// @param protocolId Protocol id such as ip::v4::PROTOCOL_ID or ip::v6::PROTOCOL_ID
    /// @param port Local port to listen for incoming connections
    /// @return true if server was started, false on error
    virtual bool listen(uint16_t protocolId, uint16_t port) = 0;

    /// @brief Close the server
    ///
    virtual void close() = 0;


    /// @brief Server socket
    /// Is associated with a server and can accept() a single connection.
    class Socket : public BufferDevice {
    public:
        Socket(State state) : BufferDevice(state) {}\

        /// @brief Accept an incoming connection.
        /// Can be called only if the device is in DISABLED state.
        /// Use close() to end a connection and accept() a new one after device has returned to DISABLED state.
        /// @return true if accept operation was started, false on error
        virtual bool accept() = 0;

        /// @brief get local or remote endpoint after accept() succeeded (device is in READY state)
        /// @return Local or remote endpoint
        virtual ip::Endpoint &getEndpoint(bool remote) = 0;
    };
};

} // namespace coco
