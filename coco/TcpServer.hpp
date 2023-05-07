#pragma once

#include <coco/ipv6.hpp>
#include <coco/BufferDevice.hpp>


namespace coco {

/**
 * TCP server socket
 */
class TcpServer {
public:
    virtual ~TcpServer() {}

    /**
     * Listen on a port
     * @param port local port to listen for incoming connections
     * @return true if server was started, false on error
     */
    virtual bool listen(uint16_t port) = 0;

    /**
     * Close the server
     */
    virtual void close() = 0;


    class Socket : public BufferDevice {
    public:
        Socket(State state) : BufferDevice(state) {}\

        /**
         * Accept an incoming connection
         * @return true if accept operation was started, false on error
         */
        virtual bool accept() = 0;
    };
};

} // namespace coco
