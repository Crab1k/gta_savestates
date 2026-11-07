#include "lab.h"
#include "arena.h"
#include "census.h"
#include "common.h"
#include "d3d.h"
#include "game.h"
#include "state.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace lab {
namespace {

// Addresses used only by the lab harness.
struct LabAddrs {
    uintptr_t initUserDirectories, userDirPath, galleryDirPath, tracksDirPath;
    uintptr_t callUpdatePads;     // CPad::UpdatePads call at the top of CGame::Process
    uintptr_t pads;               // CPad[2]; NewState (CControllerState) at +0
    uintptr_t missionPackGame;
    uintptr_t rsGlobalQuit;
    uintptr_t processOneCommand;  // CRunningScript::ProcessOneCommand
    uintptr_t activeScripts;      // CRunningScript* (m_pNext +0, name +8, IP +0x14)
    uintptr_t scriptSpace;
    uintptr_t onAMissionFlag;     // offset of $ONMISSION in ScriptSpace
    uintptr_t cutsceneRunning;
    uintptr_t foregroundApp;      // bool: the game pauses its main loop while this is false
};
const LabAddrs k10US = {0x744FB0, 0xC92368, 0xC92268, 0xC92168, 0x53BEE6, 0xB73458, 0xB72910, 0xC17050,
                        0x469EB0, 0xA8B42C, 0xA49960, 0xA476AC, 0xB5F851, 0x8D621C};
const LabAddrs k101EU = {0x7457E0, 0xC94B28, 0xC94A28, 0xC94928, 0x53C386, 0xB75AD8, 0xB74F90, 0xC19810,
                         0x469F30, 0xA8DAAC, 0xA4BFE0, 0xA49D2C, 0xB61ED1, 0x8D88B8};
const LabAddrs* L = nullptr;

// CMenuManager offsets
constexpr uint32_t MENU_DontDrawFrontEnd = 0x32, MENU_ActivateNextFrame = 0x33, MENU_Active = 0x5C,
                   MENU_MainMenuSwitch = 0xE9, MENU_CurrentScreen = 0x15D, MENU_SelectedSlot = 0x15F,
                   MENU_CurrentlyLoading = 0x1B3C;
constexpr uint8_t SCREEN_LOAD_FIRST_SAVE = 13;

char g_ini[MAX_PATH], g_dir[MAX_PATH];
bool g_runInBackground = false, g_skipIntro = false;
int g_census = 0;   // run the census at this many saves

// Run one SCM command through a throwaway CRunningScript (outputs land in its locals).
struct Script {
    alignas(4) uint8_t code[160];
    int n = 0;
    int32_t locals[34] = {0};
    Script& op(uint16_t id) { memcpy(code + n, &id, 2); n += 2; return *this; }
    Script& i32(int32_t v) { code[n++] = 0x01; memcpy(code + n, &v, 4); n += 4; return *this; }
    Script& f32(float v) { code[n++] = 0x06; memcpy(code + n, &v, 4); n += 4; return *this; }
    Script& local(uint16_t idx) { code[n++] = 0x03; memcpy(code + n, &idx, 2); n += 2; return *this; }
    bool run() {
        alignas(4) uint8_t rs[0xE0];
        memset(rs, 0, sizeof(rs));          // CRunningScript::Init: everything zero except the two below
        rs[0xC9] = 0xFF;                    // m_ExternalType = -1
        rs[0xD3] = 1;                       // m_IsDeathArrestCheckEnabled
        memcpy(rs + 0x08, "sslab", 6);
        *(uint8_t**)(rs + 0x10) = code;
        *(uint8_t**)(rs + 0x14) = code;
        memcpy(rs + 0x3C, locals, sizeof(locals));
        ((int8_t(__thiscall*)(void*))L->processOneCommand)(rs);
        memcpy(locals, rs + 0x3C, sizeof(locals));
        return rs[0xC5] != 0;
    }
};

int PlayerChar() { Script s; s.op(0x01F5).i32(0).local(0); s.run(); return s.locals[0]; }
int PlayerCar() { Script s; s.op(0x03C0).i32(PlayerChar()).local(0); s.run(); return s.locals[0]; }
void LoadScene(float x, float y, float z) { Script s; s.op(0x03CB).f32(x).f32(y).f32(z); s.run(); }

// ------------------------------------------------------------------ pad override
volatile bool g_walk = false;
volatile int g_throttle = 0, g_steer = 0, g_tapCross = 0;
void(__cdecl* g_origUpdatePads)() = nullptr;

void __cdecl Hook_UpdatePads() {
    g_origUpdatePads();
    int16_t* ns = (int16_t*)L->pads;
    if (g_walk) ns[1] = -128;
    if (g_throttle) ns[0x20 / 2] = (int16_t)g_throttle;   // ButtonCross: accelerate
    if (g_steer) ns[0] = (int16_t)g_steer;               // LeftStickX
    if (g_tapCross > 0) { ns[0x20 / 2] = (g_tapCross & 1) ? 255 : 0; g_tapCross--; }   // press/release: skips cutscenes
}

// ------------------------------------------------------------------ checks
struct Mark { bool set = false; float pos[3]; uint32_t time; };
Mark g_marks[ss::MAX_SLOTS];
int g_pass = 0, g_fail = 0, g_shot = 0;
bool g_quit = false;

bool PlayerPos(float* p) {
    uintptr_t ped = Mem<uintptr_t>(g_game->players);
    if (!ped) return false;
    uintptr_t m = Mem<uintptr_t>(ped + 0x14);
    memcpy(p, m ? (const float*)(m + 0x30) : (const float*)(ped + 4), 12);
    return true;
}

// "onmission=1 cutscene=1 | name@ip name@ip ..." for the running scripts.
std::string ScriptState() {
    char b[96];
    sprintf_s(b, "onmission=%d cutscene=%d |", Mem<int32_t>(L->scriptSpace + Mem<int32_t>(L->onAMissionFlag)),
              Mem<uint8_t>(L->cutsceneRunning));
    std::string s = b;
    int n = 0;
    for (uintptr_t sc = Mem<uintptr_t>(L->activeScripts); sc && n < 120; sc = Mem<uintptr_t>(sc), n++) {
        char name[9] = {};
        memcpy(name, (const void*)(sc + 8), 8);
        sprintf_s(b, " %s@%X", name, (unsigned)(Mem<uintptr_t>(sc + 0x14) - L->scriptSpace));
        s += b;
    }
    return s;
}

// Streaming channel (radio / cutscene track) and static SFX channels.
std::string AudioState() {
    uintptr_t hw = g_game->audioHardware, st = hw + audio::HW_StreamThread;
    uintptr_t sc = Mem<uintptr_t>(hw + audio::HW_StreamingChannel);
    int busy = 0, playing = 0;
    uint16_t n = Mem<uint16_t>(hw + audio::HW_NumChannels);
    for (uint16_t i = 0; i < n && i < 256; i++) {
        uintptr_t ch = Mem<uintptr_t>(hw + audio::HW_Channels + 4 * i);
        if (!ch || ch == sc || !Mem<void*>(ch + audio::CH_DSBuffer)) continue;
        busy++;
        if (Mem<uint32_t>(ch + audio::CH_BufferStatus) & 1) playing++;
    }
    char b[200];
    sprintf_s(b, "stream state=%d decoder=%s track=%d playing=%d t=%d ms | sfx channels with buffer %d, playing %d",
              sc ? Mem<int32_t>(sc + audio::SC_State) : 0, sc && Mem<void*>(sc + audio::SC_Decoder) ? "yes" : "no",
              Mem<int32_t>(st + 0x30), Mem<int32_t>(st + 0x34), Mem<int32_t>(st + 0x28), busy, playing);
    return b;
}

std::string LogState(const char* what) {
    std::string s = ScriptState();
    Log("TEST %s: %s", what, s.substr(0, s.find('|') + 60).c_str());
    Log("TEST %s audio: %s", what, AudioState().c_str());
    return s;
}

void Shot(const char* tag) {
    char p[MAX_PATH];
    sprintf_s(p, "%s\\shots", g_dir);
    CreateDirectoryA(p, nullptr);
    sprintf_s(p, "%s\\shots\\%02d_%s.png", g_dir, g_shot++, tag);
    d3d::Screenshot(p);
}

std::string g_scriptsAtSave[ss::MAX_SLOTS];

void SaveTo(int slot) {
    std::string msg;
    g_scriptsAtSave[slot] = ScriptState();
    bool ok = ss::Save(slot, msg);
    d3d::ShowText(msg.c_str(), 1500);
    if (!ok) { Log("TEST save %d FAILED: %s", slot + 1, msg.c_str()); g_fail++; return; }
    if (g_census > 0) { g_census--; census::Run(("save to slot " + std::to_string(slot + 1)).c_str()); }
    Mark& m = g_marks[slot];
    m.set = PlayerPos(m.pos);
    m.time = Mem<uint32_t>(g_game->timerTimeMs);
}

// Load and check: player position, game clock and the running-script list must equal the save-time values.
void LoadFrom(int slot, const char* tag) {
    std::string msg;
    float before[3] = {}, after[3] = {};
    PlayerPos(before);
    bool ok = ss::Load(slot, msg);
    PlayerPos(after);
    uint32_t t = Mem<uint32_t>(g_game->timerTimeMs);
    const Mark& m = g_marks[slot];
    bool same = m.set && memcmp(after, m.pos, 12) == 0 && t == m.time;
    bool scripts = ScriptState() == g_scriptsAtSave[slot];
    float dx = before[0] - m.pos[0], dy = before[1] - m.pos[1];
    bool pass = ok && same && scripts;
    (pass ? g_pass : g_fail)++;
    Log("TEST %-22s slot %d: %s | was %.0f m away, pos (%.2f %.2f %.2f) vs saved (%.2f %.2f %.2f), time %u vs %u, "
        "scripts %s | arena %u MB live / %u MB used, gpu objects %u",
        tag, slot + 1, pass ? "PASS" : "FAIL", std::sqrt(dx * dx + dy * dy), after[0], after[1], after[2],
        m.pos[0], m.pos[1], m.pos[2], t, m.time, scripts ? "match" : "DIFFER", (unsigned)(arena::LiveBytes() >> 20),
        (unsigned)(arena::UsedEnd() >> 20), (unsigned)d3d::TrackedCount());
    if (!ok) Log("TEST %s: %s", tag, msg.c_str());
    d3d::ShowText(msg.c_str(), 1500);
    Shot(tag);
}

void LaunchMission(int index) {
    Script s; s.op(0x0417).i32(index); s.run();     // LOAD_AND_LAUNCH_MISSION_INTERNAL
    Log("TEST mission %d launched", index);
}

void Teleport(float x, float y, float z) {
    LoadScene(x, y, z);
    int car = PlayerCar();
    if (car > 0) {
        Script s; s.op(0x00AB).i32(car).f32(x).f32(y).f32(z); s.run();     // SET_CAR_COORDINATES
    } else {
        Script s; s.op(0x00A1).i32(PlayerChar()).f32(x).f32(y).f32(z); s.run();   // SET_CHAR_COORDINATES
    }
}

// Load a normal savefile the way the pause menu does: open the menu, then drive its load screen.
// ProcessFileActions validates the slot and starts the restart (ShutDownForRestart + InitialiseWhenRestarting).
int g_menuLoadSlot = 0, g_menuLoadPhase = 0;
DWORD g_menuLoadT = 0;
void MenuLoadTick() {
    uint8_t* menu = (uint8_t*)g_game->menuManager;
    DWORD now = GetTickCount();
    if (g_menuLoadPhase == 1) {
        menu[MENU_ActivateNextFrame] = 1;
        g_menuLoadPhase = 2;
        g_menuLoadT = now;
    } else if (g_menuLoadPhase == 2 && menu[MENU_Active] && now - g_menuLoadT > 1000) {
        menu[MENU_MainMenuSwitch] = 0;
        menu[MENU_SelectedSlot] = (uint8_t)(g_menuLoadSlot - 1);
        menu[MENU_CurrentScreen] = SCREEN_LOAD_FIRST_SAVE;
        menu[MENU_CurrentlyLoading] = 1;
        Mem<uint8_t>(L->missionPackGame) = 0;
        Log("TEST loading savefile %d through the pause menu", g_menuLoadSlot);
        g_menuLoadPhase = 0;
    }
}

// ------------------------------------------------------------------ scenario timeline
struct Step {
    DWORD delay;     // ms after the previous step (or after the game became playable, when `ready`)
    bool ready;      // wait until a savestate could be made (in game, no menu) for `delay` ms
    std::function<void()> fn;
};
std::vector<Step> g_steps;
size_t g_step = 0;
DWORD g_stepT = 0, g_readySince = 0;
bool g_wasReady = false;

void Then(DWORD delay, std::function<void()> fn) { g_steps.push_back({delay, false, std::move(fn)}); }
void WhenReady(DWORD delay, std::function<void()> fn) { g_steps.push_back({delay, true, std::move(fn)}); }

void BuildWalkScenario(int cycles) {
    for (int c = 0; c < cycles; c++) {
        int slot = c % 3;
        std::string t = "walk" + std::to_string(c);
        Then(c ? 3000 : 0, [slot] { SaveTo(slot); g_walk = true; });
        Then(2500, [] { g_walk = false; });
        Then(500, [slot, t] { LoadFrom(slot, t.c_str()); });
    }
}

// Leave CJ's house, drive a car down the LS airport runway, save, jump across the map, load back, ...
void BuildDriveScenario(int cycles) {
    Then(0, [] {
        int ped = PlayerChar();
        Script i0; i0.op(0x04BB).i32(0); i0.run();                     // SELECT_INTERIOR 0
        Script i1; i1.op(0x0860).i32(ped).i32(0); i1.run();           // SET_CHAR_AREA_VISIBLE
        Script tm; tm.op(0x00C0).i32(12).i32(0); tm.run();             // SET_TIME_OF_DAY 12:00
        LoadScene(1477.f, -2493.f, 13.8f);
        Script p; p.op(0x00A1).i32(ped).f32(1477.f).f32(-2485.f).f32(13.8f); p.run();
    });
    Then(2000, [] {
        Script r; r.op(0x0247).i32(560); r.run();                      // REQUEST_MODEL sultan
        Script l; l.op(0x038B); l.run();                               // LOAD_ALL_MODELS_NOW
        Script c; c.op(0x00A5).i32(560).f32(1477.f).f32(-2493.f).f32(13.8f).local(0); c.run();
        int car = c.locals[0];
        Script h; h.op(0x0175).i32(car).f32(270.f); h.run();           // SET_CAR_HEADING
        Script w; w.op(0x036A).i32(PlayerChar()).i32(car); w.run();    // WARP_CHAR_INTO_CAR
        Script m; m.op(0x0249).i32(560); m.run();
        Log("TEST car %d spawned", car);
    });
    Then(1500, [] { g_throttle = 255; });
    Then(5000, [] { SaveTo(0); Shot("save1_runway"); });
    Then(4000, [] { Teleport(-2000.f, 290.f, 35.f); });             // San Fierro: everything streams out
    Then(6000, [] { Shot("sf_before_load"); });
    Then(500, [] { LoadFrom(0, "load1_from_sf"); });
    Then(2500, [] { Shot("load1_plus2s"); });
    Then(4000, [] { SaveTo(1); g_steer = -60; });
    Then(6000, [] { g_steer = 0; LoadFrom(1, "load2"); });
    Then(3000, [] { LoadFrom(0, "load1_older"); });
    const float spots[][3] = {{2050.f, 1500.f, 11.f}, {-500.f, -200.f, 60.f}, {-2000.f, 290.f, 35.f},
                              {2500.f, -1670.f, 13.5f}, {400.f, 2500.f, 17.f}};
    for (int c = 0; c < cycles; c++) {
        const float* s = spots[c % 5];
        float x = s[0], y = s[1], z = s[2];
        int slot = c % 2;
        std::string t = "cycle" + std::to_string(c);
        Then(3000, [x, y, z] { Teleport(x, y, z); });
        Then(4000, [slot, t] { LoadFrom(slot, t.c_str()); });
        if (c % 3 == 2) Then(2000, [slot] { SaveTo(slot); });
    }
    Then(500, [] { g_throttle = 0; });
}

// Launch a story mission, save during its cutscene and during play, load back.
void BuildMissionScenario(int mission) {
    Then(0, [mission] { LogState("before launch"); LaunchMission(mission); });
    Then(6000, [] { LogState("at save 1 (cutscene)"); SaveTo(0); Shot("m_save_cutscene"); });
    Then(5000, [] { LogState("before load 1"); });
    Then(300, [] { LoadFrom(0, "m_load_cutscene"); });
    Then(3000, [] { Shot("m_load1_plus3s"); LogState("load1+3s"); });
    Then(500, [] { g_tapCross = 6; Log("TEST skipping cutscene"); });
    Then(10000, [] { LogState("at save 2 (play)"); SaveTo(1); Shot("m_save_play"); g_walk = true; });
    Then(5000, [] { g_walk = false; Shot("m_before_load2"); LogState("before load 2"); });
    Then(300, [] { LoadFrom(1, "m_load_play"); });
    Then(5000, [] { Shot("m_load2_plus5s"); LogState("load2+5s"); });
    Then(2000, [] { LoadFrom(0, "m_load_cutscene_again"); });
    Then(6000, [] { Shot("m_again_plus6s"); LogState("again+6s"); });
    Then(500, [] { g_tapCross = 6; });
    Then(8000, [] { Shot("m_again_skipped"); LogState("again skipped"); });
}

// Savestate in the middle of mission A, then load a different savefile and start mission B, then load the
// savestate; repeat across another savefile.
void BuildCrossLoadScenario(int missionA, int missionB, int fileB, int fileC) {
    Then(0, [missionA] { LogState("savefile A"); LaunchMission(missionA); });
    Then(6000, [] { g_tapCross = 6; Log("TEST skipping cutscene A"); });
    Then(10000, [] { LogState("mission A play"); SaveTo(0); Shot("x_saveA"); });
    Then(500, [fileB] { g_menuLoadSlot = fileB; g_menuLoadPhase = 1; });
    WhenReady(5000, [fileB] { Shot("x_savefileB"); LogState(("savefile " + std::to_string(fileB) + " loaded").c_str()); });
    Then(500, [missionB] { LaunchMission(missionB); });
    Then(6000, [] { Shot("x_missionB_cutscene"); LogState("mission B cutscene"); g_tapCross = 6; });
    Then(9000, [] { Shot("x_missionB_play"); LogState("mission B play"); SaveTo(1); });
    Then(3000, [] { LoadFrom(0, "x_loadA_from_missionB"); });
    Then(5000, [] { Shot("x_A_plus5s"); LogState("A+5s"); g_walk = true; });
    Then(3000, [] { g_walk = false; LoadFrom(1, "x_loadB_from_A"); });
    Then(5000, [] { Shot("x_B_plus5s"); LogState("B+5s"); });
    Then(500, [fileC] { g_menuLoadSlot = fileC; g_menuLoadPhase = 1; });
    WhenReady(5000, [fileC] { Shot("x_savefileC"); LogState(("savefile " + std::to_string(fileC) + " loaded").c_str()); });
    Then(500, [] { LoadFrom(0, "x_loadA_from_fileC"); });
    Then(5000, [] { Shot("x_A_again_plus5s"); LogState("A again+5s"); });
    Then(500, [] { LoadFrom(1, "x_loadB_again"); });
    Then(5000, [] { Shot("x_B_again_plus5s"); LogState("B again+5s"); });
}

// Savestates that outlive the game: phase 1 saves in a mission and quits; phase 2 (a new process, another savefile,
// another mission) loads them from disk and checks them against what phase 1 recorded in lab_marks.txt.
std::string MarksPath() { return std::string(g_dir) + "\\lab_marks.txt"; }

void WriteMarks() {
    FILE* f = nullptr;
    if (fopen_s(&f, MarksPath().c_str(), "w") || !f) return;
    for (int i = 0; i < ss::MAX_SLOTS; i++)
        if (g_marks[i].set)
            fprintf(f, "%d %.9g %.9g %.9g %u %s\n", i, g_marks[i].pos[0], g_marks[i].pos[1], g_marks[i].pos[2],
                    g_marks[i].time, g_scriptsAtSave[i].c_str());
    fclose(f);
}

void ReadMarks() {
    FILE* f = nullptr;
    if (fopen_s(&f, MarksPath().c_str(), "r") || !f) { Log("TEST no marks from phase 1"); return; }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        int slot = -1, n = 0;
        Mark m;
        if (sscanf_s(line, "%d %f %f %f %u %n", &slot, &m.pos[0], &m.pos[1], &m.pos[2], &m.time, &n) < 5 ||
            slot < 0 || slot >= ss::MAX_SLOTS) continue;
        m.set = true;
        g_marks[slot] = m;
        std::string sc = line + n;
        while (!sc.empty() && (sc.back() == '\n' || sc.back() == '\r')) sc.pop_back();
        g_scriptsAtSave[slot] = sc;
    }
    fclose(f);
}

void BuildPersistScenario(int phase, int missionA, int missionB, int fileC) {
    if (phase == 1) {
        Then(0, [missionA] { LogState("savefile A"); LaunchMission(missionA); });
        Then(6000, [] { g_tapCross = 6; Log("TEST skipping cutscene A"); });
        Then(10000, [] { LogState("mission A play"); SaveTo(0); Shot("p1_save1"); g_walk = true; });
        Then(3000, [] { g_walk = false; SaveTo(1); Shot("p1_save2"); });
        Then(500, [] { WriteMarks(); Log("TEST phase 1 saved, quitting"); });
        return;
    }
    Then(0, [missionB] { ReadMarks(); LogState("phase 2 savefile"); LaunchMission(missionB); });
    Then(6000, [] { g_tapCross = 6; Log("TEST skipping cutscene B"); });
    Then(9000, [] { LogState("mission B play"); Shot("p2_before_load"); });
    // 12 s of play after each load from disk (a player's Vice City crashed 10 s after one)
    Then(500, [] { LoadFrom(0, "p2_load1_from_disk"); });
    Then(12000, [] { Shot("p2_load1_plus12s"); LogState("load1+12s"); g_walk = true; });
    Then(3000, [] { g_walk = false; SaveTo(2); });
    Then(2000, [] { LoadFrom(2, "p2_load3_same_session"); });
    Then(2000, [] { LoadFrom(1, "p2_load2_from_disk"); });
    Then(12000, [] { Shot("p2_load2_plus12s"); LogState("load2+12s"); });
    Then(500, [] { LoadFrom(0, "p2_load1_again"); });
    Then(12000, [fileC] { LogState("load1 again+12s"); g_menuLoadSlot = fileC; g_menuLoadPhase = 1; });
    WhenReady(5000, [fileC] { Shot("p2_savefileC"); LogState(("savefile " + std::to_string(fileC) + " loaded").c_str()); });
    Then(500, [] { LoadFrom(1, "p2_load2_from_fileC"); });
    Then(12000, [] { Shot("p2_load2_fileC_plus12s"); LogState("after fileC load +12s"); });
}

}  // namespace

static char* __cdecl Hook_InitUserDirectories() {
    char* dir = (char*)L->userDirPath;
    if (dir[0]) return dir;
    sprintf_s(dir, 256, "%s\\userfiles", g_dir);
    CreateDirectoryA(dir, nullptr);
    sprintf_s((char*)L->galleryDirPath, 256, "%s\\Gallery", dir);
    sprintf_s((char*)L->tracksDirPath, 256, "%s\\User Tracks", dir);
    CreateDirectoryA((char*)L->galleryDirPath, nullptr);
    CreateDirectoryA((char*)L->tracksDirPath, nullptr);
    return dir;
}

static int g_autoLoadSlot = 0;
static DWORD WINAPI AutoLoadThread(LPVOID) {
    uint8_t* menu = (uint8_t*)g_game->menuManager;
    for (int i = 0; i < 1200; i++) {
        Sleep(100);
        if (Mem<int32_t>(g_game->gGameState) == 7 && menu[MENU_Active]) {
            Sleep(1500);
            menu[MENU_DontDrawFrontEnd] = 0;
            menu[MENU_SelectedSlot] = (uint8_t)(g_autoLoadSlot - 1);
            Mem<uint8_t>(L->missionPackGame) = 0;
            menu[MENU_CurrentScreen] = SCREEN_LOAD_FIRST_SAVE;
            menu[MENU_CurrentlyLoading] = 1;
            Log("lab: auto-load slot %d requested", g_autoLoadSlot);
            return 0;
        }
    }
    return 0;
}

void Init(const char* iniPath, const char* gameDir) {
    L = g_game->version == GameVersion::V10US ? &k10US : g_game->version == GameVersion::V101EU ? &k101EU : nullptr;
    if (!L) return;
    strcpy_s(g_ini, iniPath);
    strcpy_s(g_dir, gameDir);
    auto ini = [](const char* k, int d) { return (int)GetPrivateProfileIntA("Lab", k, d, g_ini); };
    if (ini("RedirectUserFiles", 0)) WriteRel(L->initUserDirectories, 0xE9, (void*)&Hook_InitUserDirectories);
    if (ini("Windowed", 0)) {
        d3d::forceWindowed = true;
        d3d::windowX = ini("WindowX", 40);
        d3d::windowY = ini("WindowY", 40);
    }
    g_runInBackground = ini("RunInBackground", 0) != 0;
    g_skipIntro = ini("SkipIntro", 0) != 0;
    d3d::releaseCheck = ini("ReleaseCheck", 0) != 0;
    g_census = ini("Census", 0);
    ss::forceRecreate = ini("ForceRecreate", 0) != 0;
    g_autoLoadSlot = ini("AutoLoadSlot", 0);
    if (g_autoLoadSlot >= 1 && g_autoLoadSlot <= 8) CloseHandle(CreateThread(nullptr, 0, AutoLoadThread, nullptr, 0, nullptr));
    int cycles = ini("AutoTest", 0);
    int mode = ini("AutoTestMode", 1);
    g_quit = ini("AutoTestQuit", 0) != 0;
    if (cycles) {
        g_origUpdatePads = (void(__cdecl*)())ReadCallTarget(L->callUpdatePads);
        WriteRel(L->callUpdatePads, 0xE8, (void*)&Hook_UpdatePads);
        if (mode == 5) BuildPersistScenario(ini("PersistPhase", 1), ini("MissionA", 16), ini("MissionB", 23), ini("SavefileC", 4));
        else if (mode == 4) BuildCrossLoadScenario(ini("MissionA", 16), ini("MissionB", 23), ini("SavefileB", 1), ini("SavefileC", 4));
        else if (mode == 3) BuildMissionScenario(ini("AutoTestMission", 16));
        else if (mode == 2) BuildDriveScenario(cycles);
        else BuildWalkScenario(cycles);
        Log("lab: autotest mode %d, %u steps", mode, (unsigned)g_steps.size());
    }
}

void Tick() {
    if (!L) return;
    if (g_runInBackground) Mem<uint8_t>(L->foregroundApp) = 1;   // keep running while another window has focus
    if (g_skipIntro) {
        // logo / title movie states -> frontend loading (what a key press during the movies does)
        int32_t& st = Mem<int32_t>(g_game->gGameState);
        if (st >= 1 && st <= 4) st = 5;
    }
    MenuLoadTick();
    if (g_step >= g_steps.size()) return;
    DWORD now = GetTickCount();
    std::string why;
    bool ready = ss::CanOperate(why);
    if (ready && !g_wasReady) g_readySince = now;
    g_wasReady = ready;
    const Step& s = g_steps[g_step];
    if (g_step == 0) {
        if (!ready || now - g_readySince < 5000) return;
    } else if (s.ready) {
        // needs a fresh stretch of readiness that began after the previous step (e.g. after a savefile load)
        if (!ready || g_readySince <= g_stepT || now - g_readySince < s.delay) return;
    } else if (now - g_stepT < s.delay) {
        return;
    }
    s.fn();
    g_step++;
    g_stepT = GetTickCount();
    if (g_step == g_steps.size()) {
        g_throttle = 0; g_steer = 0; g_walk = false;
        Log("TEST done: %d pass, %d fail", g_pass, g_fail);
        if (g_quit) Mem<uint8_t>(L->rsGlobalQuit) = 1;
    }
}

}  // namespace lab
