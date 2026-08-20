// Phase-2 LAN: UDP transport, discovery/handshake, lobby session, delay-lockstep barrier.
//
// Two layers live behind this header and they arm independently:
//   TRANSPORT + LOCKSTEP -- the proven stage-1c machinery. While ARMED, every Xbox port is
//     served from the agreed network inputs and the game's update is gated on a barrier.
//   SESSION -- discovery beacons, the server browser, JOIN/NAK, the host-authoritative
//     lobby, and the START/READY/GO handshake that ends by arming the lockstep.
// A session can be OPEN (browsing/lobby) without being ARMED: the frontend must stay
// locally driven until the match actually launches.
//
// Design + staged plan: port/LAN_PLAN.md
#pragma once
#include <cstdint>

namespace tj::hybrid {

// One frame of one player's controls -- the ONLY thing that crosses the wire. Positions,
// health and item state are never sent: each peer derives them by running the same
// simulation (see the Stage-1 determinism proof in PROJECT_STATUS.md).
struct NetInput {
    uint16_t buttons;      // Xbox wButtons
    uint8_t  analog[8];    // A,B,X,Y,Black,White,LT,RT
    int16_t  thumb[4];     // LX,LY,RX,RY
};

// ---------------------------------------------------------------- lockstep (stage 1c)
// TJ_NET=host | join:<ip> arms this directly at boot (the determinism harness). The LAN
// UI instead opens a session and arms at GO.
bool NetInit();
bool NetArmed();

// Called around the game's update phase. Begin returns FALSE when the inputs for this
// frame have not arrived yet -- the caller must then skip the whole update (freezing the
// simulation) and try again next frame.
bool NetTickBegin(uint32_t frame);
void NetTickEnd(uint32_t frame);

// The agreed inputs for the frame currently being stepped (nullptr if that port is unused).
const NetInput* NetPad(int port);

// Per-400-frame diagnostics appended to the frame log.
void NetStatus(char* out, int cap);

// ---------------------------------------------------------------- session (stage 3)
enum LanState : uint8_t {
    LAN_OFF = 0,     // no socket
    LAN_BROWSE,      // socket open, listening for beacons (client, not in a game)
    LAN_JOINING,     // JOIN_REQ sent, waiting for JOIN_RSP
    LAN_LOBBY,       // in a lobby (host or client)
    LAN_STARTING,    // START seen; waiting for everyone's BARRIER_READY / for GO
    LAN_MATCH,       // armed, match running
};

enum LanNak : uint8_t {
    NAK_NONE = 0, NAK_FULL, NAK_BUILD, NAK_DATA, NAK_PROTO, NAK_INMATCH, NAK_PASSWORD,
};

struct LanGameInfo {           // one row of the server browser
    uint32_t sessionId;
    uint32_t ip;               // network order
    uint16_t port;
    char     host[16];         // host player's name
    uint8_t  players, maxPlayers;
    uint8_t  mode;             // 0 = quick match, 1 = tournament
    uint8_t  arena;
    uint8_t  locked;           // password required (the password itself never rides here)
    uint8_t  inMatch;
    uint8_t  compatible;       // SIM CONTRACT + data hashes match ours -- this gates joining
    uint8_t  sameBuild;        // advisory: identical module bytes (false for PC<->phone)
    uint8_t  notedBuild;       // internal: the "different build" line has been printed
    uint32_t rttMs;
    uint32_t ageMs;
};

struct LanSlotInfo {
    uint8_t  kind;             // 0 open, 1 human, 2 cpu
    uint8_t  isLocal;          // this PC owns it
    uint8_t  charId, costume, team, ready;
    char     name[16];
};

// Opens the socket if needed. host==true creates a game (name/password from the args),
// otherwise the browser starts listening. Safe to call repeatedly.
bool LanOpen(bool host, const char* gameName, const char* password);
void LanClose(const char* why);
LanState LanGetState();
bool LanIsHost();

// Pumped once per presented frame from the frontend tick: beacons, retries, timeouts.
void LanPump();

// Server browser (rows are rebuilt from the beacon table, newest activity first).
int  LanGameCount();
const LanGameInfo* LanGameAt(int i);
bool LanJoinGame(int i, const char* password);   // -> LAN_JOINING
bool LanJoinIp(const char* ip, const char* password);

// Lobby view (valid in LAN_LOBBY / LAN_STARTING / LAN_MATCH).
const LanSlotInfo* LanSlot(int i);
int  LanSlotCount();
int  LanLocalSlot();
const char* LanStatusLine();     // one line of human-readable session status
LanNak LanLastNak();
const char* LanNakText();

// Lobby actions. Client actions become INTENT packets; host actions apply directly.
void LanSetReady(bool ready);
void LanCycleChar(int delta);
// Seat-addressed forms, for when more than one person is sitting at this PC. Each drives their
// OWN seat; a seat this machine does not own is ignored. The no-argument forms above are these
// applied to the primary seat.
void LanSetReadyFor(int slot, bool ready);
void LanCycleCharFor(int slot, int delta);
void LanCycleTeamFor(int slot, int delta);
void LanCycleCostumeFor(int slot, int delta);
// How many people are playing on this PC, and the seat the n-th of them occupies (-1 if none).
int  LanLocalCount();
int  LanLocalSlotAt(int localIndex);
// Team letters (0..3 = A..D). Your own seat is yours; every other seat is the host's. The
// LAYOUT (melee vs teams) is never chosen -- it is derived from the letters at launch, the
// way retail's own setup screen derives it, so it never has to ride on the wire.
void LanCycleTeam(int delta);
void LanHostCycleTeam(int slot, int delta);
void LanHostCycleChar(int slot, int delta);
void LanHostCycleCostume(int slot, int delta);
// How many costumes a character HAS -- the static, save-independent ceiling, not the count of
// the ones this PC happens to have earned. Ids are contiguous 0..n-1, so index == id.
int  LanCostumeCount(int charId);
// Why the host cannot start yet, or nullptr when it can -- shown verbatim in the lobby.
const char* LanStartRefusal();
void LanCycleCostume(int delta);
void LanSetName(const char* name);
const char* LanGetName();
// `meat` is the MEAT RUSH setting byte (0 = off, 5/10/15/20 = target, 0xFF = unlimited),
// i.e. exactly what lives in the settings blob at 0x16A26B.
void LanHostSetRules(uint32_t timeLimit, uint8_t rounds, uint8_t difficulty, uint8_t meat);
// Arena ids 10 (BOXING) and 12 (HELL) are the two the RETAIL versus carousel excludes, and a
// LAN match on 12 diverged at frame 17,875 in the one run that tried it. Until that is
// understood they are not offered. See PROJECT_STATUS.md.
bool     LanArenaSelectable(int arena);
uint8_t  LanConfigArena();       // the lobby's current arena / mode / rules, on any peer
uint8_t  LanConfigMode();
uint8_t  LanConfigRounds();
uint8_t  LanConfigMeat();
uint32_t LanConfigTime();
uint32_t LanPingMs();
void LanHostSetArena(uint8_t arena);
void LanHostSetMode(uint8_t mode);      // 0 quick, 1 tournament
void LanHostToggleCpu(int slot);
// Remove the player in `slot` and free the seat. Host only, and never a seat this PC drives --
// unplug that controller instead.
void LanHostKick(int slot);
bool LanHostCanStart();
void LanHostStart();                     // -> LAN_STARTING -> GO -> armed
void LanLeave();

// True on the frame the launch should happen: the frontend must return the retail launch
// sentinel 0x16 from whatever screen it is on. Consumed by the caller.
bool LanConsumeLaunch();
void LanMatchEnded();                    // back to the lobby, disarm

// The config every peer applies at launch (see lan_match.cpp).
struct LanMatchConfig {
    uint32_t matchId, seed;
    uint32_t timeLimit;                  // -> 0x16A264
    uint8_t  players, inputDelay, arena, mode;
    uint8_t  teamMode;                   // screen 11 +0xA0 (0 = FFA)
    uint8_t  rounds, difficulty, tournFirstTo;
    uint8_t  meat;                       // MEAT RUSH: 0 off, 5/10/15/20 target, 0xFF unlimited
    uint8_t  slotKind[4];                // 0 open, 1 human, 2 cpu
    uint8_t  slotChar[4], slotCostume[4], slotTeam[4];
    char     slotName[4][16];
};
const LanMatchConfig* LanConfig();
uint32_t LanConfigCrc(const LanMatchConfig* c);

}  // namespace tj::hybrid
