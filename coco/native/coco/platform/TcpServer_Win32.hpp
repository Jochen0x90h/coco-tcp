#pragma once

#include <coco/TcpServer.hpp>
#include <coco/IntrusiveList.hpp>
#define NOMINMAX
#include <winsock2.h> // see https://learn.microsoft.com/en-us/windows/win32/winsock/creating-a-basic-winsock-application
#include <ws2tcpip.h>
#include <mswsock.h>
#include <coco/platform/Loop_native.hpp> // includes Windows.h


namespace coco {

class TcpServer_Win32 : public TcpServer, public Loop_Win32::CompletionHandler {
public:
    /**
     * Constructor
     * @param loop event loop
     */
    TcpServer_Win32(Loop_Win32 &loop);

    ~TcpServer_Win32() override;

    bool listen(uint16_t port) override;
    void close() override;


    class Socket;

    /**
     * Buffer for transferring data to/from a file
     */
    class Buffer : public coco::Buffer, public IntrusiveListNode, public IntrusiveListNode2 {
        friend class Socket;
    public:
        Buffer(Socket &socket, int size);
        ~Buffer() override;

        bool start(Op op) override;
        bool cancel() override;

    protected:
        void start();
        void handle(OVERLAPPED *overlapped);

        Socket &device;
        OVERLAPPED overlapped;
        Op op;
    };


    class Socket : public TcpServer::Socket, public Loop_Win32::CompletionHandler , public IntrusiveListNode {
        friend class TcpServer_Win32;
        friend class Buffer;
    public:
        Socket(TcpServer_Win32 &server);
        ~Socket() override;

        // Device methods
        //StateTasks<const State, Events> &getStateTasks() override;
        void close() override;

        // BufferDevice methods
        int getBufferCount() override;
        Buffer &getBuffer(int index) override;

        // TcpServer::Socket methods
        bool accept() override;

    protected:
        void handleAccept(OVERLAPPED *overlapped);
        void handle(OVERLAPPED *overlapped) override;

        TcpServer_Win32 &server;

        // socket handle
        SOCKET socket = INVALID_SOCKET;
        uint8_t buffer[(sizeof(sockaddr_in6) + 16) * 2];
        OVERLAPPED overlapped;

        // device state
        //StateTasks<State, Events> st = State::DISABLED;

        // list of buffers
        IntrusiveList<Buffer> buffers;

        // pending transfers
        IntrusiveList2<Buffer> transfers;
    };

protected:
    void handle(OVERLAPPED *overlapped) override;

    Loop_Win32 &loop;

    // server socket handle
    SOCKET socket = INVALID_SOCKET;

    LPFN_ACCEPTEX AcceptEx = NULL;

    // list of sockets
    IntrusiveList<Socket> sockets;
};

} // namespace coco
