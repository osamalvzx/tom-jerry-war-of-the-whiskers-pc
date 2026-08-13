#include "runtime/kernel/xboxkrnl.h"
#include <windows.h>
#include <unordered_map>
#include <mutex>

namespace tj::kernel {
namespace {
std::mutex g_lock;
std::unordered_map<void*, uint32_t> g_sizes;   // track allocation sizes for XPhysicalSize
uint64_t g_qpcFreq = 0;

void* AlignedAlloc(uint32_t bytes, uint32_t alignment) {
    if (alignment < 16) alignment = 16;
    void* p = _aligned_malloc(bytes, alignment);
    if (p) {
        std::lock_guard<std::mutex> g(g_lock);
        g_sizes[p] = bytes;
    }
    return p;
}
} // namespace

void Init() {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_qpcFreq = (uint64_t)f.QuadPart;
}

void* MmAllocateContiguousMemory(uint32_t bytes) { return AlignedAlloc(bytes, 4096); }

void* MmAllocateContiguousMemoryEx(uint32_t bytes, uint32_t, uint32_t, uint32_t alignment, uint32_t) {
    return AlignedAlloc(bytes, alignment ? alignment : 4096);
}

void MmFreeContiguousMemory(void* p) {
    if (!p) return;
    { std::lock_guard<std::mutex> g(g_lock); g_sizes.erase(p); }
    _aligned_free(p);
}

uint32_t MmGetPhysicalAddress(void* p) { return (uint32_t)(uintptr_t)p; }

void*    XPhysicalAlloc(uint32_t bytes) { return AlignedAlloc(bytes, 4096); }
void     XPhysicalFree(void* p) { MmFreeContiguousMemory(p); }
uint32_t XPhysicalSize(void* p) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_sizes.find(p);
    return it == g_sizes.end() ? 0 : it->second;
}

void* ExAllocatePool(uint32_t bytes) { return AlignedAlloc(bytes, 16); }
void  ExFreePool(void* p) {
    if (!p) return;
    { std::lock_guard<std::mutex> g(g_lock); g_sizes.erase(p); }
    _aligned_free(p);
}

uint64_t KeQueryPerformanceCounter() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)c.QuadPart;
}
uint64_t KeQueryPerformanceFrequency() { return g_qpcFreq; }

uint64_t KeQuerySystemTime() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

} // namespace tj::kernel
