#include "TcpSocketTest.hpp"
#include <coco/debug.hpp>
#include <coco/StreamOperators.hpp>
#ifdef NATIVE
#include <string>
#include <iostream>
#endif


/*
    TcpSocketTest: Starts a server and creates a client that connects to it.
    The server repleis "Hello World" to the client.
*/


Coroutine server(Loop &loop, TcpServer::Socket &serverSocket, Buffer &buffer) {
    // wait until socket is ready, not needed as read/write wait until socket becomes ready
    //co_await serverSocket.untilReadyOrDisabled();

    // receive from client
    co_await buffer.read();

    debug::out << "Server: " << dec(buffer.size()) << "\n";

    // reply to client
    co_await buffer.write("Hello World");

    //loop.exit();
}


Coroutine client(Loop &loop, TcpSocket &socket, Buffer &buffer) {
    // wait until socket is ready, not needed as read/write wait until socket becomes ready
    //co_await socket.untilReadyOrDisabled();

    // send to server
    co_await buffer.write("GET / HTTP/1.1\r\nHost: wikipedia.de\r\nUser-Agent: curl/7.87.0\r\nAccept: */*\r\n\r\n");

    // wait for reply from server
    co_await buffer.read();
    int transferred = buffer.size();

    debug::out << "Client: " << buffer.string() << "\n";

    loop.exit();
}


// it is possible to start two instances with different ports
uint16_t listenPort = 1337;
uint16_t connectPort = 1337;

#ifdef NATIVE
int main(int argc, char const **argv) {
    if (argc >= 3) {
        listenPort = std::stoi(argv[1]);
        connectPort = std::stoi(argv[2]);
    }
#else
int main() {
#endif
    // start server
    drivers.server.listen(listenPort);
    drivers.serverSocket.accept();
    server(drivers.loop, drivers.serverSocket, drivers.serverBuffer);

    // connect client to server
    ipv6::Endpoint destination = {ipv6::Address::fromString("::1"), connectPort}; // localhost
    //ipv6::Endpoint destination = {ipv6::Address::fromString("2620:0:863:ed1a::1"), 80}; // wikipedia.de
    drivers.clientSocket.connect(destination);

    // start client
    client(drivers.loop, drivers.clientSocket, drivers.clientBuffer);

    drivers.loop.run();
}
