// Lab-only helpers for automated in-game testing (1.0 US): windowed mode, auto-load of a save slot,
// user-files redirect, and scripted savestate scenarios driven through the player's pad.
#pragma once

namespace lab {
void Init(const char* iniPath, const char* gameDir);
void Tick();
}
