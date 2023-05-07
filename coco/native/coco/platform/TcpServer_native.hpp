#pragma once

#ifdef _WIN32
#include "TcpServer_Win32.hpp"
namespace coco {
using TcpServer_native = TcpServer_Win32;
}
#endif
