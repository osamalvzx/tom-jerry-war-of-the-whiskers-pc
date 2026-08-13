// Map a code VA back to the nearest function name (from re/functions.csv) for crash
// diagnostics during the bring-up. Loaded lazily from disk.
#pragma once
#include <cstdint>

namespace tj::hybrid {
// Load re/functions.csv (searches a few known locations). Safe to call repeatedly.
void LoadSymbols();
// Format "NAME+0xNN (0xVA)" for the nearest function at/below va into a static buffer.
const char* Symbolize(uint32_t va);
} // namespace tj::hybrid
