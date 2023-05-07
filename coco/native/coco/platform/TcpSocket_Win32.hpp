#pragma once

#include <coco/TcpSocket.hpp>
#include <coco/BufferImpl.hpp>
#include <coco/LinkedList.hpp>
#define NOMINMAX
#include <winsock2.h> // see https://learn.microsoft.com/en-us/windows/win32/winsock/creating-a-basic-winsock-application
#include <ws2tcpip.h>
#include <mswsock.h>
#include <coco/platform/Loop_native.hpp> // includes Windows.h


namespace coco {

class TcpSocket_Win32 : public TcpSocket, public Loop_Win32::CompletionHandler {
public:
	/**
		Constructor
		@param loop event loop
	*/
	TcpSocket_Win32(Loop_Win32 &loop);

	~TcpSocket_Win32() override;

	class Buffer;

	State state() override;
	Awaitable<State> untilState(State state) override;
	int getBufferCount() override;
	Buffer &getBuffer(int index) override;

	bool connect(const ipv6::Endpoint &endpoint) override;
	void close() override;

	/**
		Buffer for transferring data to/from a file
	*/
	class Buffer : public BufferImpl, public LinkedListNode {
		friend class TcpSocket_Win32;
	public:
		Buffer(TcpSocket_Win32 &socket, int size);
		~Buffer() override;

		bool startInternal(int size, Op op) override;
		void cancel() override;

	protected:
		void handle(OVERLAPPED *overlapped);

		TcpSocket_Win32 &socket;
		OVERLAPPED overlapped;
		Op op;
	};

protected:
	void handle(OVERLAPPED *overlapped) override;

	Loop_Win32 &loop;

	// socket handle
	SOCKET socket = INVALID_SOCKET;

	// properties
	State stat = State::DISABLED;

	OVERLAPPED overlapped;

	TaskList<State> stateTasks;

	LinkedList<Buffer> buffers;
};

} // namespace coco
