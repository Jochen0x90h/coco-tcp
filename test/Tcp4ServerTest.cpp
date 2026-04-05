#include "TcpServerTest.hpp"
#include <coco/debug.hpp>
#include <coco/StreamOperators.hpp>
#ifdef NATIVE
#include <string>
#include <iostream>
#endif


/*
    Tcp4SocketTest: Starts a server and creates a client that connects to it.
    The server replies "Hello World" to the client.
*/


Coroutine server(Loop &loop, TcpServer::Socket &serverSocket, Buffer &buffer) {
    // wait until socket is ready
    co_await serverSocket.untilReadyOrDisabled();

    // receive from client
    co_await buffer.read();

    // check for errors
    if (buffer.error()) {
        debug::out << "Server: Read error " << buffer.error().message() << '\n';
        loop.exit();
        co_return;
    }

    // get remote endpoint
    auto &ep = serverSocket.getEndpoint(true);

    // output received size
    debug::out << "Server: Received " << dec(buffer.size()) << " bytes\n";

    // reply to client
    co_await buffer.write("Hello IPv4 World");

    //loop.exit();
}


Coroutine client(Loop &loop, IpSocket &socket, Buffer &buffer) {
    // wait until socket is ready
    co_await socket.untilReadyOrDisabled();

    // send to server
    co_await buffer.write("GET / HTTP/1.1\r\nHost: wikipedia.de\r\nUser-Agent: curl/7.87.0\r\nAccept: */*\r\n\r\n");

    // check for errors
    if (buffer.error()) {
        debug::out << "Client: Write error " << buffer.error().message() << '\n';
        loop.exit();
        co_return;
    }

    // wait for reply from server (is "Hello World" when connected to localhost or a HTTP page when connected to wikipedia.de)
    co_await buffer.read();

    // check for errors
    if (buffer.error()) {
        debug::out << "Client: Read error " << buffer.error().message() << '\n';
        loop.exit();
        co_return;
    }

    int transferred = buffer.size();

    debug::out << "Client: Received \"" << buffer.string() << "\"\n";

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
    debug::out << "Tcp4ServerTest\n";

    // start server
    drivers.server.listen(ip::v4::PROTOCOL_ID, listenPort);
    drivers.serverSocket.accept();
    server(drivers.loop, drivers.serverSocket, drivers.serverBuffer);

    // connect client to server
    ip::v4::Endpoint destination = {.port = connectPort, .address = *ip::v4::Address::fromString("127.0.0.1")}; // localhost
    //ip::Endpoint destination = {.v4 = {.port = connectPort, .address = *ip::v4::Address::fromString("127.0.0.1")}}; // localhost
    if (!drivers.clientSocket.connect(destination)) {
        debug::out << "Connect failed!\n";
        return 1;
    }

    // start client
    client(drivers.loop, drivers.clientSocket, drivers.clientBuffer);

    drivers.loop.run();
}
