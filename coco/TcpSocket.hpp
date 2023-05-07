#pragma once

#include <coco/ipv6.hpp>
#include <coco/BufferDevice.hpp>


namespace coco {

/**
 * TCP client socket
 */
class TcpSocket : public BufferDevice {
public:
    TcpSocket(State state) : BufferDevice(state) {}

    /**
     * Connect to a server. The socket immediately transitions into busy state and to ready state when connected
     * @param endpoint endpoint (address and port) of server
     * @return true if connect operation was started, false on error
     */
    virtual bool connect(const ipv6::Endpoint &endpoint) = 0;
};

} // namespace coco
