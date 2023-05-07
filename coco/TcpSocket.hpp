#pragma once

#include <coco/ipv6.hpp>
#include <coco/Device.hpp>


namespace coco {

/**
	TCP socket
*/
class TcpSocket : public Device {
public:
	/**
		Connect to a server. The socket immediately transitions into busy state and to ready state when connected
		@param endpoint endpoint (address and port) of server
		@return true if connect operation was started, false on error
	*/
	virtual bool connect(const ipv6::Endpoint &endpoint) = 0;

	/**
		Close the socket
	*/
	virtual void close() = 0;
};

} // namespace coco
