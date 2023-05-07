#include <coco/debug.hpp>
#include "TcpSocketTest.hpp"
#ifdef NATIVE
#include <string>
#include <iostream>
#endif


/*
	TcpSocketTest: Starts a server and connects to it
*/


std::ostream &operator <<(std::ostream &s, const String &str) {
	return s << std::string(str.data(), str.size());
}


Coroutine server(Loop &loop, Buffer &buffer) {
	// wait until buffer is ready
	int r = co_await select(buffer.untilReady(), buffer.untilDisabled());
	if (r == 2) {
		// failed to connect to server
		co_await loop.yield();
		loop.exit();
		co_await loop.yield();
		co_return;
	}

	co_await buffer.read();

#ifdef NATIVE
	std::cout << "Server: " << buffer.transferredString() << std::endl;
#endif

	co_await buffer.writeString("hello world");

	loop.exit();
}


Coroutine client(Loop &loop, Buffer &buffer) {
	// wait until buffer is ready
	int r = co_await select(buffer.untilReady(), buffer.untilDisabled());
	if (r == 2) {
		// failed to connect to server
		co_return;
	}


	co_await buffer.writeString("GET / HTTP/1.1\r\nHost: wikipedia.de\r\nUser-Agent: curl/7.87.0\r\nAccept: */*\r\n\r\n");

	co_await buffer.read();
	int transferred = buffer.transferred();

#ifdef NATIVE
	std::cout << "Client: " << buffer.transferredString() << std::endl;
#endif
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
	debug::init();
	Drivers drivers;

	drivers.server.listen(listenPort);
	drivers.serverSocket.accept();
	server(drivers.loop, drivers.serverBuffer);

	ipv6::Endpoint destination = {ipv6::Address::fromString("::1"), connectPort}; // localhost
	//ipv6::Endpoint destination = {ipv6::Address::fromString("2620:0:863:ed1a::1"), 80}; // wikipedia.de
	drivers.clientSocket.connect(destination);

	client(drivers.loop, drivers.clientBuffer);

	drivers.loop.run();
}
