#include "hybrid/symbolize.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>

namespace tj::hybrid {

struct Sym { uint32_t addr; uint32_t size; char name[80]; };
static std::vector<Sym> g_syms;
static bool g_loaded = false;

static bool TryLoad(const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return false;
    char line[512];
    fgets(line, sizeof(line), f); // header
    while (fgets(line, sizeof(line), f)) {
        // address,size,section,name
        uint32_t addr = 0, size = 0; char sec[32] = {0}, name[80] = {0};
        // parse manually (name may contain commas rarely; take last field to EOL)
        char* p = line;
        addr = (uint32_t)strtoul(p, &p, 16); if (*p==',') ++p;
        size = (uint32_t)strtoul(p, &p, 10); if (*p==',') ++p;
        int i = 0; while (*p && *p!=',' && i<31) sec[i++]=*p++; sec[i]=0; if (*p==',') ++p;
        i = 0; while (*p && *p!='\n' && *p!='\r' && i<79) name[i++]=*p++; name[i]=0;
        Sym s; s.addr = addr; s.size = size; strncpy_s(s.name, name, _TRUNCATE);
        g_syms.push_back(s);
    }
    fclose(f);
    std::sort(g_syms.begin(), g_syms.end(), [](const Sym&a,const Sym&b){return a.addr<b.addr;});
    return !g_syms.empty();
}

void LoadSymbols() {
    if (g_loaded) return;
    g_loaded = true;
    const char* cands[] = {
        "re\\functions.csv",
        "..\\..\\..\\re\\functions.csv",
        "D:\\Projects\\Tom and Jerry in War of the Whiskers (U)\\re\\functions.csv",
    };
    for (const char* c : cands) if (TryLoad(c)) { printf("[sym] loaded %zu symbols from %s\n", g_syms.size(), c); return; }
    printf("[sym] functions.csv not found; VAs will be raw\n");
}

const char* Symbolize(uint32_t va) {
    static char buf[128];
    LoadSymbols();
    if (g_syms.empty()) { _snprintf_s(buf, sizeof(buf), _TRUNCATE, "(0x%08x)", va); return buf; }
    // binary search for nearest addr <= va
    int lo = 0, hi = (int)g_syms.size() - 1, best = -1;
    while (lo <= hi) { int m=(lo+hi)/2; if (g_syms[m].addr<=va){best=m;lo=m+1;} else hi=m-1; }
    if (best < 0) { _snprintf_s(buf, sizeof(buf), _TRUNCATE, "(0x%08x)", va); return buf; }
    uint32_t off = va - g_syms[best].addr;
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s+0x%x (0x%08x)", g_syms[best].name, off, va);
    return buf;
}

} // namespace tj::hybrid
