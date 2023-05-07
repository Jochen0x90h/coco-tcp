#include "TcpSocket_Win32.hpp"
#include <iostream>


namespace coco {

TcpSocket_Win32::TcpSocket_Win32(Loop_Win32 &loop)
	: loop(loop)
{
	// initialize winsock
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2,2), &wsaData);
}

TcpSocket_Win32::~TcpSocket_Win32() {
	closesocket(this->socket);
	WSACleanup();
}

Device::State TcpSocket_Win32::state() {
	return this->stat;
}

Awaitable<Device::State> TcpSocket_Win32::untilState(State state) {
	if (this->stat == state)
		return {};
	return {this->stateTasks, state};
}

int TcpSocket_Win32::getBufferCount() {
	return this->buffers.count();
}

TcpSocket_Win32::Buffer &TcpSocket_Win32::getBuffer(int index) {
	return this->buffers.get(index);
}

bool TcpSocket_Win32::connect(const ipv6::Endpoint &endpoint) {
	// create socket
	SOCKET socket = WSASocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (socket == INVALID_SOCKET) {
		int e = WSAGetLastError();
		return false;
	}

	// reuse address/port
	// https://stackoverflow.com/questions/14388706/how-do-so-reuseaddr-and-so-reuseport-differ
	//int reuse = 1;
	//setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

	// bind to any local address/port (required by ConnectEx)
	sockaddr_in6 ep = {.sin6_family = AF_INET6};
	//sockaddr_in ep = {.sin_family = AF_INET};
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

	// convert server endpoint (address/port)
	std::copy(endpoint.address.u8, endpoint.address.u8 + 16, ep.sin6_addr.s6_addr);
	ep.sin6_port = htons(endpoint.port);

	//ep.sin_addr.s_addr = inet_addr("173.194.37.36"); // google.com
    //ep.sin_port = htons(80);
	//getaddrinfo

	// get ConnectEx
	GUID guidConnectEx = WSAID_CONNECTEX;
	LPFN_CONNECTEX ConnectEx = NULL;
	DWORD transferred;
	if (WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidConnectEx, sizeof(guidConnectEx),
		&ConnectEx, sizeof(ConnectEx),
		&transferred, NULL, NULL) != 0)
	{
		int error = WSAGetLastError();
		closesocket(socket);
		return false;
	}

	// connect
	memset(&this->overlapped, 0, sizeof(OVERLAPPED));
	if (ConnectEx(socket, (struct sockaddr *)&ep, sizeof(ep),
		nullptr, 0, // send buffer
		nullptr, // transferred
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

void TcpSocket_Win32::close() {
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

void TcpSocket_Win32::handle(OVERLAPPED *overlapped) {
	if (overlapped == &this->overlapped) {
		// result of ConnectEx
		DWORD transferred;
		DWORD flags;
		auto result = WSAGetOverlappedResult(this->socket, overlapped, &transferred, false, &flags);
		if (!result) {
			// "real" error or cancelled (ERROR_OPERATION_ABORTED): close
			auto error = WSAGetLastError();
			close();
		} else {
			setsockopt(this->socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);

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
	} else {
		for (auto &buffer : this->buffers) {
			if (overlapped == &buffer.overlapped) {
				buffer.handle(overlapped);
				break;
			}
		}
	}
}


// Buffer

TcpSocket_Win32::Buffer::Buffer(TcpSocket_Win32 &socket, int size)
	: BufferImpl(new uint8_t[size], size, socket.stat)
	, socket(socket)
{
	socket.buffers.add(*this);
}

TcpSocket_Win32::Buffer::~Buffer() {
	delete [] this->dat;
}

bool TcpSocket_Win32::Buffer::startInternal(int size, Op op) {
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

void TcpSocket_Win32::Buffer::cancel() {
	if (this->stat != State::BUSY)
		return;

	auto result = CancelIoEx((HANDLE)this->socket.socket, &this->overlapped);
	if (!result) {
		auto e = WSAGetLastError();
		std::cerr << "cancel error " << e << std::endl;
	}
}

void TcpSocket_Win32::Buffer::handle(OVERLAPPED *overlapped) {
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
