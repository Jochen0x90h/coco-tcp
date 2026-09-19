#pragma once

#include <coco/TcpServer.hpp>
#include <coco/IntrusiveList.hpp>
#include <coco/platform/Loop_native.hpp>


namespace coco {

class TcpServer_io_uring : public TcpServer {
public:
    /// @brief Constructor
    /// @param loop event loop
    TcpServer_io_uring(Loop_io_uring &loop);

    ~TcpServer_io_uring() override;

    bool listen(uint16_t protocolId, uint16_t port) override;
    void close() override;


    class Socket;

    /// @brief Buffer for sending/receiving data
    ///
    class Buffer : public coco::Buffer, public Loop_io_uring::CompletionHandler, public IntrusiveListNode {
        friend class Socket;
    public:
        Buffer(Socket &socket, int size);
        ~Buffer() override;

        bool start() override;
        bool cancel() override;

    protected:
        bool transfer();
        void onCompletion(io_uring_cqe &cqe, int);

        Socket &device_;
    };

    /// @brief Server socket that can accept a connection
    ///
    class Socket : public TcpServer::Socket, public Loop_io_uring::CompletionHandler, public IntrusiveListNode {
        friend class TcpServer_Win32;
        friend class Buffer;
    public:
        Socket(TcpServer_io_uring &server);
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
        //void onAccept(OVERLAPPED *overlapped);
        void onCompletion(io_uring_cqe &cqe, int) override;

        TcpServer_io_uring &server_;

        // socket handle
        int socket_ = INVALID_SOCKET;

        // local and remote address
        struct AddressBuffer {
            ip::Endpoint endpoint = {};
            uint8_t extra[16]; // see AcceptEx documentation
        };
        AddressBuffer addressBuffers_[2];
        //OVERLAPPED overlapped_;

        // list of buffers
        IntrusiveList<Buffer> buffers_;
    };

protected:
    Loop_io_uring &loop_;

    // server socket protocol and handle
    int protocolId_ = 0;
    static constexpr int INVALID_SOCKET = -1;
    int socket_ = INVALID_SOCKET;

    // list of sockets
    IntrusiveList<Socket> sockets_;
};

} // namespace coco
