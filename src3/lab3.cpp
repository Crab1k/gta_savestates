#include "lab3.h"
#include "../src/arena.h"
#include "../src/common.h"
#include "census3.h"
#include "d3d8.h"
#include "game3.h"
#include "state3.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace lab {
namespace {

char g_ini[MAX_PATH], g_dir[MAX_PATH], g_self[MAX_PATH];
bool g_labActive = false;   // a [Lab] section is set up (the status line goes to the log only then)
bool g_redirectUserFiles = false, g_runInBackground = false, g_skipIntro = false, g_autoNewGame = false, g_quit = false, g_unpause = false;

// ------------------------------------------------------------------ script commands
// One SCM command run through a throwaway CRunningScript. The instruction pointer is an offset into ScriptSpace, so
// the code goes into the last 256 bytes of the mission area (beyond the largest mission).
struct Script {
    uint8_t code[200];
    int n = 0;
    int32_t locals[16] = {0};
    Script& op(uint16_t id) { memcpy(code + n, &id, 2); n += 2; return *this; }
    Script& i32(int32_t v) { code[n++] = 0x01; memcpy(code + n, &v, 4); n += 4; return *this; }
    Script& f(float v) {
        code[n++] = 0x06;
        if (g3->fixedFloats) { int16_t x = (int16_t)(v * 16.0f); memcpy(code + n, &x, 2); n += 2; }
        else { memcpy(code + n, &v, 4); n += 4; }
        return *this;
    }
    Script& local(uint16_t idx) { code[n++] = 0x03; memcpy(code + n, &idx, 2); n += 2; return *this; }
    Script& label(const char* s) { char l[8] = {}; strncpy_s(l, s, 7); memcpy(code + n, l, 8); n += 8; return *this; }
    bool run() {
        const uint32_t at = g3->scriptScratch;
        memcpy((void*)(g3->scriptSpace + at), code, n);
        alignas(4) uint8_t rs[0x100] = {};
        memcpy(rs + 0x08, "sslab", 6);
        *(uint32_t*)(rs + g3->scriptIp) = at;
        memcpy(rs + 0x30, locals, sizeof(locals));
        int8_t r = ((int8_t(__fastcall*)(void*))g3->processOneCommand)(rs);
        memcpy(locals, rs + 0x30, sizeof(locals));
        return r != 0;
    }
};

// ------------------------------------------------------------------ state for the checks
bool PlayerPos(float* p) {
    uintptr_t ped = Mem<uintptr_t>(g3->players);
    if (!ped) return false;
    memcpy(p, (const void*)(ped + 0x34), 12);   // CPlaceable::m_matrix (inline, after the vtable): pos at +0x30
    return true;
}

std::string ScriptState() {
    char b[96];
    sprintf_s(b, "onmission=%d |", Mem<int32_t>(g3->scriptSpace + Mem<int32_t>(g3->onMissionFlag)));
    std::string s = b;
    int n = 0;
    for (uintptr_t sc = Mem<uintptr_t>(g3->activeScripts); sc && n < 120; sc = Mem<uintptr_t>(sc), n++) {
        char name[9] = {};
        memcpy(name, (const void*)(sc + 8), 8);
        sprintf_s(b, " %s@%X", name, Mem<uint32_t>(sc + g3->scriptIp));   // an offset into ScriptSpace
        s += b;
    }
    return s;
}

std::string LogState(const char* what) {
    std::string s = ScriptState();
    Log("TEST %s: %s", what, s.substr(0, 160).c_str());
    return s;
}

struct Mark { bool set = false; float pos[3]; uint32_t time; };
Mark g_marks[ss::MAX_SLOTS];
std::string g_scriptsAtSave[ss::MAX_SLOTS];
int g_pass = 0, g_fail = 0, g_shot = 0;

void Shot(const char* tag) {
    char p[MAX_PATH];
    sprintf_s(p, "%s\\shots", g_self);
    CreateDirectoryA(p, nullptr);
    sprintf_s(p, "%s\\shots\\%02d_%s.png", g_self, g_shot++, tag);
    d3d::Screenshot(p);
}

void SaveTo(int slot) {
    std::string msg;
    g_scriptsAtSave[slot] = ScriptState();
    bool ok = ss::Save(slot, msg);
    d3d::ShowText(msg.c_str(), 1500);
    if (!ok) { Log("TEST save %d FAILED: %s", slot + 1, msg.c_str()); g_fail++; return; }
    Mark& m = g_marks[slot];
    m.set = PlayerPos(m.pos);
    m.time = Mem<uint32_t>(g3->timerMs);
}

void LoadFrom(int slot, const char* tag) {
    std::string msg;
    float before[3] = {}, after[3] = {};
    PlayerPos(before);
    bool ok = ss::Load(slot, msg);
    PlayerPos(after);
    uint32_t t = Mem<uint32_t>(g3->timerMs);
    const Mark& m = g_marks[slot];
    bool same = m.set && memcmp(after, m.pos, 12) == 0 && t == m.time;
    bool scripts = ScriptState() == g_scriptsAtSave[slot];
    float dx = before[0] - m.pos[0], dy = before[1] - m.pos[1];
    bool pass = ok && same && scripts;
    (pass ? g_pass : g_fail)++;
    Log("TEST %-22s slot %d: %s | was %.0f m away, pos (%.2f %.2f %.2f) vs saved (%.2f %.2f %.2f), time %u vs %u, "
        "scripts %s | arena %u MB live / %u MB used, gpu objects %u", tag, slot + 1, pass ? "PASS" : "FAIL",
        std::sqrt(dx * dx + dy * dy), after[0], after[1], after[2], m.pos[0], m.pos[1], m.pos[2], t, m.time,
        scripts ? "match" : "DIFFER", (unsigned)(arena::LiveBytes() >> 20), (unsigned)(arena::UsedEnd() >> 20),
        (unsigned)d3d::TrackedCount());
    if (!ok) Log("TEST %s: %s", tag, msg.c_str());
    if (!scripts) Log("TEST %s scripts now: %s", tag, ScriptState().substr(0, 300).c_str());
    d3d::ShowText(msg.c_str(), 1500);
    Shot(tag);
}

// ------------------------------------------------------------------ scenario timeline
struct Step { DWORD delay; std::function<void()> fn; bool ready; };   // ready: also wait until saving is possible
std::vector<Step> g_steps;
size_t g_step = 0;
DWORD g_stepT = 0, g_readySince = 0;
bool g_wasReady = false;

void Then(DWORD delay, std::function<void()> fn) { g_steps.push_back({delay, std::move(fn), false}); }
void WhenReady(DWORD delay, std::function<void()> fn) { g_steps.push_back({delay, std::move(fn), true}); }

// New game: the intro mission is running from the start (cutscene, then play).
void BuildBasicScenario(int cycles) {
    Then(0, [] { LogState("start"); SaveTo(0); Shot("save1"); });
    Then(3000, [] { LoadFrom(0, "load1"); });
    Then(3000, [] { Shot("load1_plus3s"); LogState("load1+3s"); });
    for (int c = 0; c < cycles; c++) {
        int slot = 1 + c % 2;
        std::string t = "cycle" + std::to_string(c);
        Then(2000, [slot] { SaveTo(slot); });
        Then(4000, [slot, t] { LoadFrom(slot, t.c_str()); });
    }
    Then(2000, [] { LoadFrom(0, "load1_again"); });
    Then(3000, [] { Shot("again_plus3s"); LogState("again+3s"); });
}

// Savestates that outlive the game: phase 1 saves and quits; phase 2 (a new process, later in a new game) loads them
// from disk and checks them against what phase 1 recorded in lab_marks.txt.
std::string MarksPath() { return std::string(g_self) + "\\lab_marks.txt"; }

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

void BuildPersistScenario(int phase) {
    if (phase == 1) {
        Then(0, [] { LogState("phase 1"); SaveTo(0); Shot("p1_save1"); });
        Then(4000, [] { SaveTo(1); Shot("p1_save2"); });
        Then(500, [] { WriteMarks(); Log("TEST phase 1 saved, quitting"); });
        return;
    }
    Then(0, [] { ReadMarks(); LogState("phase 2"); });
    Then(12000, [] { Shot("p2_before_load"); LoadFrom(0, "p2_load1_from_disk"); });
    Then(3000, [] { Shot("p2_load1_plus3s"); LogState("load1+3s"); });
    Then(2000, [] { SaveTo(2); });
    Then(2000, [] { LoadFrom(2, "p2_load3_same_session"); });
    Then(2000, [] { LoadFrom(1, "p2_load2_from_disk"); });
    Then(3000, [] { Shot("p2_load2_plus3s"); LogState("load2+3s"); });
    Then(500, [] { LoadFrom(0, "p2_load1_again"); });
    Then(5000, [] { Shot("p2_end"); LogState("end"); });
}

// Load savefile `file` (1-based) the way the menu does: check it, then let WinMain restart the game into it. From
// the title menu the game starts, and restarts into the file before any script runs. In game the restart cannot
// drop a loaded cutscene (it resets the camera to a player it already deleted), so the load waits for none.
int g_startFile = 0, g_pendingFile = 0;
bool g_restartWhenPlaying = false;
DWORD g_pendingSince = 0;

// WinMain checks the restart flag at the top of its loop, before PeekMessageA (where this code runs): set from the
// title menu it would restart a game not yet initialised. So there the restart waits until the game is playing.
bool LoadSavefile(int file, bool fromTitle = false) {
    bool ok = ((uint8_t(__cdecl*)(int32_t))g3->checkSlotFn)(file - 1) != 0;
    Log("TEST savefile %d: %s (%s)", file, ok ? "loading" : "NOT USABLE", (const char*)g3->loadFileName);
    if (!ok) { g_fail++; return false; }
    Mem<uint8_t>(g3->loadSavedFlag) = 1;
    if (fromTitle) g_restartWhenPlaying = true;
    else Mem<uint8_t>(g3->wantRestart) = 1;
    return true;
}

void MenuLoad(int file) {
    g_pendingFile = file;
    g_pendingSince = GetTickCount();
}

void StartMission(int n) {
    Script s;
    s.op(0x0417).i32(n);   // START_MISSION: loads mission n into the mission area and starts it
    s.run();
    Log("TEST mission %d started", n);
}

// Across savefiles, missions and game restarts. Phase 1: savefile A, mission A, savestates 1 and 2, quit. Phase 2 (new
// process): savefile B, mission B, load 1 and 2 from disk, savestate 3, savefile C loaded in game, load 1 again, quit.
// Phase 3 (new process): new game, load 3 (from phase 2), 1 and, after savefile A, 2. 12 s of play after each load
// from disk (a player's game crashed 10 s after one).
void BuildCrossScenario(int phase, int fileA, int fileB, int fileC, int missionA, int missionB) {
    g_startFile = phase == 1 ? fileA : phase == 2 ? fileB : 0;
    if (phase == 1) {
        Then(0, [fileA, missionA] {
            Shot("p1_savefileA");
            LogState(("savefile " + std::to_string(fileA)).c_str());
            StartMission(missionA);
        });
        WhenReady(15000, [] { LogState("mission A"); SaveTo(0); Shot("p1_save1"); });
        Then(12000, [] { LogState("mission A later"); SaveTo(1); Shot("p1_save2"); });
        Then(500, [] { WriteMarks(); Log("TEST phase 1 saved, quitting"); });
        return;
    }
    if (phase == 2) {
        Then(0, [fileB, missionB] {
            ReadMarks();
            Shot("p2_savefileB");
            LogState(("savefile " + std::to_string(fileB)).c_str());
            StartMission(missionB);
        });
        WhenReady(15000, [] { LogState("mission B"); Shot("p2_before_load"); });
        Then(500, [] { LoadFrom(0, "p2_load1_from_disk"); });
        Then(12000, [] { Shot("p2_load1_plus12s"); LogState("load1+12s"); });
        Then(500, [] { SaveTo(2); });
        Then(3000, [] { LoadFrom(2, "p2_load3_same_session"); });
        Then(2000, [] { LoadFrom(1, "p2_load2_from_disk"); });
        Then(12000, [] { Shot("p2_load2_plus12s"); LogState("load2+12s"); });
        Then(500, [fileC] { MenuLoad(fileC); });
        WhenReady(6000, [fileC] { Shot("p2_savefileC"); LogState(("savefile " + std::to_string(fileC)).c_str()); });
        Then(500, [] { LoadFrom(0, "p2_load1_after_savefileC"); });
        Then(12000, [] { Shot("p2_load1_after_C_plus12s"); LogState("load1 after C +12s"); });
        Then(500, [] { WriteMarks(); Log("TEST phase 2 done, quitting"); });
        return;
    }
    Then(0, [] { ReadMarks(); LogState("new game"); });
    Then(500, [] { LoadFrom(2, "p3_load3_from_disk"); });
    Then(12000, [] { Shot("p3_load3_plus12s"); LogState("load3+12s"); });
    Then(500, [] { LoadFrom(0, "p3_load1_from_disk"); });
    Then(12000, [] { Shot("p3_load1_plus12s"); LogState("load1+12s"); });
    Then(500, [fileA] { MenuLoad(fileA); });
    WhenReady(6000, [] { Shot("p3_savefileA"); });
    Then(500, [] { LoadFrom(1, "p3_load2_after_savefileA"); });
    Then(12000, [] { Shot("p3_load2_plus12s"); LogState("load2+12s"); });
}

// Streaming churn between a save and a load: every model's RW objects are deleted and streamed again (what driving
// across the map does), then the savestate must look exactly as when it was saved.
void Teleport(float x, float y, float z) {
    Script s;
    s.op(0x0055).i32(0).f(x).f(y).f(z);   // SET_PLAYER_COORDINATES player 0
    s.run();
    Log("TEST player moved to %.0f %.0f %.0f", x, y, z);
}

void BuildChurnScenario() {
    Then(0, [] { LogState("start"); Log("TEST sound channels playing: %d, silent loops %d", ss::PlayingChannels(), ss::SilentLoops()); SaveTo(0); Shot("c_save1"); });
    static const float iii[3][3] = {{1000, -850, 40}, {200, -1000, 60}, {-700, 300, 80}};
    static const float vc[3][3] = {{227, -1265, -100}, {-850, -450, -100}, {-550, 1100, -100}};   // z -100: on the ground
    const float (*t)[3] = g3->id == Game3::VC_JP ? vc : iii;
    Then(2000, [t] { Teleport(t[0][0], t[0][1], t[0][2]); });
    Then(10000, [] { Log("TEST sound channels playing: %d, silent loops %d", ss::PlayingChannels(), ss::SilentLoops()); SaveTo(1); Shot("c_save2"); });
    Then(2000, [t] { Teleport(t[1][0], t[1][1], t[1][2]); });
    Then(10000, [] { ((void(__cdecl*)())g3->deleteAllRwObjects)(); Log("TEST all RW objects deleted"); });
    Then(2000, [t] { Teleport(t[2][0], t[2][1], t[2][2]); });
    Then(10000, [] { Shot("c_elsewhere"); d3d::Collect(); });
    Then(500, [] { LoadFrom(1, "c_load2"); });
    Then(2000, [] { Shot("c_load2_plus2s"); Log("TEST sound channels playing 2 s after the load: %d, silent loops %d", ss::PlayingChannels(), ss::SilentLoops()); });
    Then(500, [] { LoadFrom(1, "c_load2_again"); });
    Then(2000, [] { LoadFrom(0, "c_load1"); });
    Then(2000, [] { Shot("c_load1_plus2s"); Log("TEST sound channels playing 2 s after the load: %d, silent loops %d", ss::PlayingChannels(), ss::SilentLoops()); });
}

// The radio across loads: the player sits in a car (its radio plays) when savestates are made; phase 1 loads one in
// the same process, phase 2 (a new process) loads them from disk and plays on for a while after each.
void PlayerIntoCar(int model) {
    float p[3] = {};
    PlayerPos(p);
    Script s;
    s.op(0x0247).i32(model).run();   // REQUEST_MODEL
    Script l;
    l.op(0x038B).run();              // LOAD_ALL_MODELS_NOW
    Script c;
    c.op(0x00A5).i32(model).f(p[0] + 4.0f).f(p[1]).f(p[2]).local(0);   // CREATE_CAR
    c.run();
    Script w;
    w.op(0x0369).i32(0).local(0);    // WARP_PLAYER_INTO_CAR player 0
    memcpy(w.locals, c.locals, sizeof(w.locals));
    w.run();
    Log("TEST player put into a new car (model %d, handle %d)", model, c.locals[0]);
}

void BuildRadioScenario(int phase) {
    if (phase == 1) {
        Then(0, [] { LogState("savefile"); PlayerIntoCar(141); });
        Then(8000, [] { Shot("r_in_car"); SaveTo(0); });
        Then(3000, [] { LoadFrom(0, "r_load1_same_session"); });
        Then(4000, [] { Shot("r_load1_plus4s"); SaveTo(1); });
        Then(500, [] { WriteMarks(); Log("TEST phase 1 saved, quitting"); });
        return;
    }
    Then(0, [] { ReadMarks(); LogState("phase 2"); });
    Then(500, [] { LoadFrom(1, "r_load2_from_disk"); });
    Then(20000, [] { Shot("r_load2_plus20s"); LogState("load2+20s"); });
    Then(500, [] { LoadFrom(0, "r_load1_from_disk"); });
    Then(20000, [] { Shot("r_load1_plus20s"); LogState("load1+20s"); });
}

// A dialogue line loaded by a mission (LOAD_MISSION_AUDIO) waits until the script plays it. Savestate 1: the line
// waiting; savestate 2: the line played to its end. Each is loaded, the waiting one played again; the streams are
// logged every second (a load must leave the waiting line whole and the finished one finished).
void LineCommand(uint16_t op, const char* name = nullptr) {
    Script s;
    s.op(op);
    if (g3->id == Game3::VC_JP) s.i32(1);   // Vice City: mission audio slot 1; GTA III has one
    if (name) s.label(name);
    s.run();
}

void LogStreams(const std::string& when) { Log("TEST %s: streams %s", when.c_str(), ss::StreamStates().c_str()); }

void BuildMissionAudioScenario() {
    const char* line = g3->id == Game3::VC_JP ? "MONO_8" : "MF1_A";
    Then(0, [line] { LogState("start"); LineCommand(0x03CF, line); Log("TEST loading mission audio %s", line); });
    Then(3000, [] { LogStreams("line loaded"); SaveTo(0); });
    Then(500, [] { LineCommand(0x03D1); LogStreams("play"); });
    for (int i = 1; i <= 7; i++) Then(1000, [i] { LogStreams("played " + std::to_string(i) + " s"); });
    Then(500, [] { SaveTo(1); });
    Then(1000, [] { LoadFrom(0, "ma_load_waiting"); LogStreams("after the load (line waiting)"); });
    Then(2000, [] { LogStreams("waiting +2 s"); LineCommand(0x03D1); LogStreams("play after the load"); });
    for (int i = 1; i <= 7; i++) Then(1000, [i] { LogStreams("after the load, played " + std::to_string(i) + " s"); });
    Then(500, [] { LoadFrom(1, "ma_load_finished"); LogStreams("after the load (line finished)"); });
    Then(3000, [] { LogStreams("finished +3 s"); });
}

// Show the mission-passed message the way the scripts do, and find where the game keeps its text.
void FindWide(const char* ascii) {
    std::wstring w(ascii, ascii + strlen(ascii));
    size_t n = w.size() * 2;
    for (uintptr_t a = g3->dataBegin; a + n < g3->dataEnd; a += 2)
        if (!memcmp((void*)a, w.data(), n)) Log("TEST text \"%s\" found at %08X", ascii, (unsigned)a);
}

void BuildMissionPassedProbe() {
    Then(0, [] { Script s; s.op(0x01E3).label("M_PASS").i32(1000).i32(6000).i32(1); s.run(); Log("TEST mission passed shown"); });
    Then(1500, [] { FindWide("MISSION"); Shot("mission_passed"); });
    Then(3000, [] { Shot("mission_passed_later"); });
}

// ------------------------------------------------------------------ lab hooks
char* __cdecl Hook_UserDir() {
    char* dir = (char*)g3->userDirBuf;
    sprintf_s(dir, 256, "%s\\userfiles", g_dir);
    CreateDirectoryA(dir, nullptr);
    return dir;
}

uintptr_t g_setSaveDir = 0;

void __cdecl Hook_SetSaveDir(const char*) {
    char dir[MAX_PATH];
    sprintf_s(dir, "%s\\userfiles", g_dir);
    CreateDirectoryA(dir, nullptr);
    ((void(__cdecl*)(const char*))g_setSaveDir)(dir);
}

DWORD g_frontendSince = 0;

}  // namespace

void Init(const char* iniPath, const char* gameDir, const char* selfDir) {
    strcpy_s(g_ini, iniPath);
    strcpy_s(g_dir, gameDir);
    strcpy_s(g_self, selfDir);
    auto ini = [](const char* k, int d) { return (int)GetPrivateProfileIntA("Lab", k, d, g_ini); };
    g_redirectUserFiles = ini("RedirectUserFiles", 0) != 0;
    if (ini("Windowed", 0)) {
        d3d::forceWindowed = true;
        d3d::windowX = ini("WindowX", 40);
        d3d::windowY = ini("WindowY", 40);
    }
    g_runInBackground = ini("RunInBackground", 0) != 0;
    g_skipIntro = ini("SkipIntro", 0) != 0;
    g_autoNewGame = ini("AutoNewGame", 0) != 0;
    d3d::releaseCheck = ini("ReleaseCheck", 0) != 0;
    ss::forceRecreate = ini("ForceRecreate", 0) != 0;
    g_quit = ini("AutoTestQuit", 0) != 0;
    char sec[64] = {};
    g_labActive = GetPrivateProfileSectionA("Lab", sec, sizeof(sec), g_ini) > 0;
    int cycles = ini("AutoTest", 0);
    if (cycles) {
        int mode = ini("AutoTestMode", 1);
        if (mode == 2) BuildMissionPassedProbe();
        else if (mode == 3) Then(0, [] { census::Run("in game"); });
        else if (mode == 4) { g_startFile = ini("StartSavefile", 0); BuildPersistScenario(ini("PersistPhase", 1)); }
        else if (mode == 6) { g_startFile = ini("StartSavefile", 0); BuildChurnScenario(); }
        else if (mode == 7) { g_startFile = ini("StartSavefile", 0); BuildRadioScenario(ini("PersistPhase", 1)); }
        else if (mode == 8) { g_startFile = ini("StartSavefile", 0); BuildMissionAudioScenario(); }
        else if (mode == 5)
            BuildCrossScenario(ini("PersistPhase", 1), ini("SavefileA", 1), ini("SavefileB", 2), ini("SavefileC", 3),
                               ini("MissionA", 0), ini("MissionB", 0));
        else BuildBasicScenario(cycles);
        Log("lab: autotest, %u steps", (unsigned)g_steps.size());
    }
}

// Code patches wait until the game's code is final (Vice City JP: after its protector ran).
void LatePatch() {
    if (g_redirectUserFiles && g3->userDirFn) WriteRel(g3->userDirFn, 0xE9, (void*)&Hook_UserDir);
    if (g_redirectUserFiles && g3->saveDirCall && Mem<uint8_t>(g3->saveDirCall) == 0xE8) {
        g_setSaveDir = ReadCallTarget(g3->saveDirCall);
        WriteRel(g3->saveDirCall, 0xE8, (void*)&Hook_SetSaveDir);
    }
}

// Lab check: is the user-files redirect still in place (Vice City's protector rewrites some code after start-up)?
void CheckUserDirPatch(const char* when) {
    if (!g_redirectUserFiles || !g3->userDirFn) return;
    const uint8_t* p = (const uint8_t*)g3->userDirFn;
    Log("lab: user files redirect %s: %02X %02X %02X %02X %02X -> %s", when, p[0], p[1], p[2], p[3], p[4],
        p[0] == 0xE9 ? "in place" : "GONE");
}

void Tick() {
    if (!g3) return;
    static int checks = 0;
    if (checks < 1) { checks++; CheckUserDirPatch("in the main loop"); }
    if (g_runInBackground) Mem<int32_t>(g3->foregroundApp) = 1;
    int32_t& st = Mem<int32_t>(g3->gGameState);
    if (g_skipIntro && st >= 1 && st <= g3->gsIntroLast) st = g3->gsInitOnce;   // logo / intro movies -> init once
    DWORD now = GetTickCount();
    if (g_autoNewGame && st == g3->gsFrontend && Mem<uint8_t>(g3->menuActive)) {
        if (!g_frontendSince) g_frontendSince = now;
        if (now - g_frontendSince > 1500) {
            if (g_startFile) LoadSavefile(g_startFile, true);   // the game starts, then restarts into the file
            Mem<uint8_t>(g3->menuActive) = 0;   // title menu closed: the game starts a new game
            g_autoNewGame = false;
            g_unpause = true;
            Log("lab: %s from the title menu", g_startFile ? "savefile load" : "new game started");
        }
    }
    static DWORD lastBeat = 0;
    if (g_labActive && now - lastBeat > 2000) {
        lastBeat = now;
        Log("lab: state %d menu %d time %u frame %u fg %d save status %d pause %d/%d", st, Mem<uint8_t>(g3->menuActive),
            Mem<uint32_t>(g3->timerMs), Mem<uint32_t>(g3->timerFrame), Mem<int32_t>(g3->foregroundApp),
            Mem<int32_t>(g3->saveStatus),
            Mem<uint8_t>(g3->timerUserPause), Mem<uint8_t>(g3->timerCodePause));
    }
    // The menu's "new game" ends the user pause on its way out; once the player exists, do the same.
    if (g_unpause && st == g3->gsPlaying && Mem<void*>(g3->players)) {
        Mem<uint8_t>(g3->timerUserPause) = 0;
        g_unpause = false;
        Log("lab: user pause ended");
    }
    // Someone using the PC during a run takes the focus away, and Vice City then opens its pause menu, which no test
    // step does: log focus changes and close such a menu (a savefile loaded behind it left the game in the menu).
    static bool hadFocus = true;
    HWND hwnd = Mem<HWND>(g3->rwGlobals);
    bool focus = hwnd && GetForegroundWindow() == hwnd;
    if (g_labActive && hwnd && focus != hadFocus) Log("lab: game window %s the focus", focus ? "got" : "lost");
    hadFocus = focus;
    if (g_runInBackground && g_step < g_steps.size() && st == g3->gsPlaying && Mem<uint8_t>(g3->menuActive)) {
        Mem<uint8_t>(g3->menuActive) = 0;
        Mem<uint8_t>(g3->timerUserPause) = 0;
        Log("lab: pause menu opened during the test, closed");
    }
    if (g_restartWhenPlaying && st == g3->gsPlaying) {
        g_restartWhenPlaying = false;
        Mem<uint8_t>(g3->wantRestart) = 1;
    }
    if (g_pendingFile && st == g3->gsPlaying && !Mem<uint8_t>(g3->cutsceneLoaded)) {
        Log("lab: savefile %d after %u ms without a cutscene", g_pendingFile, (unsigned)(now - g_pendingSince));
        LoadSavefile(g_pendingFile);
        g_pendingFile = 0;
    } else if (g_pendingFile && now - g_pendingSince > 90000) {
        Log("TEST savefile %d: still a cutscene after 90 s, not loaded", g_pendingFile);
        g_pendingFile = 0;
        g_fail++;
    }
    if (g_step >= g_steps.size()) return;
    std::string why;
    bool ready = ss::CanOperate(why) && !g_pendingFile;
    if (ready && !g_wasReady) g_readySince = now;
    g_wasReady = ready;
    const Step& s = g_steps[g_step];
    if (g_step == 0) {
        if (!ready || now - g_readySince < 8000) return;
    } else if (now - g_stepT < s.delay || (s.ready && !ready)) {
        return;
    }
    s.fn();
    g_step++;
    g_stepT = GetTickCount();
    if (g_step == g_steps.size()) {
        Log("TEST done: %d pass, %d fail", g_pass, g_fail);
        if (g_quit) Mem<int32_t>(g3->rsPs - 4) = 1;   // RsGlobal.quit (just before ps)
    }
}

}  // namespace lab
