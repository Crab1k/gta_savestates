// Lab-only helpers for automated in-game testing of GTA III / Vice City: windowed mode, user-files redirect, new game
// from the title menu, and scripted savestate scenarios with checks.
#pragma once

namespace lab {
void Init(const char* iniPath, const char* gameDir, const char* selfDir);
void LatePatch();   // once the game's code is final
void Tick();
}
