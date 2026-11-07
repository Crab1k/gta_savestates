// Game-version-specific addresses: GTA III 1.1 and GTA Vice City JP (Capcom release, protected exe).
// Found in the analysed exes (GTA III 1.1 file; Vice City JP from a memory dump after its protector ran).
#pragma once
#include "../src/common.h"
#include <string>

enum class Game3 { Unknown, III_11, VC_JP };

struct Range3 { uintptr_t addr; uint32_t size; };

struct Addrs3 {
    Game3 id;
    const char* name;
    bool lateImports;                 // the game's import table is filled by a protector after we load

    // Writable game sections captured in a savestate: [dataBegin, dataEnd) (import tables inside are kept live)
    uintptr_t dataBegin, dataEnd;

    uintptr_t callPeekMessage;        // WinMain's "call [PeekMessageA]" (the main loop)
    uintptr_t callGlobalAlloc, callGlobalFree;   // the runtime's "call [GlobalAlloc]" / "call [GlobalFree]" (its heap)
    bool      peekViaImport;          // hook PeekMessageA in the import tables (by return address), not at the call
    uintptr_t gGameState;             // int32, 9 = playing
    uintptr_t menuActive;             // bool FrontEndMenuManager.m_bMenuActive
    uintptr_t players;                // CPlayerInfo CWorld::Players[]; CPed* at +0
    uintptr_t rsPs;                   // RsGlobal.ps: PsGlobal* (window, DirectInput)
    uint32_t  psSize;
    uintptr_t timerMs;                // uint32 CTimer::m_snTimeInMilliseconds
    uintptr_t timerFrame;             // uint32 CTimer::m_FrameCounter
    uintptr_t timerUserPause, timerCodePause;   // bool CTimer::m_UserPause, m_CodePause
    Range3    timerOs[2];             // CTimer values that hold OS time

    // Metrowerks runtime: per-thread data in TLS, all blocks also in a linked list
    uintptr_t tlsIndex;
    uintptr_t threadDataList;
    uint32_t  threadDataSize;
    uintptr_t mallocFn, freeFn;       // the runtime's malloc / free (cdecl)

    // CdStream
    Range3    cdStreamGlobals;        // image names .. flags (handles, thread, semaphore, LocalAlloc'd arrays)
    uintptr_t cdStreams;              // CdReadInfo* (0x30 each)
    uintptr_t cdStreamCount;
    uintptr_t cdStreamQueue;          // {int* items; int head; int tail; int size}
    uintptr_t cdStreamThreadId;

    // RenderWare D3D8
    uintptr_t rwDevice;               // IDirect3DDevice8*
    uintptr_t vbReuseJz;              // dynamic VB pool: "same size, reuse" branch (2-byte jz, NOPed)
    uint16_t  vbReuseJzBytes;
    uintptr_t winMainFreeArgs;        // call _free(argv) at the end of WinMain (NOPed)
    uintptr_t texCacheJne;            // RW D3D8 texture creation: "not square, skip the texture cache" (made a jmp)
    uintptr_t texListCount;           // "mov esi, [count]" of a second cache of released textures (made xor esi, esi)
    uintptr_t passedScaleCall;        // CHud: call CFont::SetScale before the "mission passed" big message
    uintptr_t fontSetScale;           // void CFont::SetScale(float x, float y) (cdecl)

    // Miles
    uintptr_t milesDriver;            // HDIGDRIVER
    uintptr_t milesStream;            // HSTREAM of the streamed track (radio, cutscene)
    Range3    milesBuffers[2];        // Miles-allocated buffers {pointer var, size var|size; 0 = static, not saved}

    // Scripts (lab and checks)
    uintptr_t scriptSpace, activeScripts, onMissionFlag;
    uint32_t  scriptIp;               // offset of the instruction pointer in CRunningScript (an offset into ScriptSpace)
    uintptr_t processOneCommand;      // int8 __fastcall CRunningScript::ProcessOneCommand(this)
    uint32_t  scriptSize;             // sizeof(CRunningScript); locals at +0x30
    bool      fixedFloats;            // script floats are int16 * 16 (III) instead of float32 (VC)

    // Lab
    uintptr_t userDirFn, userDirBuf;  // char* psGetUserFilesFolder(), its static buffer
    uintptr_t saveDirCall;            // call C_PcSave::SetSaveDirectory(dir) with a second copy of that folder (VC)
    uintptr_t foregroundApp;          // the main loop idles while this is 0

    // Values that belong to the process (kept live when a savestate of another process is loaded)
    uintptr_t rwGlobals;              // RenderWare D3D8: HWND, IDirect3DDevice8*, render target, depth surface, IDirect3D8*
    Range3    rwPresent;              // the D3DPRESENT_PARAMETERS the device was created with
    uint32_t  streamCount;            // HSTREAMs at milesStream
    Range3    milesSamples;           // H3DSAMPLE array (3D channels)
    uintptr_t milesSample2d;          // HSAMPLE (2D channel)
    uintptr_t milesProvider;          // HPROVIDER of the open 3D provider
    // cAudioManager's channels (tSound m_asActiveSamples[]): entity index at +0 (-5 = none), sample at +8
    uintptr_t audioActive, audioActiveCount;   // the array; uint8 number of channels
    uint32_t  audioActiveStride, noSample;
    // Restarting a channel mid-sound the way cAudioManager::ProcessActiveQueues starts one (cSampleManager methods,
    // thiscall on sampleManager; translate is cAudioManager::TranslateEntity on audioManager).
    struct {
        uintptr_t sampleManager, audioManager;
        uintptr_t init, frequency, emittingVolume, loopPoints, loopCount, reverb, position, distances, start, translate;
        uintptr_t minDistFactor, pos2d, panStep;   // float constants
        uint32_t masterVolume, masterFlags;         // VC: uint8 effects volume and two flags in cAudioManager (0: none)
    } sfx;
    uintptr_t sfxFile;                // FILE* of the sound effects file, open while the game runs (check of mslHandles)
    uintptr_t mslHandles;             // the runtime's file handle table: 256 pointers to {HANDLE os; bool text; ...}

    // gGameState values
    int32_t   gsIntroLast;            // the logo / intro movie states are 1..gsIntroLast
    int32_t   gsInitOnce, gsFrontend, gsPlaying;

    // Lab: one-off script commands, loading a savefile the way the menu does
    uint32_t  scriptScratch;          // offset in ScriptSpace for them: the end of the mission area
    uintptr_t checkSlotFn;            // bool CheckSlotDataValid(int slot): sets the load file name, frees streamed models
    uintptr_t loadSavedFlag;          // bool: the restart loads that file (b_FoundRecentSavedGameWantToLoad)
    uintptr_t wantRestart;            // bool FrontEndMenuManager.m_bWantToRestart: WinMain restarts the game
    uintptr_t cutsceneLoaded;         // bool CCutsceneMgr::ms_loaded (the restart cannot drop a loaded cutscene)
    uintptr_t saveStatus;             // int PcSaveHelper.nErrorCode (4 open, 5 read, 6 close, 7 invalid)
    uintptr_t loadFileName;           // char[]: the file GenericLoad reads
    uintptr_t deleteAllRwObjects;     // void CStreaming::DeleteAllRwObjects(): every model's RW objects (re-streamed)

    // bool CCutsceneMgr::ms_running, when the window procedure skips a running cutscene on losing focus (VC; 0: no)
    uintptr_t cutsceneRunning;

    // Addresses of the 7 ped comment slots in the ped speech buffer (milesBuffers[1]); 0 = not yet loaded
    uintptr_t pedSlotAddrs;
};

extern const Addrs3* g3;
const Addrs3* DetectGame3();
