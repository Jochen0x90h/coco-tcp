#include "TcpServer_Win32.hpp"
#include <iostream>


namespace coco {

TcpServer_Win32::TcpServer_Win32(Loop_Win32 &loop)
    : loop_(loop)
{
    // initialize winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
}

TcpServer_Win32::~TcpServer_Win32() {
    closesocket(socket_);
    WSACleanup();
}

bool TcpServer_Win32::listen(uint16_t protocolId, uint16_t port) {
    if (socket_ != INVALID_SOCKET)
        return false;

    // create socket
    SOCKET socket = WSASocket(protocolId, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (socket == INVALID_SOCKET) {
        int e = WSAGetLastError();
        return false;
    }

    // bind to local port
    sockaddr_in6 local = {.sin6_family = protocolId, .sin6_port = htons(port)};
    if (bind(socket, (struct sockaddr *)&local, sizeof(local)) != 0) {
        int e = WSAGetLastError();
        closesocket(socket);
        return false;
    }

    // add socket to completion port of event loop
    Loop_Win32::CompletionHandler *handler = this;
    if (CreateIoCompletionPort(
        (HANDLE)socket,
        loop_.port,
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
        &AcceptEx, sizeof(AcceptEx),
        &transferred, NULL, NULL) != 0)
    {
        int e = WSAGetLastError();
        closesocket(socket);
        return false;
    }
    protocolId_ = protocolId;
    socket_ = socket;
    return true;
}

void TcpServer_Win32::close() {
    closesocket(socket_);
    socket_ = INVALID_SOCKET;
}

void TcpServer_Win32::handle(OVERLAPPED *overlapped) {
    for (auto &socket : sockets_) {
        if (overlapped == &socket.overlapped_) {
            socket.handleAccept(overlapped);
            break;
        }
    }
}


// TcpServer_Win32::Socket

TcpServer_Win32::Socket::Socket(TcpServer_Win32 &server)
    : TcpServer::Socket(State::DISABLED)
    , server_(server)
{
    server.sockets_.add(*this);
}

TcpServer_Win32::Socket::~Socket() {
    closesocket(socket_);
}

void TcpServer_Win32::Socket::close() {
    if (socket_ == INVALID_SOCKET)
        return;

    // close socket
    closesocket(socket_);
    socket_ = INVALID_SOCKET;

    // clear local and remote address
    addressBuffers_[0].endpoint = {};
    addressBuffers_[1].endpoint = {};

    // set state
    st.set(State::DISABLED);

    // set state of buffers to disabled
    for (auto &buffer : buffers_) {
        buffer.setDisabled();
    }

    // resume all coroutines waiting for disabled state
    st.notify(Events::ENTER_CLOSING | Events::ENTER_DISABLED);
}

int TcpServer_Win32::Socket::getBufferCount() {
    return buffers_.count();
}

TcpServer_Win32::Buffer &TcpServer_Win32::Socket::getBuffer(int index) {
    return buffers_.get(index);
}

bool TcpServer_Win32::Socket::accept() {
    if (socket_ != INVALID_SOCKET)
        return false;

    auto &server = server_;

    // create socket
    SOCKET socket = WSASocket(server.protocolId_, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (socket == INVALID_SOCKET) {
        int e = WSAGetLastError();
        return false;
    }

    // add socket to completion port of event loop
    Loop_Win32::CompletionHandler *handler = this;
    if (CreateIoCompletionPort(
        (HANDLE)socket,
        server.loop_.port,
        ULONG_PTR(handler),
        0) == nullptr)
    {
        int e = WSAGetLastError();
        closesocket(socket);
        return false;
    }

    // accept
    // https://learn.microsoft.com/de-de/windows/win32/api/mswsock/nf-mswsock-acceptex
    memset(&overlapped_, 0, sizeof(OVERLAPPED));
    if (server.AcceptEx(
        server.socket_,
        socket,
        addressBuffers_, // buffer for addresses
        0, // receive size
        sizeof(AddressBuffer), sizeof(AddressBuffer), // sizes for local and remote address
        nullptr,
        &overlapped_) == FALSE)
    {
        int error = WSAGetLastError();
        if (error != ERROR_IO_PENDING) {
            // "real" error
            closesocket(socket);
            return false;
        }
    }
    socket_ = socket;

    // set state
    st.set(State::OPENING);

    // enable buffers
    for (auto &buffer : buffers_) {
        buffer.setReady();
    }

    // resume all coroutines waiting for state change
    st.notify(Events::ENTER_OPENING);

    return true;
}

ip::Endpoint &TcpServer_Win32::Socket::getEndpoint(bool remote) {
    return addressBuffers_[int(remote)].endpoint;
}

void TcpServer_Win32::Socket::handleAccept(OVERLAPPED *overlapped) {
    // result of AcceptEx
    DWORD transferred;
    DWORD flags;
    auto result = WSAGetOverlappedResult(socket_, overlapped, &transferred, false, &flags);
    if (!result) {
        // "real" error or cancelled (ERROR_OPERATION_ABORTED): close
        auto error = WSAGetLastError();
        close();
    } else {
        setsockopt(socket_, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&server_.socket_, sizeof(SOCKET));

        // set state
        st.set(State::READY);

        // start pending transfers
        for (auto &buffer : transfers_) {
            buffer.start();
        }

        // resume all coroutines waiting for state change
        st.notify(Events::ENTER_READY);
    }
}

void TcpServer_Win32::Socket::handle(OVERLAPPED *overlapped) {
    for (auto &buffer : transfers_) {
        if (overlapped == &buffer.overlapped_) {
            buffer.handle(overlapped);
            break;
        }
    }
}


// TcpServer_Win32::Buffer

TcpServer_Win32::Buffer::Buffer(TcpServer_Win32::Socket &device, int size)
    : coco::Buffer(new uint8_t[size], size, device.st.state)
    , device_(device)
{
    device.buffers_.add(*this);
}

TcpServer_Win32::Buffer::~Buffer() {
    delete [] data_;
}

bool TcpServer_Win32::Buffer::start(Op op) {
    if (st.state != State::READY) {
        assert(st.state != State::BUSY);
        return false;
    }

    // check if READ or WRITE flag is set
    assert((op & Op::READ_WRITE) != 0);
    op_ = op;

    // add to list of pending transfers
    device_.transfers_.add(*this);

    // start if device is ready
    if (device_.st.state == Device::State::READY)
        start();

    // set state
    setBusy();

    return true;
}

bool TcpServer_Win32::Buffer::cancel() {
    if (st.state != State::BUSY)
        return false;

    auto result = CancelIoEx((HANDLE)device_.socket_, &overlapped_);
    if (!result) {
        auto e = WSAGetLastError();
        std::cerr << "cancel error " << e << std::endl;
    }

    return true;
}

void TcpServer_Win32::Buffer::start() {
    // initialize overlapped
    memset(&overlapped_, 0, sizeof(OVERLAPPED));

    int result;
    if ((op_ & Op::WRITE) == 0) {
        // receive
        WSABUF buffer{capacity_, (CHAR*)(data_)};
        DWORD flags = 0;
        result = WSARecv(device_.socket_, &buffer, 1, nullptr, &flags, &overlapped_, nullptr);
    } else {
        // send
        WSABUF buffer{size_, (CHAR*)(data_)};
        result = WSASend(device_.socket_, &buffer, 1, nullptr, 0, &overlapped_, nullptr);
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
    auto result = WSAGetOverlappedResult(device_.socket_, overlapped, &transferred, false, &flags);
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
