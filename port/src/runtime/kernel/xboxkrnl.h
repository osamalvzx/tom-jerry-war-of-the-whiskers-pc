// Native replacements for the xboxkrnl.exe imports the game uses (105 of them).
// This first slice covers the foundational ones — memory, timing, synchronization —
// which are thin wrappers over Win32. The Xbox kernel exposes these by ordinal; the
// ported game code (and the eventual XBE loader) binds to these implementations.
#pragma once
#include <cstdint>

namespace tj::kernel {

// --- Physical / contiguous memory (Xbox has a unified 64 MB pool; we use Win32) ---
// XPhysicalAlloc / MmAllocateContiguousMemory: page-aligned, optionally within a range.
void*    MmAllocateContiguousMemory(uint32_t bytes);
void*    MmAllocateContiguousMemoryEx(uint32_t bytes, uint32_t lowest, uint32_t highest,
                                      uint32_t alignment, uint32_t protect);
void     MmFreeContiguousMemory(void* p);
uint32_t MmGetPhysicalAddress(void* p);       // identity within our model
void*    XPhysicalAlloc(uint32_t bytes);
void     XPhysicalFree(void* p);
uint32_t XPhysicalSize(void* p);

// --- Heap-ish pool allocation (ExAllocatePool / XMemAlloc live in xapi, see xapi.h) ---
void*    ExAllocatePool(uint32_t bytes);
void     ExFreePool(void* p);

// --- Timing ---
uint64_t KeQueryPerformanceCounter();
uint64_t KeQueryPerformanceFrequency();
uint64_t KeQuerySystemTime();                 // 100 ns units since 1601 (Win32 FILETIME epoch)

// Initialize the kernel runtime (call once at startup).
void Init();

} // namespace tj::kernel
