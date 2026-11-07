// Savestate slots: save/load the whole game state (writable game sections + used arena pages).
#pragma once
#include <string>
#include <cstdint>

namespace ss {

constexpr int MAX_SLOTS = 10;

bool Init(const char* folder, bool restartStreams);   // restartStreams: re-open radio/cutscene audio after a load
bool CanOperate(std::string& why);        // in game, no menu, device OK, arena intact
bool Save(int slot, std::string& msg);
bool Load(int slot, std::string& msg);
bool Used(int slot);
std::string Describe(int slot);           // "slot 3: saved 42 s ago" / "slot 3: empty"
void DropAll();                           // device reset: every slot becomes unusable
void Tick();                              // per main-loop iteration: finishes restarting streamed audio after a load

// Lab: load every savestate the way one from another process is loaded (re-created graphics objects).
extern bool forceRecreate;

}
