#pragma once

#include <coco/platform/TcpServer_native.hpp>
#include <coco/platform/IpSocket_native.hpp>


using namespace coco;

// drivers for UdpSocketTest
struct Drivers {
    Loop_native loop;

    TcpServer_native server{loop};
    TcpServer_native::Socket serverSocket{server};
    TcpServer_native::Buffer serverBuffer{serverSocket, 4096};

    IpSocket_native clientSocket{loop};
    IpSocket_native::Buffer clientBuffer{clientSocket, 4096};
};

Drivers drivers;
