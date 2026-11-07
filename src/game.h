// Game-version-specific addresses: 1.0 US (HOODLUM and Compact share them) and 1.01 EU.
// Sources: gta-reversed RH_ScopedInstall/StaticRef annotations and the analysed 1.0 US Ghidra program.
#pragma once
#include "common.h"
#include <cstdint>
#include <string>

enum class GameVersion { Unknown, V10US, V101EU };

struct GameAddrs {
    GameVersion version;
    const char* name;

    // Writable game sections captured in a savestate: [dataBegin, dataEnd)
    uintptr_t dataBegin, dataEnd;

    // Main loop
    uintptr_t peekMessageRet;         // return address of WinMain's PeekMessageA call
    uintptr_t gGameState;             // int32, 9 = playing
    uintptr_t menuManager;            // CMenuManager FrontEndMenuManager
    uintptr_t menuActiveOffset;       // bool m_bMenuActive
    uintptr_t timerFrameCounter;      // uint32 CTimer::m_FrameCounter
    uintptr_t timerTimeMs;            // uint32 CTimer::m_snTimeInMilliseconds
    uintptr_t players;                // CPlayerInfo[2], CPed* at +0
    uintptr_t rsGlobal;               // RwGlobals-like RsGlobal; +0x14 = psGlobalType* (window, DirectInput devices)

    // CTimer fields that hold OS time; kept live across a load: {address, size}
    struct Range { uintptr_t addr; uint32_t size; };
    Range timerOsAnchors[4];

    // CRT
    uintptr_t getptd;                 // _ptiddata __getptd(void)
    uint32_t  holdrandOffset;         // _ptiddata->_holdrand

    // CdStream
    uintptr_t cdStreams;              // CdStream* gCdStreams
    uintptr_t cdStreamCount;          // int32 gStreamCount
    uintptr_t cdStreamQueue;          // Queue gStreamQueue {int* items; int head; int tail; int size}

    // RenderWare / D3D resource management
    uintptr_t rwD3DDevice;            // IDirect3DDevice9* _RwD3DDevice
    uintptr_t rwCreateVertexBuffer;   // RwD3D9CreateVertexBuffer
    uintptr_t rwDestroyVertexBuffer;  // RwD3D9DestroyVertexBuffer
    uintptr_t setUseResBuffering;     // D3DResourceSystem::SetUseD3DResourceBuffering(bool)
    uintptr_t useResBuffering;        // bool D3DResourceSystem::UseD3DResourceBuffering

    // Audio
    uintptr_t audioHardware;          // CAEAudioHardware AEAudioHardware
    uintptr_t smoothFadeThread;       // CAESmoothFadeThread AESmoothFadeThread
    uintptr_t soundManager;           // CAESoundManager AESoundManager

    // Values that belong to the running process, not to the game state: kept live when a savestate made by
    // another process is loaded (and re-checked at startup, see ValidateProcessGlobals).
    uintptr_t tlsIndex;               // DWORD __tlsindex: TLS slot of the CRT's per-thread data (_tiddata, 0x88 bytes)
    uintptr_t crtHeap;                // HANDLE _crtheap
    uintptr_t ioinfo;                 // ioinfo* __pioinfo[64] (32 entries of 0x24 bytes each); _nhandle just before
    uintptr_t crtInitCSCache;         // InitializeCriticalSectionAndSpinCount, cached by the CRT
    uintptr_t wmaLibrary;             // HMODULE wmvCoreModule, FARPROC WMCreateSyncReader
    Range cdStreamGlobals;            // gStreamFileCreateFlags .. end of gCdImageNames (handles, thread, LocalAlloc'd queues)
    uintptr_t rwWindowHandle;         // HWND WindowHandle, IDirect3D9*, adapter index, device, depth-stencil and render surface
    uintptr_t rwAdapterInfo;          // _RwD3D9AdapterInformation .. end of _RwD3D9DeviceCaps (0x350 bytes)
    uintptr_t rwPresent;              // D3DPRESENT_PARAMETERS Present
    uintptr_t winMainFreeArgs;        // call _free(argv) at the end of WinMain

    uintptr_t scriptSpace;            // uint8 CTheScripts::ScriptSpace[200000 + 69000] (main script, mission)
};

// Offsets inside audio classes (same in every PC version).
namespace audio {
constexpr uint32_t HW_NumChannels      = 0x8E;     // uint16
constexpr uint32_t HW_DirectSound      = 0xDA0;    // IDirectSound8* (each CAEAudioChannel holds a copy at +4)
constexpr uint32_t HW_Listener         = 0xE0C;    // IDirectSound3DListener*
constexpr uint32_t HW_StreamingChannel = 0xE10;    // CAEStreamingChannel* (0xE0C is the DS3D listener)
constexpr uint32_t HW_StreamThread     = 0xE14;    // CAEStreamThread (embedded, 0x50 bytes)
constexpr uint32_t HW_Channels         = 0xE64;    // CAEAudioChannel* [numChannels]
constexpr uint32_t ST_ThreadId         = 0x04;     // DWORD (CreateThread writes it here)
constexpr uint32_t ST_StreamingChannel = 0x08;
constexpr uint32_t ST_CritSec          = 0x38;     // CRITICAL_SECTION (0x18 bytes)
// CAEStreamThread request fields (what PlayTrack writes; the thread acts on them in Service)
constexpr uint32_t ST_NeedsService     = 0x12;     // bool
constexpr uint32_t ST_IsUserTrack      = 0x13;
constexpr uint32_t ST_NextIsUserTrack  = 0x14;
constexpr uint32_t ST_Preparing        = 0x15;
constexpr uint32_t ST_TrackId          = 0x18;
constexpr uint32_t ST_StartOffsetMs    = 0x1C;
constexpr uint32_t ST_NextTrackId      = 0x20;
constexpr uint32_t ST_TrackFlags       = 0x24;     // int8
constexpr uint32_t ST_PlayTimeMs       = 0x28;     // last play position seen by the thread
constexpr uint32_t ST_ActiveTrackId    = 0x30;
constexpr uint32_t CH_VtPlay           = 0x14;     // Play(int16 startOffsetMs, int8 flags, float freq)
constexpr uint32_t CH_DSBuffer         = 0x08;     // IDirectSoundBuffer*
constexpr uint32_t CH_DS3DBuffer       = 0x0C;     // IDirectSound3DBuffer*
constexpr uint32_t CH_BufferStatus     = 0x5C;     // DSBSTATUS bits
constexpr uint32_t SC_Decoder          = 0x6006C;  // CAEStreamingDecoder* (after the 0x60000-byte PCM buffer at +0x6C)
constexpr uint32_t SC_NextDecoder      = 0x60070;
constexpr uint32_t SC_State            = 0x60074;  // -6 = stopped; -3/-4/-7 dereference the decoder in Service
constexpr uint32_t SC_SilenceBuffer    = 0x60090;  // second IDirectSoundBuffer* the streaming channel keeps
constexpr int32_t  SC_StateStopped     = -6;
constexpr uint32_t DEC_DataStream      = 0x04;     // CAEDataStream* (IStream: vtable, then FILE* at +4)
constexpr uint32_t DS_File             = 0x04;
constexpr uint32_t FE_Size             = 0x20;     // tSmoothFadeEntry: IDirectSoundBuffer* +0, status +0x18 (1 active, 2 cancel)
constexpr uint32_t FE_Status           = 0x18;
constexpr uint32_t FT_ThreadId         = 0x04;     // CAESmoothFadeThread::m_dwThreadId
constexpr uint32_t FT_Entries          = 0x08;     // tSmoothFadeEntry[64], 0x20 bytes each
constexpr uint32_t FT_NumEntries       = 64;
// CAESoundManager: every sound plays "virtually" with its own play time; the ones holding a channel are listed per
// channel, and a sound that gets a channel starts there at its play time.
constexpr uint32_t SM_NumChannels      = 0x00;     // uint16
constexpr uint32_t SM_Sounds           = 0x04;     // CAESound[300]
constexpr uint32_t SM_NumSounds        = 300;
constexpr uint32_t SM_OnChannel        = 0x87F4;   // int16* sound index per channel (-1 = free)
constexpr uint32_t SND_Size            = 0x74;
constexpr uint32_t SND_InUse           = 0x58;     // uint16
constexpr uint32_t SND_PlayTime        = 0x5C;     // int16 ms (-1 = finished)
constexpr uint32_t SND_OnChannel       = 0x5E;     // int16 m_IsPhysicallyPlaying
}

extern const GameAddrs* g_game;
const GameAddrs* DetectGame();

// The CRT's per-thread data (_tiddata, VC 7.1): [0x18, 0x4C) holds strtok positions and lazily malloc'd buffers.
constexpr uint32_t PTD_SIZE = 0x88, PTD_BUFFERS = 0x18, PTD_BUFFERS_END = 0x4C;

// Startup checks of the per-process addresses above (the 1.01 ones are mapped from 1.0); false disables loading
// savestates made by another process.
bool ValidateProcessGlobals(HWND window, void* device, std::string& why);
