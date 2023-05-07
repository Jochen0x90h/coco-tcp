#include "TcpServer_Win32.hpp"
#include <iostream>


namespace coco {

TcpServer_Win32::TcpServer_Win32(Loop_Win32 &loop)
	: loop(loop)
{
	// initialize winsock
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2,2), &wsaData);
}

TcpServer_Win32::~TcpServer_Win32() {
	closesocket(this->socket);
	WSACleanup();
}

bool TcpServer_Win32::listen(uint16_t port) {
	// create socket
	SOCKET socket = WSASocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (socket == INVALID_SOCKET) {
		int e = WSAGetLastError();
		return false;
	}

	// bind to local port
	sockaddr_in6 ep = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
	if (bind(socket, (struct sockaddr *)&ep, sizeof(ep)) != 0) {
		int e = WSAGetLastError();
		closesocket(socket);
		return false;
	}

	// add socket to completion port of event loop
	Loop_Win32::CompletionHandler *handler = this;
	if (CreateIoCompletionPort(
		(HANDLE)socket,
		this->loop.port,
		ULONG_PTR(handler),
		0) == nullptr)
	{
		int e = WSAGetLastError();
		closesocket(socket);
		return false;
	}

	// listen
	int result = ::listen(socket, 100);
	if (result == SOCKET_ERROR) {
		int e = WSAGetLastError();
		closesocket(socket);
		return false;
	}

	// get AcceptEx
	GUID guidAcceptEx = WSAID_ACCEPTEX;
	DWORD transferred;
	if (WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidAcceptEx, sizeof(guidAcceptEx),
		&this->AcceptEx, sizeof(this->AcceptEx),
		&transferred, NULL, NULL) != 0)
	{
		int e = WSAGetLastError();
		closesocket(socket);
		return false;
	}
	this->socket = socket;
	return true;
}

void TcpServer_Win32::close() {
	closesocket(this->socket);
	this->socket = INVALID_SOCKET;
}

void TcpServer_Win32::handle(OVERLAPPED *overlapped) {
	for (auto &socket : this->sockets) {
		if (overlapped == &socket.overlapped) {
			socket.handleAccept(overlapped);
			break;
		}
	}
}


// Socket

TcpServer_Win32::Socket::Socket(TcpServer_Win32 &server)
	: server(server)
{
	server.sockets.add(*this);
}

TcpServer_Win32::Socket::~Socket() {
	closesocket(this->socket);
}

Device::State TcpServer_Win32::Socket::state() {
	return this->stat;
}

Awaitable<Device::State> TcpServer_Win32::Socket::untilState(State state) {
	if (this->stat == state)
		return {};
	return {this->stateTasks, state};
}

int TcpServer_Win32::Socket::getBufferCount() {
	return this->buffers.count();
}

TcpServer_Win32::Buffer &TcpServer_Win32::Socket::getBuffer(int index) {
	return this->buffers.get(index);
}

bool TcpServer_Win32::Socket::accept() {
	auto &server = this->server;

	// create socket
	SOCKET socket = WSASocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (socket == INVALID_SOCKET) {
		int e = WSAGetLastError();
		return false;
	}

	// add socket to completion port of event loop
	Loop_Win32::CompletionHandler *handler = this;
	if (CreateIoCompletionPort(
		(HANDLE)socket,
		server.loop.port,
		ULONG_PTR(handler),
		0) == nullptr)
	{
		int e = WSAGetLastError();
		closesocket(socket);
		return false;
	}

	// accept
	memset(&this->overlapped, 0, sizeof(OVERLAPPED));
	if (server.AcceptEx(
		server.socket,
		socket,
		this->buffer, // buffer for addresses
		0, // receive size
		sizeof(sockaddr_in6) + 16, sizeof(sockaddr_in6) + 16,
		nullptr,
		&this->overlapped) == FALSE)
	{
		int error = WSAGetLastError();
		if (error != ERROR_IO_PENDING) {
			// "real" error
			closesocket(socket);
			return false;
		}
	}
	this->socket = socket;

	// set state
	this->stat = State::BUSY;

	// enable buffers
	for (auto &buffer : this->buffers) {
		buffer.setBusy();
	}

	// resume all coroutines waiting for busy state
	this->stateTasks.resumeAll([](State state) {
		return state == State::BUSY;
	});

	return true;
}

void TcpServer_Win32::Socket::close() {
	// close socket
	closesocket(this->socket);
	this->socket = INVALID_SOCKET;

	// set state
	this->stat = State::DISABLED;

	// set state of buffers to disabled
	for (auto &buffer : this->buffers) {
		buffer.setDisabled();
	}

	// resume all coroutines waiting for disabled state
	this->stateTasks.resumeAll([](State state) {
		return state == State::DISABLED;
	});
}

void TcpServer_Win32::Socket::handleAccept(OVERLAPPED *overlapped) {
	// result of AcceptEx
	DWORD transferred;
	DWORD flags;
	auto result = WSAGetOverlappedResult(this->socket, overlapped, &transferred, false, &flags);
	if (!result) {
		// "real" error or cancelled (ERROR_OPERATION_ABORTED): close
		auto error = WSAGetLastError();
		close();
	} else {
		setsockopt(this->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&this->server.socket, sizeof(SOCKET));

		// set state
		this->stat = State::READY;

		// set state of buffers to disabled
		for (auto &buffer : this->buffers) {
			buffer.setReady();
		}

		// resume all coroutines waiting for disabled state
		this->stateTasks.resumeAll([](State state) {
			return state == State::READY;
		});
	}
}

void TcpServer_Win32::Socket::handle(OVERLAPPED *overlapped) {
	for (auto &buffer : this->buffers) {
		if (overlapped == &buffer.overlapped) {
			buffer.handle(overlapped);
			break;
		}
	}
}


// Buffer

TcpServer_Win32::Buffer::Buffer(TcpServer_Win32::Socket &socket, int size)
	: BufferImpl(new uint8_t[size], size, socket.stat)
	, socket(socket)
{
	socket.buffers.add(*this);
}

TcpServer_Win32::Buffer::~Buffer() {
	delete [] this->dat;
}

bool TcpServer_Win32::Buffer::startInternal(int size, Op op) {
	if (this->stat != State::READY) {
		assert(false);
		return false;
	}

	// check if READ or WRITE flag is set
	assert((op & Op::READ_WRITE) != 0);

	this->op = op;

	// initialize overlapped
	memset(&this->overlapped, 0, sizeof(OVERLAPPED));

	WSABUF buffer;
	buffer.buf = (CHAR*)this->dat;
	buffer.len = size;
	int result;
	if ((op & Op::READ) != 0) {
		// receive
		DWORD flags = 0;
		result = WSARecv(this->socket.socket, &buffer, 1, nullptr, &flags, &this->overlapped, nullptr);
	} else {
		// send
		result = WSASend(this->socket.socket, &buffer, 1, nullptr, 0, &this->overlapped, nullptr);
	}
	if (result != 0) {
		int error = WSAGetLastError();
		if (error != WSA_IO_PENDING) {
			// "real" error
			setReady(0);
			return false;
		}
	}

	// set state
	setBusy();

	return true;
}

void TcpServer_Win32::Buffer::cancel() {
	if (this->stat != State::BUSY)
		return;

	auto result = CancelIoEx((HANDLE)this->socket.socket, &this->overlapped);
	if (!result) {
		auto e = WSAGetLastError();
		std::cerr << "cancel error " << e << std::endl;
	}
}

void TcpServer_Win32::Buffer::handle(OVERLAPPED *overlapped) {
	DWORD transferred;
	DWORD flags;
	auto result = WSAGetOverlappedResult(this->socket.socket, overlapped, &transferred, false, &flags);
	if (!result) {
		// "real" error or cancelled (ERROR_OPERATION_ABORTED): return zero size
		auto error = WSAGetLastError();
		transferred = 0;
	}

	// transfer finished
	setReady(transferred);
}

} // namespace coco
