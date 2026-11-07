// Savestate slots for GTA III / Vice City.
#pragma once
#include <string>
#include <cstdint>

namespace ss {

constexpr int MAX_SLOTS = 10;

bool Init(const char* folder);
void LateInit();                          // once the game's imports are usable (Miles functions)
bool CanOperate(std::string& why);
bool Save(int slot, std::string& msg);
bool Load(int slot, std::string& msg);
bool Used(int slot);
int PlayingChannels();   // Miles 3D channels playing now (lab check)
int SilentLoops();       // loops the audio manager has as playing that Miles does not play (lab check)
std::string StreamStates();   // each Miles stream: file, position / length in ms, status (lab check)
std::string Describe(int slot);
void DropAll();
void Tick();

// Lab: load every savestate the way one from another process is loaded (re-created graphics objects).
extern bool forceRecreate;

}
