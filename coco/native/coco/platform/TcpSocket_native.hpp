#pragma once

#ifdef _WIN32
#include "TcpSocket_Win32.hpp"
namespace coco {
using TcpSocket_native = TcpSocket_Win32;
}
#endif
