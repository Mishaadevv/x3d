#pragma once

// Single place where <Windows.h> is included inside the Platform module (private sources only).

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00
#endif

#include <Windows.h>

#undef Yield
#undef CreateWindow
#undef GetMessage
#undef SendMessage
#undef PostMessage
#undef DrawText
#undef LoadImage
#undef GetObject
#undef ERROR
