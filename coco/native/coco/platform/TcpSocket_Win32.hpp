#pragma once

#include <coco/TcpSocket.hpp>
#include <coco/IntrusiveList.hpp>
#define NOMINMAX
#include <winsock2.h> // see https://learn.microsoft.com/en-us/windows/win32/winsock/creating-a-basic-winsock-application
#include <ws2tcpip.h>
#include <mswsock.h>
#include <coco/platform/Loop_native.hpp> // includes Windows.h


namespace coco {

class TcpSocket_Win32 : public TcpSocket, public Loop_Win32::CompletionHandler {
public:
    /**
     * Constructor
     * @param loop event loop
     */
    TcpSocket_Win32(Loop_Win32 &loop);

    ~TcpSocket_Win32() override;

    class Buffer;

    // Device methods
    //StateTasks<const State, Events> &getStateTasks() override;
    void close() override;

    // BufferDevice methods
    int getBufferCount() override;
    Buffer &getBuffer(int index) override;

    // TcpSocket methods
    bool connect(const ipv6::Endpoint &endpoint) override;

    /**
     * Buffer for transferring data to/from a file
     */
    class Buffer : public coco::Buffer, public IntrusiveListNode, public IntrusiveListNode2 {
        friend class TcpSocket_Win32;
    public:
        Buffer(TcpSocket_Win32 &device, int size);
        ~Buffer() override;

        bool start(Op op) override;
        bool cancel() override;

    protected:
        void start();
        void handle(OVERLAPPED *overlapped);

        TcpSocket_Win32 &device;
        OVERLAPPED overlapped;
        Op op;
    };

protected:
    void handle(OVERLAPPED *overlapped) override;

    Loop_Win32 &loop;

    // socket handle
    SOCKET socket = INVALID_SOCKET;
    OVERLAPPED overlapped;

    // device state
    //StateTasks<State, Events> st = State::DISABLED;

    // list of buffers
    IntrusiveList<Buffer> buffers;

    // pending transfers
    IntrusiveList2<Buffer> transfers;
};

} // namespace coco
