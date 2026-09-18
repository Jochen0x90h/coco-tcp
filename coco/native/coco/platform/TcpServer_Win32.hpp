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
    /// @brief Constructor
    /// @param loop event loop
    TcpServer_Win32(Loop_Win32 &loop);

    ~TcpServer_Win32() override;

    bool listen(uint16_t protocolId, uint16_t port) override;
    void close() override;


    class Socket;

    /// @brief Buffer for sending/receiving data
    ///
    class Buffer : public coco::Buffer, public IntrusiveListNode {//, public IntrusiveListNode2 {
        friend class Socket;
    public:
        Buffer(Socket &socket, int size);
        ~Buffer() override;

        bool start() override;
        bool cancel() override;

    protected:
        bool transfer();
        void onCompletion(OVERLAPPED *overlapped);

        Socket &device_;
        OVERLAPPED overlapped_;
    };

    /// @brief Server socket that can accept a connection
    ///
    class Socket : public TcpServer::Socket, public Loop_Win32::CompletionHandler, public IntrusiveListNode {
        friend class TcpServer_Win32;
        friend class Buffer;
    public:
        Socket(TcpServer_Win32 &server);
        ~Socket() override;

        // Device methods
        void close() override;

        // BufferDevice methods
        int getBufferCount() override;
        Buffer &getBuffer(int index) override;

        // TcpServer::Socket methods
        bool accept() override;
        ip::Endpoint &getEndpoint(bool remote) override;

    protected:
        void onAccept(OVERLAPPED *overlapped);
        void onCompletion(OVERLAPPED *overlapped) override;

        TcpServer_Win32 &server_;

        // socket handle
        SOCKET socket_ = INVALID_SOCKET;

        // local and remote address
        struct AddressBuffer {
            ip::Endpoint endpoint = {};
            uint8_t extra[16]; // see AcceptEx documentation
        };
        AddressBuffer addressBuffers_[2];
        OVERLAPPED overlapped_;

        // list of buffers
        IntrusiveList<Buffer> buffers_;

        // pending transfers
        //IntrusiveList2<Buffer> transfers_;
    };

protected:
    void onCompletion(OVERLAPPED *overlapped) override;

    Loop_Win32 &loop_;

    // server socket protocol and handle
    int protocolId_ = 0;
    SOCKET socket_ = INVALID_SOCKET;

    LPFN_ACCEPTEX AcceptEx = NULL;

    // list of sockets
    IntrusiveList<Socket> sockets_;
};

} // namespace coco
