#pragma once

#include <coco/platform/TcpServer_native.hpp>
#include <coco/platform/TcpSocket_native.hpp>


using namespace coco;

// drivers for UdpSocketTest
struct Drivers {
    Loop_native loop;

    TcpServer_native server{loop};
    TcpServer_native::Socket serverSocket{server};
    TcpServer_native::Buffer serverBuffer{serverSocket, 4096};

    TcpSocket_native clientSocket{loop};
    TcpSocket_native::Buffer clientBuffer{clientSocket, 4096};
};

Drivers drivers;
