// NULL net_lan for the headless aarch64 build (Stage 5). Gate S5c's runs are
// single-player scripted matches with TJ_NET unset, so nothing here ever arms —
// this file exists so lan_ui / lan_match / net_sync / d3d8_bridge link. Every query
// answers exactly what the Windows build answers when no session is open (LAN_OFF,
// zero games, empty slots), so the frontend hooks behave identically. The real
// Winsock->BSD port is Stage 7 work (ANDROID_PLAN §2.7: "mechanical") and replaces
// this file wholesale in the CMake list.
#if !defined(_WIN32)
#include "hybrid/net_lan.h"
#include <cstdio>
#include <cstring>

namespace tj::hybrid {

bool NetInit() { return false; }
bool NetArmed() { return false; }
bool NetTickBegin(uint32_t) { return true; }
void NetTickEnd(uint32_t) {}
const NetInput* NetPad(int) { return nullptr; }
void NetStatus(char* out, int cap) { if (out && cap) out[0] = 0; }

bool LanOpen(bool, const char*, const char*) {
    printf("[lan] headless build: LAN is Stage-7 work (session refused)\n");
    return false;
}
void LanClose(const char*) {}
LanState LanGetState() { return LAN_OFF; }
bool LanIsHost() { return false; }
void LanPump() {}

int  LanGameCount() { return 0; }
const LanGameInfo* LanGameAt(int) { return nullptr; }
bool LanJoinGame(int, const char*) { return false; }
bool LanJoinIp(const char*, const char*) { return false; }

const LanSlotInfo* LanSlot(int) { return nullptr; }
int  LanSlotCount() { return 0; }
int  LanLocalSlot() { return -1; }
const char* LanStatusLine() { return ""; }
LanNak LanLastNak() { return NAK_NONE; }
const char* LanNakText() { return ""; }

void LanSetReady(bool) {}
void LanCycleChar(int) {}
void LanSetReadyFor(int, bool) {}
void LanCycleCharFor(int, int) {}
void LanCycleTeamFor(int, int) {}
void LanCycleCostumeFor(int, int) {}
int  LanLocalCount() { return 1; }
int  LanLocalSlotAt(int) { return -1; }
void LanCycleTeam(int) {}
void LanHostCycleTeam(int, int) {}
void LanHostCycleChar(int, int) {}
void LanHostCycleCostume(int, int) {}
int  LanCostumeCount(int) { return 1; }
const char* LanStartRefusal() { return "LAN unavailable"; }
void LanCycleCostume(int) {}
void LanSetName(const char*) {}
const char* LanGetName() { return "PLAYER"; }
void LanHostSetRules(uint32_t, uint8_t, uint8_t, uint8_t) {}
bool     LanArenaSelectable(int arena) { return arena != 10 && arena != 12; }
uint8_t  LanConfigArena() { return 0; }
uint8_t  LanConfigMode() { return 0; }
uint8_t  LanConfigRounds() { return 0; }
uint8_t  LanConfigMeat() { return 0; }
uint32_t LanConfigTime() { return 0; }
uint32_t LanPingMs() { return 0; }
void LanHostSetArena(uint8_t) {}
void LanHostSetMode(uint8_t) {}
void LanHostToggleCpu(int) {}
void LanHostKick(int) {}
bool LanHostCanStart() { return false; }
void LanHostStart() {}
void LanLeave() {}

bool LanConsumeLaunch() { return false; }
void LanMatchEnded() {}

const LanMatchConfig* LanConfig() { return nullptr; }   // no session, ever (matches LAN_OFF)
uint32_t LanConfigCrc(const LanMatchConfig* c) {
    // net_lan.cpp's exact Crc32, so a stored value can never disagree across a future
    // Stage-7 swap-in.
    const uint8_t* b = (const uint8_t*)c;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < sizeof(*c); ++i) {
        crc ^= b[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

} // namespace tj::hybrid
#endif // !_WIN32
