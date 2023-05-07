#pragma once

#include <coco/TcpServer.hpp>
#include <coco/BufferImpl.hpp>
#include <coco/LinkedList.hpp>
#define NOMINMAX
#include <winsock2.h> // see https://learn.microsoft.com/en-us/windows/win32/winsock/creating-a-basic-winsock-application
#include <ws2tcpip.h>
#include <mswsock.h>
#include <coco/platform/Loop_native.hpp> // includes Windows.h


namespace coco {

class TcpServer_Win32 : public TcpServer, public Loop_Win32::CompletionHandler {
public:
	/**
		Constructor
		@param loop event loop
	*/
	TcpServer_Win32(Loop_Win32 &loop);

	~TcpServer_Win32() override;

	bool listen(uint16_t port) override;
	void close() override;


	class Socket;

	/**
		Buffer for transferring data to/from a file
	*/
	class Buffer : public BufferImpl, public LinkedListNode {
		friend class Socket;
	public:
		Buffer(Socket &socket, int size);
		~Buffer() override;

		bool startInternal(int size, Op op) override;
		void cancel() override;

	protected:
		void handle(OVERLAPPED *overlapped);

		Socket &socket;
		OVERLAPPED overlapped;
		Op op;
	};


	class Socket : public TcpServer::Socket, public Loop_Win32::CompletionHandler , public LinkedListNode {
		friend class TcpServer_Win32;
		friend class Buffer;
	public:
		Socket(TcpServer_Win32 &server);
		~Socket() override;

		State state() override;
		Awaitable<State> untilState(State state) override;
		int getBufferCount() override;
		Buffer &getBuffer(int index) override;

		bool accept() override;
		void close() override;

	protected:
		void handleAccept(OVERLAPPED *overlapped);
		void handle(OVERLAPPED *overlapped) override;

		TcpServer_Win32 &server;

		// properties
		State stat = State::DISABLED;

		// socket handle
		SOCKET socket = INVALID_SOCKET;
		uint8_t buffer[(sizeof(sockaddr_in6) + 16) * 2];
		OVERLAPPED overlapped;

		TaskList<State> stateTasks;

		LinkedList<Buffer> buffers;
	};

protected:
	void handle(OVERLAPPED *overlapped) override;

	Loop_Win32 &loop;

	// server socket handle
	SOCKET socket = INVALID_SOCKET;

	LPFN_ACCEPTEX AcceptEx = NULL;

	LinkedList<Socket> sockets;
};

} // namespace coco
