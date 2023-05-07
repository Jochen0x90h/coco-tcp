#include "TcpSocket_Win32.hpp"
#include <iostream>


namespace coco {

TcpSocket_Win32::TcpSocket_Win32(Loop_Win32 &loop)
    : TcpSocket(State::DISABLED)
    , loop(loop)
{
    // initialize winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
}

TcpSocket_Win32::~TcpSocket_Win32() {
    closesocket(this->socket);
    WSACleanup();
}

//StateTasks<const Device::State, Device::Events> &TcpSocket_Win32::getStateTasks() {
//	return makeConst(this->st);
//}

/*
BufferDevice::State TcpSocket_Win32::state() {
    return this->stat;
}

Awaitable<Device::Condition> TcpSocket_Win32::until(Condition condition) {
    // check if IN_* condition is met
    if ((int(condition) >> int(this->stat)) & 1)
        return {}; // don't wait
    return {this->stateTasks, condition};
}*/

void TcpSocket_Win32::close() {
    // close socket
    closesocket(this->socket);
    this->socket = INVALID_SOCKET;

    // set state
    this->st.state = State::CLOSING;

    // disable buffers
    for (auto &buffer : this->buffers) {
        buffer.setDisabled();
    }

    // set state
    this->st.state = State::DISABLED;

    // resume all coroutines waiting for state change
    this->st.doAll(Events::ENTER_CLOSING | Events::ENTER_DISABLED);
    //this->stateTasks.doAll([](Condition condition) {
    //	return (condition & (Condition::ENTER_CLOSING | Condition::ENTER_DISABLED)) != 0;
    //});
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
    } else {
        for (auto &buffer : this->transfers) {
            if (overlapped == &buffer.overlapped) {
                buffer.handle(overlapped);
                break;
            }
        }
    }
}


// Buffer

TcpSocket_Win32::Buffer::Buffer(TcpSocket_Win32 &device, int size)
    : coco::Buffer(new uint8_t[size], size, device.st.state)
    , device(device)
{
    device.buffers.add(*this);
}

TcpSocket_Win32::Buffer::~Buffer() {
    delete [] this->p.data;
}

bool TcpSocket_Win32::Buffer::start(Op op) {
    if (this->st.state != State::READY) {
        assert(this->st.state != State::BUSY);
        return false;
    }

    // check if READ or WRITE flag is set
    assert((op & Op::READ_WRITE) != 0);

    this->op = op;

/*
    // initialize overlapped
    memset(&this->overlapped, 0, sizeof(OVERLAPPED));

    int result;
    if ((op & Op::WRITE) == 0) {
        // receive
        WSABUF buffer{this->p.capacity, (CHAR*)this->p.data};
        DWORD flags = 0;
        result = WSARecv(this->device.socket, &buffer, 1, nullptr, &flags, &this->overlapped, nullptr);
    } else {
        // send
        WSABUF buffer{this->p.size, (CHAR*)this->p.data};
        result = WSASend(this->device.socket, &buffer, 1, nullptr, 0, &this->overlapped, nullptr);
    }
    if (result != 0) {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            // "real" error
            setReady(0);
            return false;
        }
    }
*/

    // add to list of pending transfers
    this->device.transfers.add(*this);

    // start if device is ready
    if (this->device.st.state == Device::State::READY)
        start();

    // set state
    setBusy();

    return true;
}

bool TcpSocket_Win32::Buffer::cancel() {
    if (this->st.state != State::BUSY)
        return false;

    auto result = CancelIoEx((HANDLE)this->device.socket, &this->overlapped);
    if (!result) {
        auto e = WSAGetLastError();
        std::cerr << "cancel error " << e << std::endl;
    }

    return true;
}

void TcpSocket_Win32::Buffer::start() {
    // initialize overlapped
    memset(&this->overlapped, 0, sizeof(OVERLAPPED));

    int result;
    if ((this->op & Op::WRITE) == 0) {
        // receive
        WSABUF buffer{this->p.capacity, (CHAR*)this->p.data};
        DWORD flags = 0;
        result = WSARecv(this->device.socket, &buffer, 1, nullptr, &flags, &this->overlapped, nullptr);
    } else {
        // send
        WSABUF buffer{this->p.size, (CHAR*)this->p.data};
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

void TcpSocket_Win32::Buffer::handle(OVERLAPPED *overlapped) {
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
