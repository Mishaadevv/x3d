#include "Core/HAL/PlatformMemory.h"

#include "Windows/WindowsInclude.h"

namespace zx::platform {

namespace {
HANDLE ProcessHeap()
{
    static HANDLE heap = ::GetProcessHeap();
    return heap;
}

const SYSTEM_INFO& SysInfo()
{
    static SYSTEM_INFO si = [] {
        SYSTEM_INFO s = {};
        ::GetSystemInfo(&s);
        return s;
    }();
    return si;
}
} // namespace

void* HeapAllocate(usize size) { return ::HeapAlloc(ProcessHeap(), 0, size ? size : 1); }

void* HeapReallocate(void* ptr, usize size)
{
    if (!ptr)
        return HeapAllocate(size);
    return ::HeapReAlloc(ProcessHeap(), 0, ptr, size ? size : 1);
}

void HeapFree(void* ptr)
{
    if (ptr)
        ::HeapFree(ProcessHeap(), 0, ptr);
}

void* VirtualReserve(usize size) { return ::VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS); }

bool VirtualCommit(void* address, usize size)
{
    return ::VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void VirtualDecommit(void* address, usize size)
{
#pragma warning(suppress : 6250) // decommit without release is intended
    ::VirtualFree(address, size, MEM_DECOMMIT);
}

void VirtualRelease(void* address)
{
    if (address)
        ::VirtualFree(address, 0, MEM_RELEASE);
}

usize GetPageSize() { return SysInfo().dwPageSize; }
usize GetAllocationGranularity() { return SysInfo().dwAllocationGranularity; }

} // namespace zx::platform
