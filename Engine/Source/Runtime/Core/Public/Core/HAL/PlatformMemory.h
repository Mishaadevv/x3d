#pragma once

#include "Core/Types.h"

namespace zx::platform {

/// Raw OS heap (process heap). Alignment is 16 bytes.
void* HeapAllocate(usize size);
void* HeapReallocate(void* ptr, usize size);
void HeapFree(void* ptr);

/// Virtual memory. All sizes/addresses must be multiples of the page size (Commit/Decommit) or
/// allocation granularity (Reserve).
void* VirtualReserve(usize size);
bool VirtualCommit(void* address, usize size);
void VirtualDecommit(void* address, usize size);
void VirtualRelease(void* address);

usize GetPageSize();
usize GetAllocationGranularity();

} // namespace zx::platform
