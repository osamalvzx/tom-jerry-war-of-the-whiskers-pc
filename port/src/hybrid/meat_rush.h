// MEAT RUSH -- a new game mode: no weapons, no KOs, collect the turkey leg.
#pragma once
#include <cstdint>

namespace tj::hybrid {

int  InstallMeatRush();

// Is the mode active for the match that is about to start / is running?  Set by the frontend
// (FIGHT SETTINGS row) and by the LAN lobby; TJ_MEAT=1 forces it on for scripted tests.
bool MeatRushActive();
void MeatRushSetMode(bool on);

// The mode cannot run in BOXING (arena 10): its descriptor registers ZERO category-1 AND
// zero category-0 item types, so there is no slot to repurpose and the item scheduler
// early-outs.  BOXING is already excluded from LAN and from the retail versus carousel.
bool MeatRushArenaOk(int arenaId);

// Zero all four fighters' meat counts.  Called from the match/round barrier that net_sync
// already owns (0x18AC2), because that is the one point both peers reach on the same frame
// and BEFORE mode 6 touches any simulation state.
void MeatRushResetScores(const char* why);

// The target meat count for a round; 0 = unlimited (the clock decides).
void MeatRushSetTarget(uint8_t target);

// meat_ui.cpp -- the FIGHT SETTINGS row that turns the mode on, and its strings.
int  InstallMeatUi();
void MeatRushApplySetting();
const char* MeatCustomText(uint16_t idx);

// meat_menu.cpp -- the MULTIPLAYER main-menu row and the submenu on dead screen 14.
int  InstallMeatMenu();
void MeatMenuBuild(uint32_t self);          // called from fe_menu's main-menu Build hook
const char* MeatMenuText(uint16_t idx);
uint32_t MeatMenuRow();                     // the MULTIPLAYER item, for the menu's A-press
void MeatMenuLaunch();                      // "settings confirmed -- go to the setup screen"

// MEAT RUSH gets its OWN pass through FIGHT SETTINGS (the MAX MEAT row only exists there);
// QUICK GAME and TOURNAMENT never see that row.
void MeatOpenFightSettings();
bool MeatFightSettingsOpen();

}  // namespace tj::hybrid
