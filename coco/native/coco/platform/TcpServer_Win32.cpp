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
    : TcpServer::Socket(State::DISABLED)
    , server(server)
{
    server.sockets.add(*this);
}

TcpServer_Win32::Socket::~Socket() {
    closesocket(this->socket);
}

//StateTasks<const Device::State, Device::Events> &TcpServer_Win32::Socket::getStateTasks() {
//	return makeConst(this->st);
//}
/*
BufferDevice::State TcpServer_Win32::Socket::state() {
    return this->stat;
}

Awaitable<Device::Condition> TcpServer_Win32::Socket::until(Condition condition) {
    // check if IN_* condition is met
    if ((int(condition) >> int(this->stat)) & 1)
        return {}; // don't wait
    return {this->stateTasks, condition};
}*/

void TcpServer_Win32::Socket::close() {
    // close socket
    closesocket(this->socket);
    this->socket = INVALID_SOCKET;

    // set state
    this->st.state = State::CLOSING;

    // set state of buffers to disabled
    for (auto &buffer : this->buffers) {
        buffer.setDisabled();
    }

    // set state
    this->st.state = State::DISABLED;

    // resume all coroutines waiting for disabled state
    this->st.doAll(Events::ENTER_CLOSING | Events::ENTER_DISABLED);
    //this->stateTasks.doAll([](Condition condition) {
    //	return (condition & (Condition::ENTER_CLOSING | Condition::ENTER_DISABLED)) != 0;
    //});
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
    this->st.state = State::OPENING;

    // enable buffers
    for (auto &buffer : this->buffers) {
        buffer.setReady();
    }

    // resume all coroutines waiting for state change
    this->st.doAll(Events::ENTER_OPENING);
    //this->stateTasks.doAll([](Condition condition) {
    //	return (condition & Condition::ENTER_OPENING) != 0;
    //});

    return true;
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
        this->st.state = State::READY;

        // start pending transfers
        for (auto &buffer : this->transfers) {
            buffer.start();
        }

        // resume all coroutines waiting for state change
        this->st.doAll(Events::ENTER_READY);
        //this->stateTasks.doAll([](Condition condition) {
        //	return (condition & Condition::ENTER_READY) != 0;
        //});
    }
}

void TcpServer_Win32::Socket::handle(OVERLAPPED *overlapped) {
    for (auto &buffer : this->transfers) {
        if (overlapped == &buffer.overlapped) {
            buffer.handle(overlapped);
            break;
        }
    }
}


// Buffer

TcpServer_Win32::Buffer::Buffer(TcpServer_Win32::Socket &device, int size)
    : coco::Buffer(new uint8_t[size], size, device.st.state)
    , device(device)
{
    device.buffers.add(*this);
}

TcpServer_Win32::Buffer::~Buffer() {
    delete [] this->p.data;
}

bool TcpServer_Win32::Buffer::start(Op op) {
    if (this->st.state != State::READY) {
        assert(this->st.state != State::BUSY);
        return false;
    }

    // check if READ or WRITE flag is set
    assert((op & Op::READ_WRITE) != 0);
    this->op = op;

    // add to list of pending transfers
    this->device.transfers.add(*this);

    // start if device is ready
    if (this->device.st.state == Device::State::READY)
        start();

    // set state
    setBusy();

    return true;
}

bool TcpServer_Win32::Buffer::cancel() {
    if (this->st.state != State::BUSY)
        return false;

    auto result = CancelIoEx((HANDLE)this->device.socket, &this->overlapped);
    if (!result) {
        auto e = WSAGetLastError();
        std::cerr << "cancel error " << e << std::endl;
    }

    return true;
}

void TcpServer_Win32::Buffer::start() {
    // initialize overlapped
    memset(&this->overlapped, 0, sizeof(OVERLAPPED));

    int result;
    if ((op & Op::WRITE) == 0) {
        // receive
        WSABUF buffer{this->p.capacity, (CHAR*)(this->p.data)};
        DWORD flags = 0;
        result = WSARecv(this->device.socket, &buffer, 1, nullptr, &flags, &this->overlapped, nullptr);
    } else {
        // send
        WSABUF buffer{this->p.size, (CHAR*)(this->p.data)};
        result = WSASend(this->device.socket, &buffer, 1, nullptr, 0, &this->overlapped, nullptr);
    }
    if (result != 0) {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            // "real" error
            setReady(0);
        }
    }
}

void TcpServer_Win32::Buffer::handle(OVERLAPPED *overlapped) {
    DWORD transferred;
    DWORD flags;
    auto result = WSAGetOverlappedResult(this->device.socket, overlapped, &transferred, false, &flags);
    if (!result) {
        // "real" error or cancelled (ERROR_OPERATION_ABORTED): return zero size
        auto error = WSAGetLastError();
        transferred = 0;
    }

    // remove from list of active transfers
    remove2();

    // transfer finished
    setReady(transferred);
}

} // namespace coco
