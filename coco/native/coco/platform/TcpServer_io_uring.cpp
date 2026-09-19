#include "TcpServer_io_uring.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>


namespace coco {

TcpServer_io_uring::TcpServer_io_uring(Loop_io_uring &loop)
    : loop_(loop)
{
}

TcpServer_io_uring::~TcpServer_io_uring() {
    ::close(socket_);
}

bool TcpServer_io_uring::listen(uint16_t protocolId, uint16_t port) {
    if (socket_ != INVALID_SOCKET)
        return false;

    // create socket
    int socket = ::socket(protocolId, SOCK_STREAM /*| SOCK_NONBLOCK*/, 0);
    if (socket == INVALID_SOCKET) {
        int error = errno;
        //setSystemError(error);
        return false;
    }

    sockaddr_in6 local = {.sin6_family = protocolId, .sin6_port = htons(port)};

    // reuse address
    int opt = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        goto error;

    // bind to local port
    if (bind(socket, (struct sockaddr *)&local, sizeof(local)) < 0)
        goto error;

    // listen
    if (::listen(socket, 100) < 0)
        goto error;

    protocolId_ = protocolId;
    socket_ = socket;
    return true;
error:
    int error = errno;
    //setSystemError(error);
    ::close(socket);
    return false;
}

void TcpServer_io_uring::close() {
    ::close(socket_);
    socket_ = INVALID_SOCKET;
}


// TcpServer_io_uring::Socket

TcpServer_io_uring::Socket::Socket(TcpServer_io_uring &server)
    : TcpServer::Socket(State::DISABLED)
    , server_(server)
{
    server.sockets_.add(*this);
}

TcpServer_io_uring::Socket::~Socket() {
    ::close(socket_);
}

void TcpServer_io_uring::Socket::close() {
    if (socket_ == INVALID_SOCKET)
        return;

    // close socket
    ::close(socket_);
    socket_ = INVALID_SOCKET;

    // clear local and remote address
    addressBuffers_[0].endpoint = {};
    addressBuffers_[1].endpoint = {};

    // set state
    state_ = State::DISABLED;

    // set state of buffers to disabled
    for (auto &buffer : buffers_) {
        buffer.setDisabled();
    }

    // resume all coroutines waiting for disabled state
    notify(Events::ENTER_CLOSING | Events::ENTER_DISABLED);
}

int TcpServer_io_uring::Socket::getBufferCount() {
    return buffers_.count();
}

TcpServer_io_uring::Buffer &TcpServer_io_uring::Socket::getBuffer(int index) {
    return buffers_.get(index);
}

bool TcpServer_io_uring::Socket::accept() {
    if (socket_ != INVALID_SOCKET)
        return false;

    auto &server = server_;

    // create socket
    int socket = ::socket(server.protocolId_, SOCK_STREAM, 0);
    if (socket == INVALID_SOCKET) {
        int error = errno;
        setSystemError(error);
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
    state_ = State::OPENING;

    // enable buffers
    for (auto &buffer : buffers_) {
        buffer.setReady();
    }

    // resume all coroutines waiting for state change
    notify(Events::ENTER_OPENING);

    return true;
}

ip::Endpoint &TcpServer_io_uring::Socket::getEndpoint(bool remote) {
    return addressBuffers_[int(remote)].endpoint;
}

void TcpServer_io_uring::Socket::onAccept(OVERLAPPED *overlapped) {
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
        state_ = State::READY;

        // start pending transfers
        //for (auto &buffer : transfers_) {
        //    buffer.start();
        //}

        // resume all coroutines waiting for state change
        notify(Events::ENTER_READY);
    }
}

void TcpServer_io_uring::Socket::onCompletion(OVERLAPPED *overlapped) {
    /*for (auto &buffer : transfers_) {
        if (overlapped == &buffer.overlapped_) {
            buffer.handle(overlapped);
            break;
        }
    }*/
    // search the buffer that caused the event
    for (auto &buffer : buffers_) {
        if (overlapped == &buffer.overlapped_) {
            buffer.onCompletion(overlapped);
            break;
        }
    }
}


// TcpServer_io_uring::Buffer

TcpServer_io_uring::Buffer::Buffer(TcpServer_Win32::Socket &device, int size)
    : coco::Buffer(new uint8_t[size], size, device.state_)
    , device_(device)
{
    device.buffers_.add(*this);
}

TcpServer_io_uring::Buffer::~Buffer() {
    delete [] data_;
}

bool TcpServer_io_uring::Buffer::start() {
    if (state_ != State::READY) {
        assert(false);
        setError(std::errc::resource_unavailable_try_again);
        return false;
    }
    if ((op_ & Op::READ_WRITE) == 0 || size_ == 0) {
        setSuccess();
        return false;
    }

    // store read/write flags for use in transfer(), onCompletion() and cancel()
    steps_ = uint8_t(op_ & Op::READ_WRITE);

    // start transfer
    if (!transfer())
        return false;

    // set state
    setBusy();

    return true;
}

bool TcpServer_io_uring::Buffer::cancel() {
    if (state_ != State::BUSY)
        return false;

    if (steps_ != 0) {
        auto result = CancelIoEx((HANDLE)device_.socket_, &overlapped_);
        if (!result) {
            // error
            auto error = WSAGetLastError();
            setSystemError(error);
            //debug::out << "cancel error " << dec(e) << '\n';
            return false;
        }
        steps_ = 0;
    }
    return true;
}

bool TcpServer_io_uring::Buffer::transfer() {
    // initialize overlapped
    memset(&overlapped_, 0, sizeof(OVERLAPPED));

    int result;
    if ((Op(steps_) & Op::WRITE) == 0) {
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
            // error
            setSystemError(error);
            setReady();
            return false;        }
    }
    return true;
}

void TcpServer_io_uring::Buffer::onCompletion(OVERLAPPED *overlapped) {
    DWORD transferred;
    DWORD flags;
    auto result = WSAGetOverlappedResult(device_.socket_, overlapped, &transferred, false, &flags);
    if (result) {
        // success, check for read after write
        if (Op(steps_) == Op::READ_WRITE) {
            steps_ = int(Op::READ);
            transfer();
            return;
        }
        setSuccess(transferred);
    } else {
        // error
        // canceled: ERROR_OPERATION_ABORTED
        auto error = WSAGetLastError();
        setSystemError(error);
    }

    // transfer finished
    setReady();
}

} // namespace coco
