// Native xboxkrnl shim layer. The XBE's kernel imports are resolved through a thunk
// table at VA 0xEE000 (105 entries, each originally holding 0x80000000|ordinal). We
// overwrite each slot with the address of a native shim (FUNC) or a data blob (VAR).
#pragma once
#include <cstdint>

namespace tj::hybrid {

// Fill the kernel thunk table at 0xEE000: read each encoded ordinal and replace it
// with the matching native shim/VAR address. Unknown ordinals get a logging trap that
// prints the ordinal+name and returns 0, so bring-up surfaces missing imports cleanly.
void FillKernelThunks(uint32_t thunkTableVa);

// Human-readable name for a kernel ordinal (for logging). "" if unknown.
const char* KernelOrdinalName(int ordinal);

} // namespace tj::hybrid
