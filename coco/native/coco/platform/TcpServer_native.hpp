#pragma once

#ifdef _WIN32
#include "TcpServer_Win32.hpp"
namespace coco {
using TcpServer_native = TcpServer_Win32;
}
#endif

#ifdef __linux__
#include "TcpServer_io_uring.hpp"
namespace coco {
using TcpServer_native = TcpServer_io_uring;
}
#endif
