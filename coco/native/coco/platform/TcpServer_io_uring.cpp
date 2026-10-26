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
    //addressBuffers_[0].endpoint = {};
    //addressBuffers_[1].endpoint = {};

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
    if (state_ == State::OPENING || socket_ != INVALID_SOCKET)
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
    endpointSize_ = sizeof(endpoint_);
    bool result = server_.loop_.accept(*this,
        server_.socket_, // listening socket
        &endpoint_, &endpointSize_); // remote address and port
    if (!result) {
        // error: submit queue full
        setError(std::errc::resource_unavailable_try_again);
        return false;
    }

    // set state
    state_ = State::OPENING;

    // resume all coroutines waiting for state change
    notify(Events::ENTER_OPENING);

    return true;
}

ip::Endpoint TcpServer_io_uring::Socket::getEndpoint(bool remote) {
    if (!remote) {
        // local
        ip::Endpoint endpoint = {};
        socklen_t endpointSize = sizeof(endpoint);
        getsockname(socket_, reinterpret_cast<struct sockaddr*>(&endpoint), &endpointSize);
        return endpoint;
    } else {
        // remote
        return endpoint_;
    }
}

void TcpServer_io_uring::Socket::onCompletion(io_uring_cqe &cqe, int) {
   int result = cqe.res;
    if (result < 0) {
        // accept error
        int error = -result;
        setSystemError(error);

        // set state
        state_ = State::DISABLED;

        // resume all coroutines waiting for state change
        notify(Events::ENTER_DISABLED);
    } else {
        // accept success
        socket_ = result;

        // set state
        state_ = State::READY;

        // enable buffers
        for (auto &buffer : buffers_) {
            buffer.setReady();
        }

        // resume all coroutines waiting for state change
        notify(Events::ENTER_READY);
    }
}


// TcpServer_io_uring::Buffer

TcpServer_io_uring::Buffer::Buffer(TcpServer_io_uring::Socket &device, int size)
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
        if (!device_.server_.loop_.cancel(*this)) {
            // error: submit buffer full
            setError(std::errc::resource_unavailable_try_again);
            return false;
        }
        steps_ = 0;
    }
    return true;
}

bool TcpServer_io_uring::Buffer::transfer() {
    if (!device_.server_.loop_.transfer(*this, (Op(steps_) & Op::WRITE) == 0 ? IORING_OP_RECV : IORING_OP_SEND,
        device_.socket_, data_, size_))
    {
        // error: submit queue full
        setError(std::errc::resource_unavailable_try_again);
        setReady();
        return false;
    }
    return true;    
}

void TcpServer_io_uring::Buffer::onCompletion(io_uring_cqe &cqe, int id) {
   int result = cqe.res;
    if (result < 0) {
        // error
        // ECANCELED: cancelled
        // EMSGSIZE: datagram was larger than buffer (for UDP)
        int error = -result;
        setSystemError(error);
    } else {
        // success
        int transferred = result;

        // check for connection closed by peer
        if (Op(steps_) == Op::READ && transferred == 0) {
            device_.close();
            return;
        }

        // check for read after write
        if (Op(steps_) == Op::READ_WRITE) {
            steps_ = int(Op::READ);
            transfer();
            return;
        }

        setSuccess(transferred);
    }

    // transfer finished
    setReady();
}

} // namespace coco
