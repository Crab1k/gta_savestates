# MODLOG: GTA San Andreas full savestates

Goal (user, 2026-11-02): a mod that makes and loads **full savestates** (like VM snapshots), usable during
missions, not the generic save files. Target install: `D:\Grand Theft Auto San Andreas 1.1` (user's speedrun
install: `gta_sa.exe` = 1.01, `gta_sa_10.exe` = 1.0 US HOODLUM, trix `d3d9.dll` FPS hook, real `DINPUT8.dll`,
no ASI loader).

## Recon (2026-11-02)
- Exes: 1.0 US HOODLUM MD5 9369501599574D19AC93DE41547C4EC1 (entry 0x824570), 1.01 MD5
  25405921D1C47747FD01FD0BFE0A05AE (entry 0x825330). Both import only Win32 DLLs (static CRT: `HeapAlloc`,
  `HeapCreate`, ... from KERNEL32; no msvcr*). 1.01 IAT = 1.0 IAT + 0x1000.
- 1.0 US writable game sections: `.data` 0x8A4000-0xC9E000, `_TEXT_HA` 0xC9E000-0xCAF000, `_rwdseg`
  0xCAF000-0xCB0000. Sections after `.rsrc` are SecuROM/HOODLUM (not game state).
- Reused from earlier projects on this PC: gta-reversed + plugin-sdk + an analysed Ghidra program of 1.0 US
  (`E:\Claude Projects\gta_sa_opcodes`), UAL zip (`gta_sa_5\deps\ual`), windowed/autoload lab code.
  Ghidra headless needs `GSA_GHIDRA_USERDIR=E:\gsa_ghidra_user` here (Felix rejects the AppData dir);
  queries: `tools/ghidra/Decomp.java` (copied to `C:\gsa_scripts`).

## Route: whole-memory snapshot of game-owned memory (chosen 2026-11-02)
Structural serialization (peds' task trees, scripts, ...) is hopeless for "full" fidelity. Instead:
1. **Arena**: hook the exe's IAT `HeapAlloc/HeapFree/HeapReAlloc/HeapSize` -> TLSF allocator in one reserved
   region. All game allocations (CRT malloc, operator new, RW, pools, streaming) land there.
2. **Snapshot** = writable game sections + used arena pages (+ CRT rand seed). Restore = copy back.
3. **Safe point**: WinMain's `PeekMessageA` call (1.0 US 0x748A57, return 0x748A5D) via IAT hook: no game
   function is on the main thread's stack.
4. **Other threads**: CdStream thread (wait until channels idle: `gCdStreams` 0x8E3FFC, count 0x8E4090);
   audio stream thread (`CAEStreamThread::MainLoop` 0x4F15C0) and smooth-fade thread (0x4EEE90) both call
   `Sleep` through the IAT (pointer cached in a register at loop start) -> parked inside a `Sleep` hook.
5. **External objects**: D3D9 objects created by game code are tracked (device vtable hooks) and pinned; refcounts
   reconciled on load; device state restored by a `D3DSBT_ALL` state block captured at save.
   - RW `RwD3D9CreateVertexBuffer` (0x7F5500) sub-allocates shared VBs and never frees them -> replaced by one
     VB per call so a pinned VB's contents never change.
   - SA `D3DResourceSystem` recycles textures/index buffers (`UseD3DResourceBuffering` 0x8D6084) -> forced off.
   - Static audio channels create/release DirectSound buffers per sound (`CAEStaticChannel::Stop` 0x4F0FB0
     releases) -> on load, live channels are stopped properly and restored channels' buffer pointers nulled.
   - Radio decoders hold libvorbis state in vorbis DLL heaps (own VC6 CRT) and CRT FILEs -> on load, live
     decoders are deleted, restored ones are neutralised (FILE slot freed, object leaked), track re-requested.
6. Exclusions kept live across a load: CTimer OS-time anchors (0xB7CB28..0xB7CB48), stream-thread CRITICAL_SECTION.
7. Savestates are files in %TEMP% (valid for this game session only: they reference live D3D/DSound objects).

## Facts learned
- `CMemoryMgr::Malloc` -> CRT `malloc` -> `HeapAlloc(_crtheap 0xC9C2F4)`; `__active_heap` 0xC9C2F8 (system heap on Win10+).
- `srand` 0x821B11: `__getptd()` (0x827B3D) -> `_holdrand` at +0x14.
- CdStream: 5 channels, `CdStream` structs LocalAlloc'd (process heap, not restored); thread waits on a semaphore.
- `_rwD3D9RenderStateReset` 0x7FD100 invalidates RW's D3D state caches (not needed with state blocks).
- AEAudioHardware 0xB5F8B8: numChannels u16 +0x8E, m_aChannels +0xE64, m_pStreamingChannel +0xE0C,
  m_pStreamThread (CAEStreamThread, 0x50 bytes, CS at +0x38) +0xE14. Channel vtable: +0x1C Stop.
  CAEAudioChannel: DS buffer +8, DS3D buffer +0xC, m_nBufferStatus +0x5C.
  CAEStreamingChannel: decoder +0x60070, next decoder +0x60074, state +0x60078.
- Game threads (CreateThread xrefs): CdStream, AEStreamThread, AESmoothFadeThread, user-track scan (menu only).

## Progress
(see below)
- [x] TLSF arena + offline stress test (`tests/arena_test.cpp`, 388k ops, 0 corruption, full coalescing).
- [x] Lab: `lab\GTASA` = robocopy of the D: install (no backup\), `gta_sa.exe` = 1.0 US HOODLUM, 1.01 kept as
  `gta_sa_101.exe`, UAL (from gta_sa_5\deps\ual) as `vorbisFile.dll` + original as `vorbisHooked.dll`,
  `userfiles\` = copies of the 4 saves (RedirectUserFiles=1). `tools\labrun.ps1` copies the build, launches,
  waits for "TEST done", prints the log.

## In-game verification log
- **Run 1-3 (2026-11-02 04:00):** crashes found and fixed:
  1. `d3d9.dll` is a **delay-load import** (IAT slot 0x8E3ED4 in .data) -> normal IAT patch missed it; now the
     delay IAT slot is patched and the hook resolves the real function with LoadLibrary on first call.
  2. RW's vertex-declaration cache (0x7FAA30) keeps raw pointers without a reference -> collecting a "released"
     declaration crashed on the next cache hit. Declarations and shaders are now pinned forever.
  3. `AEAudioHardware+0xE0C` is the DS3D listener (vtable +0x44 CommitDeferredSettings); the streaming channel
     pointer is +0xE10 (cross-checked against the stream thread's copy at +0xE14+8).
- **Run 4 (walk test, 8 cycles):** 8/8 PASS (player pos bit-identical, game time identical after each load).
  Save: ~8 ms write (23 MB file: 4.2 MB sections + 19 MB arena pages), load ~6 ms memory copy. Arena only
  28 MB used in CJ's house. The CRT heap is created after the ASI loads (HeapCreate seen), so all CRT blocks
  live in the arena.
- **Run 5 (drive scenario, 2026-11-02 04:08):** 9/9 PASS. Sultan on the LS runway, save, teleport to SF / LV /
  countryside (4-5 km), load back: up to 5,506 GPU objects tracked, up to 2,700 refcounts reconciled per load,
  no crash, clean exit. Screenshots (`lab\GTASA\shots`) show the first frame after load identical to the save
  frame and correct textures/models afterwards.
- **Run 6-8 (mission scenario, 04:12-04:19):** mission 16 (`sweet2` script) launched with 0417 from save 3.
  Saves during the cutscene and during mission play (after a pad-tap cutscene skip); loads compare the full active
  script list (names + instruction pointers) and `$ONMISSION`: all MATCH, 6/6 PASS. Cutscene frames identical
  after load, animation continues. Streamed audio (cutscene soundtrack 744, ambience/radio 166) is re-opened at
  the saved play position via the stream thread's PlayTrack fields (0x12..0x24), then `Play` (channel vtable
  +0x14) once the thread has prepared it (checked by TryEnterCriticalSection on its CS, since the thread clears
  `m_bNeedsService` before preparing). SFX channels repopulate by themselves (33-42 playing a few s after load).
- Audio threads now sleep on an event that a savestate request signals, so parking takes <1 ms (was up to 100 ms).

## 1.01 EU support (2026-11-02)
- User's `gta_sa.exe` is **1.01 EU** (`[0x82533C]==0x94BF`). Its bytes at 0x401000 equal the value plugin-sdk uses
  for "1.0 US Compact", so detection now uses only the CRT version markers (0x82457C 1.0 US, 0x82533C 1.01 EU,
  0x8252FC 1.01 US = unsupported, no exe to map from).
- Addresses mapped with `tools/xver.py` (masked byte patterns of 1.0 functions; abs operands give data pairs) and
  verified by reference counts. Data: +0x2680 for 0x8E....-0xBA...., +0x27C0 for 0xC1....-0xC9...., 0x8D6084 ->
  0x8D844C. Code: +0x0..+0x900 by region. 1.01's RwD3D9CreateVertexBuffer jumps into `.dev1` after 8 bytes
  (obfuscation); patched from byte 0, so irrelevant. 1.01 has CRunningScript::Init relocated -> the lab script
  runner initialises the struct itself.

## Cross-savefile scenario (user request)
- Lab mode 4: mission A (16) in savefile 3 -> savestate A -> load savefile 1 through the pause menu
  (`m_bActivateMenuNextFrame` +0x33, then load-screen fields; ProcessFileActions does CheckSlotDataValid +
  DoSettingsBeforeStartingAGame -> WinMain restart) -> mission B (23, `drugs3`) -> savestate B -> load A, load B,
  load savefile 4, load A, load B.
- First run crashed 5 s after "load A from savefile 4": `CPad::GetMouseState` (0x746ED0) through
  `PsGlobal.diMouse` (`*(RsGlobal+0x14)` + 0x1C): the menu load re-creates the DirectInput devices. psGlobal
  (window, DInput interface/devices, last mouse pos) is now kept live across loads. Rerun: 4/4 PASS, clean exit.

## Over-release crash (2026-11-02 04:45-05:05)
- Drive scenario crashed deterministically 3 s after the 5th load (teleport into SF): AV in AMDXN32.DLL / ntdll
  heap code on the **process heap** (d3d9 HeapFree from RW render path). Not address space (1.1 GB free), not the
  audio restart (crash persisted with RestartStreamedAudio=0), not D3D state blocks (tests/d3d_refs.cpp: bindings
  and state blocks do not change public refcounts on this AMD driver).
- Lab diagnostics added: `HeapCheck=1` (HeapValidate of the process heap every 30 frames and around save/load) and
  `ReleaseCheck=1` (Release hook on tracked classes' vtables; logs a Release that drops a tracked object to 0).
- ReleaseCheck caught it: a **vertex declaration** (pinned forever, excluded from reconciliation) reached 0 refs
  from RW 0x7FAC48. RW's declaration cache hands out counted refs (AddRef per cache hit), so after a load the
  restored users released refs counted against the live timeline. Fix: declarations/shaders are reconciled like
  everything else and are only exempt from collection. 1.0: 9/9; 1.01: 23/23 over 20 cycles; heap clean.
- At shutdown RW releases its declaration cache once more than our accounting expects (logged as OVER-RELEASE after
  the game quit): harmless at exit.
- 1.01 lab runs stalled on the intro movies when the window was unfocused -> lab options `RunInBackground=1`
  (forces ForegroundApp, 1.0 0x8D621C / 1.01 0x8D88B8) and `SkipIntro=1` (game states 1-4 -> 5).
- Final cross-savefile scenario: 4/4 on 1.01 EU and 4/4 on 1.0 US.

## Savestates that survive a game restart (user request, 2026-11-02)
User: alt-tab in fullscreen doesn't matter (windowed use), but savestates must survive restarting the game.
- Census first (lab `Census=N`, src/census.cpp): every 32-bit value in .data and in allocated arena blocks that
  points outside the image, classified (tracked D3D object, COM object by vtable module, heap, exported function).
  Real external references: D3D objects (~2300: textures, VB/IB, decls, shaders) + the device, IDirect3D9 and its
  two implicit surfaces (RW globals 0xC97C1C..0xC97C34); DirectSound device (EAX wrapper) at AEAudioHardware+0xDA0
  and at +4 of every CAEAudioChannel, the listener at +0xE0C, the streaming channel's buffers (+8, +0x60090);
  DirectInput (psGlobal); HWND at 0xC8CF88/0xC97C1C/0xC9C05C (Present.hDeviceWindow); LocalAlloc'd CdStream arrays,
  CdStream handles/semaphores/thread; CRT heap handle, TLS index, __pioinfo (arena blocks with OS handles), the
  CRT's cached InitializeCriticalSectionAndSpinCount (0xC9AF94), WMA library + WMCreateSyncReader (0xB6BAD4/8),
  delay-load IAT/module handles (one slot points at our Direct3DCreate9 hook). CRITICAL_SECTIONs are safe to copy on
  Windows 10/11: DebugInfo = -1, LockSemaphore 0.
- Savestate file v2 (`<game>\savestates\slotNN.sast`, written to .tmp then renamed): header, .data, page bitmap +
  pages, then the GPU catalog: per object kind, creation parameters, game reference count, contents (managed
  textures level by level; all VB/IB incl. dynamic ones; decl elements; shader code), the device state as values
  (Get*), and texture locks the game holds. ~55 MB per slot (27 MB GPU). Save ~35 ms.
- Load from another process: commit the arena to the saved size, restore memory, re-create every object, rewrite
  every dword equal to an old object pointer (one pass over .data + allocated blocks, 64 KB-granule bitmap filter
  + hash map; ~2800 pointers), then put back the process's own values (keep list above), give each live game thread
  a fresh CRT per-thread block (TLS slot of other threads through the TEB; old blocks freed), re-home __pioinfo
  blocks, apply the device state, drop the previous state's references, Collect. ~35 ms.
- Arena reserved at a fixed base (`ArenaBase=0x10100000`, EAX.dll sits at 0x10000000); a savestate needs the same
  base, game exe (TimeDateStamp), back-buffer size/format. Same-process savestates still load through the pinned
  objects (fast path, ~7 ms).
- WinMain frees its argv array (arena block from startup) at exit -> NOPed (0x748EA6 / 0x7497AB): after a load from
  another process that address belongs to something else.
- Bugs found on the way: (1) arena pages beyond the fresh process's commit -> CommitAtLeast; (2) crash at quit in
  RW raster unlock: CCustomCarPlateMgr keeps the plate charset raster locked for the whole game (lockedSurface and
  pixel pointer in game memory) -> LockRect/UnlockRect/GetSurfaceLevel vtable hooks record held locks; the file
  carries them and the load re-locks the new texture and maps old surface/pixel pointers; the content of a held
  level comes from the game's own lock; d3d9 locks a level's surface through its vtable but unlocks it directly, so
  those records are dropped when the texture-level lock is taken; (3) world dark after a load from disk: building
  day/night colours live in DYNAMIC default-pool VBs that are only rewritten when the light changes -> dynamic
  buffers are saved too, read with SSE4.1 streaming loads (write-combined memory: 60 ms -> 5 ms); (4) vertex
  declarations re-created by each load stayed "permanent" and the next save wrote them all again (doubling per
  load) -> "permanent" now means "reachable from the current memory": reset on loads from disk, restored from the
  capture on in-process loads; captures pin only objects with game references, bound to the device, or permanent.
- Verification (tools/persisttest.ps1): run 1 saves two slots in mission 16 of savefile 3 and quits; run 2 (new
  process, savefile 1, mission 23 running, optionally with a 6 KB extra environment variable) loads both from disk,
  saves/reloads in-process, loads savefile 4 through the menu and loads again: 5/5 on 1.0 US and 5/5 on 1.01 EU,
  clean exits. Lab `ForceRecreate=1` loads every savestate through the from-disk path: mission 3/3 (screenshots
  match the saved frame), drive 15/15, walk 8/8 with flat address space. Regression on the pinned path: cross-
  savefile 4/4, drive 15/15.
- Reboot check (2026-11-02 13:05): slots written at 12:51 by 1.01 EU, Windows restarted at 13:03 (cold boot; an
  earlier "restart" had been a hibernate resume that kept DLL addresses). System DLLs moved (ntdll HeapAlloc
  77E0DBA0 -> 76FEDBA0, kernel32 76BD41B0 -> 767641B0, user32 PeekMessageA 77C8DB00 -> 759ADB00); arena still at
  0x10100000. Run 2 of persisttest on the pre-reboot files: 5/5, clean exit, frames render normally.

## Separate launcher instead of an ASI mod (user request, 2026-11-02)
User: keep the original game files intact.
- `GTASA-Savestates.exe` (src/launcher.cpp): CreateProcess(gta_sa.exe, CREATE_SUSPENDED); the initial x86 context has
  EAX = entry point and EBX = PEB (checked against PEB.ImageBaseAddress + AddressOfEntryPoint). A stub page in the
  game (VirtualAllocEx RWX) does LoadLibraryW(savestate.dll), records module/GetLastError/done, and jumps to the
  real entry point with the stack untouched; the launcher sets EAX = stub and resumes. RtlUserThreadStart therefore
  runs the stub after the loader has loaded the static imports and before the CRT creates its heap: same timing as
  an ASI loader, no file written, no code bytes of the exe patched. LoadLibraryW's address comes from the
  launcher's own kernel32 (same base in every 32-bit process of a boot). The launcher waits until the stub has run
  and reports a DLL that failed to load.
- savestate.dll keeps ini, log and savestates next to itself (as an ASI that is `scripts\`).
- Lab: lab\GTASA restored to the original files (vorbisFile.dll back, UAL and scripts\ removed); lab\launcher holds
  the launcher, the DLL and the ini with the [Lab] options; labrun.ps1 starts the game through the launcher and
  finds the game process by exact path. Mission 3/3; persisttest 5/5 on 1.01 EU (padded environment) and 5/5 on
  1.0 US, clean exits. The only files the lab writes into the game folder come from [Lab] options (screenshots,
  RedirectUserFiles).
- D: install: ASI mod uninstalled (vorbisFile.dll restored, hash equal to the original), savestate_backup and an
  empty savestates folder removed; no file in the game folder newer than the session. Launcher package in
  `D:\GTA SA Savestates` with `[Launcher] Game=` set.

## GTA III and Vice City (user request, 2026-11-02)
User: same separate programs for GTA Vice City (E:\Grand Theft Auto Vice City JP) and GTA 3 (D:\GTA3).
Code: src3/ (savestate3.dll for both games), build3.bat,
tools/labrun3.ps1, lab\GTA3 + lab\launcher_iii, lab\GTAVC + lab\launcher_vc (lab copies; user files of the real games
untouched; GTA3 User Files backed up to work\backup_user_files).
- Recon: both are D3D8 + Miles (mss32.dll at fixed 0x21100000 -> ArenaBase 0x30000000 for these games). gta3.exe =
  1.1 (non-Steam, rebuilt 2012 header, entry 0x5C2130). Vice City JP = 2004 exe wrapped by a protector (sections
  .bcvb/.ljtsrx, entry 0xACD0A2, parts of .text encrypted on disk). The Windows loader fills only the protector's
  import table (0xBD5000, includes HeapAlloc etc.); the game's own table (.idata 0x6EF000) is filled by the protector
  ~90 ms after start (work\probe.cpp logged it). Analysis of VC uses a memory dump taken after start-up
  (work\gta-vc-jp.dump.exe, sections raw==virtual) in Ghidra project work\ghidra3vc (program gta-vc-jp.dump.exe);
  GTA3 file analysed as gta3.exe. Query helper: work\gh3.sh <name> <program> (Decomp.java gained "t TEXT").
- Both use the Metrowerks (CodeWarrior) MSL runtime ("MW Win32 Runtime"): malloc pools on GlobalAlloc chunks, no
  HeapAlloc import -> heap hook = GlobalAlloc/GlobalFree import hooks (src3/hooks3.cpp). For VC the game table is
  patched late: one-shot hot-patch on kernel32!GlobalAlloc catches the first call from game .text.
- D3D8: no SDK header -> src3/d3d8min.h (vtable-order interfaces). tests\d3d8_refs.cpp: binding / state blocks do not
  change public refcounts (same as D3D9). src3/d3d8.cpp = port of src/d3d.cpp (screenshots via GetFrontBuffer).
- GTA III 1.1 addresses (src3/game3.cpp): WinMain 0x582A50, PeekMessageA call 0x582D5D (ret 0x582D63), gGameState
  0x8F58EC, FrontEndMenuManager.m_bMenuActive 0x8F5B9D (m_bWantToLoad 0x8F5EE0, m_bWantToRestart 0x8F5BA0), RsGlobal
  0x8F4414 (quit 0x8F442C, ps 0x8F4430 -> PsGlobal), CWorld::Players 0x9414A8 (0x13C each, ped +0, pos ped+0x34),
  CTimer: ms 0x885AF8, prev 0x8F2A98, frame 0x9414A4, user/code pause 0x95CF34/0x95CF69, OS anchors 0x62A308..0x62A324
  + oldPcTimer 0x9436AC; MSL TLS index 0x619790, thread-data list 0x663728 (0x288-byte blocks, link at +0),
  malloc 0x5BD7C0 / free 0x5BD7F0; CdStream block 0x621030..0x6212C8 (names, handles 0x621278, count 0x621274,
  gpReadInfo 0x6212A4, queue 0x6212A8, thread 0x621298 / id 0x6212A0); RW device 0x662EF0; RW dynamic VB pool list
  0x663344 (reuse branch jz at 0x5BBCD8 NOPed); WinMain free(argv) call 0x583398 (NOPed); ForegroundApp 0x60E8C8;
  user dir fn 0x580F00 (buffer 0x6510D8); ScriptSpace 0x74B248 (0x28000), pActiveScripts 0x8E2CA8, OnAMissionFlag
  0x8F1C18, CRunningScript: ip offset +0x10 (an offset), stack +0x14, sp +0x2C, locals +0x30, timers +0x70/+0x74,
  Process 0x439440, ProcessOneCommand 0x439500 (__fastcall); script floats are int16*16. Miles: driver 0x8F1AD8,
  stream 0x709C50, buffers 0x87C788 (sfx bank, size var 0x70D6A0) / 0x87C78C (ped speech, 0x87028).
- Lab: new game from the title menu = m_bMenuActive=0 at state 7, then clear CTimer user pause once the player exists
  (clearing it earlier crashed in audio code asking for the player). GTA3 needs gta3.set in the lab userfiles (copy of
  the user's) or it stops on "Cannot find 640x480 video mode". In-session test (new game, intro mission): 5/5 PASS,
  save ~30 ms (54 MB), load ~4 ms, clean quit.
- Cross-session (both games): census (src3/census3.cpp, AutoTestMode=3) showed the process-owned values. Kept live on
  a load from another process: RW D3D8 globals (GTA3 0x662EEC HWND, device, RT, DS, IDirect3D8; present params
  0x9431C8), psGlobal, CdStream block, TLS index, timer OS values, import slots, Miles handles (driver, stream slots,
  3D sample array, 2D sample, provider, mem_alloc_lock buffer pointers). Old RW device/surface/D3D8 pointers are
  remapped to the live ones next to the re-created objects; Restore(keep) runs again after the remap.
- MSL file table (GTA3 0x713FF8, VC 0x7F6F70: 256 pointers to {HANDLE os; bool text; ...}); FILE+0 is the table
  index, not the OS handle. Save records each open file's path (GetFinalPathNameByHandle) and OS position; load gives
  each restored record the live handle of the same file or opens it again, seeks to the saved position, and closes
  live files the restored runtime does not know (the SFX.RAW file stays open all game).
- Miles streams: handles belong to the process. Hooks on _AIL_open_stream/_close_stream/_set_stream_volume/_pan track
  file name, volume, pan per handle; save stores {file, ms position, status}; every load ends all 3D/2D samples,
  closes the live streams and re-opens the saved ones at the saved position (played if they were playing).
  Both games ship the same Mss32.dll whose exports start with "inc word [abs]" (66 FF 05, 7 bytes): entry hooks with
  a trampoline (VC's protector routes every Miles call through its own dispatchers; import hooks would miss them).
- Lab results: GTA3 forced re-create 5/5, persist (save, quit, load in a new process) 4/4; VC in-session 5/5,
  persist 4/4 (positions differ: the intro mission drives). Cutscene audio (BET.MP3 / INT_A.MP3) resumes at the
  saved ms. Real game folders unchanged (0 files modified).

### Vice City JP addresses (memory dump, work\gta-vc-jp.dump.exe)
- Sections: .data 0x681000, .bss 0x6F4000..0xA0DBB3; captured 0x680000..0xA0E000 (.asrc .. .bss). Protector pointer
  tables (0xAE2Cxx, 0xAFFAxx) and dispatchers (0xA7Bxxx) are outside it. Layout close to VC 1.0, most globals
  0x2FF0..0x2FF8 lower.
- WinMain 0x5FF660, call [PeekMessageA] at 0x5FFB0D (ret 0x5FFB13), free(argv) call 0x60043F. gGameState 0x9B2F18:
  1..8 logo/intro/Capcom/titles movies, 9 init once, 10 init frontend, 11 frontend, 12 init game, 13 playing.
  RsGlobal.ps 0x9B1904 (quit 0x9B1900), ForegroundApp 0x6D2D24, FrontEndMenuManager.m_bMenuActive 0x866670.
- CTimer::Update 0x4D0870: ms 0x971B34, frame 0xA0A8A8, user pause 0xA0DB46, code pause 0xA0DB87, OS values
  0x7022F8..0x702313 + 0xA0D690. CWorld::Players 0x947D30 (0x170 each).
- MSL: malloc 0x6608A0, free 0x6608D0, __sys_alloc 0x675C30 (call [0x6EF504] GlobalAlloc), __sys_free calls GlobalFree
  through protector slot 0xAFFA70 -> kernel32!GlobalFree hot-patched permanently. TLS index 0x6DBB18, thread data
  list 0x787098 (0x288 blocks).
- CdStreamInit 0x408C80: globals block 0x6F4488 (+0x298), count 0x6F46CC, read infos 0x6F46FC, queue 0x6F4700,
  thread id 0x6F46F8. RW: HWND 0x7867AC, device 0x7867B0, RT, DS, IDirect3D8 0x7867BC, present params 0xA0CD14,
  VB pool reuse jz 0x65EDC8.
- Scripts: ScriptSpace 0x81E288, active list 0x972340, OnAMissionFlag 0x975750, ProcessOneCommand 0x44FF50 (ip +0x10).
- Miles: driver 0x975558, 3 stream slots 0x975670, 3D samples 0x9481A8 (28), 2D sample 0x9B18D4, provider 0xA0CCAC,
  bank buffers 0x948790 / 0x948794 (ped comments, 0xB44E0), SFX.RAW FILE* 0x97562C.
- Code patches for VC happen in LateInit (after the protector), i.e. for both games: VB reuse, free(argv),
  lab user-dir hook. Hook_GlobalAlloc only takes calls from the game's .text (the header import table also serves the
  protector).
- Known: closing VC while in game (lab: RsGlobal.quit) crashes in the window-deactivate handler 0x405FA0, which asks
  for the player after shutdown; it happens with no savestate activity too.

### Packages
- tools/package3.ps1 [-Install]: dist3\iii, dist3\vc, GTA3-Savestates.zip, GTAVC-Savestates.zip; installed to
  D:\GTA III Savestates (Game=D:\GTA3\gta3.exe) and D:\GTA Vice City Savestates (Game=E:\Grand Theft Auto Vice City
  JP\gta-vc.exe). [Display] Windowed=0 by default: the user plays windowed through DxWnd, which also hooks games
  started by the launcher (dxwnd.dll was in the lab GTA III process).

## GTA III release, launch crash on the user's PC (2026-11-03)
- User report: installed GTA III launcher crashed on launch, Vice City did not start. Logs stopped right after
  Direct3DCreate8, before CreateDevice. Cause: other code re-hooks IDirect3D8::CreateDevice between the game's two
  Direct3DCreate8 calls (SDK 120, then 220): Windows' compatibility layer (apphelp.dll, seen for gta-vc.exe) and
  DxWnd. The old hook re-hooked whenever the slot was not ours and saved that code as its original; when that code
  calls us back (DxWnd) the two hooks call each other forever.
- Now: CreateDevice is hooked in front of whatever is in the slot (per vtable), d3d8.dll's own function is recorded,
  and a call that re-enters our hook goes straight to d3d8.dll. Objects count as the game's unless our own DLL made
  them (a hook in front of us would hide the game as caller). Device slots are not re-hooked over foreign code.
- The heap and main loop no longer depend on import slots or return addresses: the runtime's call [GlobalAlloc] /
  call [GlobalFree] and WinMain's call [PeekMessageA] are redirected (GTA III 0x5D825A / 0x5D82A0 / 0x582D5D;
  VC 0x675C3A / kernel32 hot-patch / 0x5FFB0D).
- DxWnd (H:\dxwnd, runs elevated, entries by exact exe path) hooks a game started by our launcher only while its
  hooking is switched on (CORRECTION, 2026-11-03: the module checks from 64-bit PowerShell could not see 32-bit
  DLLs; the reliable sign is the log line "CreateDevice was dxwnd.dll"). With DxWnd attached the game crashes in
  DxWnd's GetDepthStencilSurface right after its first device reset (d3d8.dll+0x4E0DB, during the logo movie);
  plain gta3.exe under DxWnd does not. Supported setup: the built-in [Display] Windowed=1, DxWnd closed or not
  attached (real install: 5/5, clean exit). The log now warns when dxwnd.dll is in the game.
- Verified: lab GTA III 5/5; the user's real D:\GTA3\gta3.exe through a test launcher (lab\launcher_real_iii, user
  files not redirected, gta3.set backed up and found unchanged): device created, 5/5 save/load, clean exit.
- Release: GTA3-Savestates.zip (tools/package3.ps1 -Games iii), installed to D:\GTA III Savestates (Windowed=1).
- Vice City: parked by the user. With the call-site changes VC exited with code 1 before creating its device; the
  vtable slot of its second IDirect3D8 pointed into apphelp.dll. Not retested with the final CreateDevice rule.
- Lab additions: AutoTestMode 5 (savefiles x missions x restarts, WIP): savefile loads through the game's own path
  (CheckSlotDataValid + load flag + m_bWantToRestart; GTA III 0x591CF0 / 0x8F5EE0 / 0x8F5BA0, VC 0x61A460 /
  0x866674 / 0x866671) from the title menu, or in game once no cutscene is loaded: GTA III's restart crashes in
  CCutsceneMgr::DeleteCutsceneData (camera reset to a deleted player) when a cutscene is loaded. Script scratch
  offset per game (VC's 0x27F00 was inside the main script). tools/labini.py sets [Lab] keys.

## GTA III: wrong geometry/textures after an in-session load (user report, 2026-11-03)
- Symptom (user screenshot): after driving around, F9 loads showed broken water/sky and wrong textures. Reproduced in
  the lab with AutoTestMode 6: start from a user savefile (title-menu load now works: load flag + menu closed at the
  title, m_bWantToRestart only once the game is playing, since WinMain checks it before PeekMessageA), save, teleport
  (SET_PLAYER_COORDINATES 0055) across three areas, CStreaming::DeleteAllRwObjects (0x407390), load.
- Two RenderWare mechanisms change pinned objects after the save:
  1. Vertex data: all world geometry lives in ~20 vertex buffers (6 dynamic + RW's sub-allocated static ones), parts
     reused after the save while the restored memory keeps the old layout. Fix: an in-session load writes back the
     saved contents of every vertex buffer (and dynamic index buffers) from the savestate file
     (Capture::dynamic = file offsets recorded by Write; d3d::RestoreDynamic; ~2-3 MB, ~1 ms).
  2. Textures: RW D3D8 texture creation 0x598B00 keeps a cache of destroyed square textures and hands one of the same
     size/levels/format to the next texture, which rewrites its pixels. Fix: jne at 0x598B13 -> jmp (cache never used).
     VC JP's RenderWare has no such code.
- After both: savestates load exactly as saved after streaming other areas (lab screenshots), cross-session 4/4.
- Lab screenshots now come from the back buffer before Present (the front buffer copy included other windows when
  the game was covered). The lab status line is only logged when savestate.ini has a [Lab] section.

## GTA III: ambient loops silent after loads (user report, 2026-11-03)
- Every load ends all Miles 3D/2D samples. GTA III's cAudioManager (0x880F70) keeps its playing sounds per channel in
  m_asActiveSamples (+0x1414, 0x5C each, count at +3): ProcessActiveQueues (0x57BDB0) only updates an entry that
  matches a request, so restored entries kept loops (boat engine, waves) "playing" on channels Miles had stopped.
  Fix: after a load every entry gets entity -5 / sample 0xBD9 (none), so the next frame starts each wanted sound.
- Lab check (AutoTestMode 6): "silent loops" = entries with a sample, loop count 0 (+0x20) and a Miles channel not
  playing. Without the reset 1 and 7 after loads; with it 0 (sounds playing 5-9 channels).
- Two GTA III processes at once freeze each other on the loading screen: CdStreamInit creates the named semaphore
  "CdStream", shared by every instance. Never run a lab game while the user plays.

## GTA III: sound effects resume mid-sample (user request, 2026-11-03)
- Save records, per audio-manager channel, whether Miles plays it and its position (AIL_3D_sample_offset; channel 28
  = the 2D sample, AIL_sample_position). File VERSION 4 (ChannelRec[] after the FileRecs; older files are refused).
- Load: every channel that was playing is started again from its restored tSound record the way
  ProcessActiveQueues starts a sound (cSampleManager 0x7341E0: InitialiseChannel 0x5679F0, frequency 0x567D20,
  emitting volume 0x567B70, loop points 0x567D80, loop count 0x567DF0, reverb 0x567980, 3D position 0x567BE0 from
  cAudioManager::TranslateEntity 0x57AFB0 (thiscall) or the 2D pan, distances 0x567C20, StartChannel 0x567ED0),
  then seeked to the saved offset (before and after the start). Other channels are cleared as before.
- Lab: in session 5/5 and 7/7 resumed at their saved position, silent loops 0; from disk in a new process 9/9 and
  6/6. AutoTestMode 4 now also honours StartSavefile.

## Vice City JP release: starts, windowed, sound resume (user request, 2026-11-03)
- WinMain is protected: the patched "call [PeekMessageA]" stops it after one frame, so VC hooks the PeekMessageA
  import and recognises the main loop by return address (peekViaImport). GlobalFree: kernel32 hot-patch.
- VC streams through its own Miles file callbacks (MSL FILEs in the game heap; AIL_set_file_callbacks keeps them in
  four globals read from its `A3 imm32` stores): the open callback is wrapped to know each stream's FILE. Load closes
  the live streams before memory is restored and the saved streams' FILEs (in the restored heap) after it. File
  VERSION 5 (StreamRec has that FILE); version 4 files (first GTA III release) still load (lab: 4/4 from disk).
- RW texture caches off: jne 0x6275E5 -> jmp, and "mov esi, [count]" 0x627680 -> xor esi, esi (released textures).
- Sound effects resume as in GTA III (cSampleManager 0x980BA0, cAudioManager 0x78A720, active samples +0x14F0 0x60
  each; VC also scales by the effects volume at +0x5538 / flags +0x5536). Lab: churn 3/3, sounds resumed 8/8.
- Lab savefiles: VC computes the user folder again before C_PcSave::SetSaveDirectory (call 0x601920), redirected too.
- The real install quit at once (exit 0, also without our DLL): gta_vc.set keeps the video mode as one byte at file
  offset 1489 (LoadSettings 0x48FE4B: menu +0x60), saved as 20 under DxWnd's emulated mode list. Without DxWnd
  RenderWare lists 20 modes, so psSelectDevice falls back to 640x480x16, which this PC lacks (the lab copy's path
  has the DWM8And16BitMitigation shim, which adds 16-bit modes). The user chose a settings fix over a code patch:
  byte set to 8 (1152x864x32); original in work\backup_user_files. Real install: 5/5, windowed.
- Quitting during a cutscene crashed in WM_ACTIVATE (0x601132): losing focus skips a running cutscene
  (CCutsceneMgr::ms_running 0xA0DAC2, skip 0x405FA0 reads FindPlayerPed()), and DestroyWindow deactivates the
  window after the players are gone. The DLL subclasses the window and clears the flag in that case only.
- The crash logger starts after the protector ran (its DLLs raise and handle access violations while unpacking).

## GTA SA: sound effects resume mid-sample (user request, 2026-11-03)
- SA's CAESoundManager (1.0 US 0xB62CB0, 1.01 EU 0xB65330: 259/258 references) keeps 300 CAESounds (0x74 each at
  +4) playing "virtually" with their own play time (+0x5C, ms); the ones on a channel are listed per channel
  (int16* at +0x87F4) and flagged (+0x5E). Service gives a free channel to a sound without one and starts it at its
  play time (CAEAudioHardware::PlaySound -> CAEStaticChannel::Play seeks), as when a sound gets its channel back.
- Before: the load emptied the channels, so the restored sounds read play time -1 from them and finished (loops were
  requested again from the start, one-shots were lost). Now the load also clears the per-channel list and every
  sound's on-channel flag: the next Service starts each one where it was. No file format change.
- Exception (the game's rule): a loop with a lead-in before its loop start (buffer = loops + lead-in at the end) is
  not seeked and plays the lead-in (< ~100 ms) again.
- Lab (log "N sound effects playing at the save, M on a channel again, K going on from their position", 3 frames
  after the load): drive 4/4 PASS, 7/7/7; cross-session 1.0 US 5/5 PASS and 1.01 EU 5/5 PASS, e.g. 33 -> 27 -> 25
  (the others: short footsteps that ended in those frames, sounds already asked to stop, lead-in loops).
- Release: new DLL and README in dist, GTA-SA-Savestates.zip and D:\GTA SA Savestates; launcher exe unchanged
  (the released one; src/launcher.cpp has since changed for III/VC). Old release in work\sa_release_2026-11-02.

## III/VC: a player's PC (friend of the user, 2026-11-03): tall windows, read-only page in a load
- Their game quit at start (exit 0, log ends after the pixel shaders, before the main loop) at 1920x1440 on a
  2560x1440 screen: Windows cuts a window down to its maximum track size (screen + ~20 px), so the framed window's
  client area was shorter than the back buffer, RenderWare could not create the camera and WinMain returned.
  Lab: 2560x1440 (mode 19 in the lab gta_vc.set) gave the same exit with the released DLL.
- Fix (d3d8.cpp MakeWindowed): a window that does not fit the monitor's work area with a title bar is borderless
  (WS_POPUP) at the top of that monitor, centred; the window procedure raises WM_GETMINMAXINFO's max track size to
  the window size, so modes larger than the screen still start. Lab: 2560x1440 borderless, autotest 5/5; a build
  that always frames it: window 2566x1469, client 2560x1440, in game.
- Their next session crashed in the load: the memcpy of the data range stopped at 0x6F3000 (_rwcseg, 0x31B000
  bytes left) with a write access violation: that page was not writable on their PC (here it is WRITECOPY; the
  protector itself makes .idata RWX). Lab: VirtualProtectEx of that page to READONLY reproduced the crash at the
  same DLL offset with the released DLL.
- Fix (state3.cpp): the load makes non-writable regions of the data range writable (RWX if executable), writes
  them and restores their protection (flushing the instruction cache for executable ones); logged as "load:
  <range> had protection X, written through". Lab with the page READONLY and EXECUTE_READ: 5/5 PASS each; GTA
  III autotest 3/3.

## III/VC: crash ~10 s after loading a Riot savestate from disk; radio silent after loads (friend, 2026-11-03)
- Their log: the load wrote through 0x6F3000 (protection 0x20 there), then 10 s later CRASH at 0x46345E: the
  LOCATE_CHAR_*_3D handler (0x463400, opcode 0x100 in esi/edi, CRunningScript 0x7DFEC0 in ebx) read +0x3AC of a
  null CPools::GetPed result. MONO_8 (loaded only by LAWYER4 = "Riot", mission 6) was in the savestate, so it was
  their own Riot savestate; neither LAWYER4 nor the main script ever uses opcode 0x100: a script ran code it never
  had there.
- Cause found (tools: scratchpad remapscan/remaprules/remapfinal.py read .sast files): a load from another session
  rewrote every 4-byte word equal to an old graphics object address. Script code is full of 0x04 type bytes, and
  on their PC objects live at 0x04xxxxxx (device 04E666C0): over 400 random object addresses, 7.5% of loads of a
  lab VC savestate (1985 objects; theirs had 4677) rewrote script words; GTA III savestates 2-8%. Even here each
  load changed words in texture pixels (VC), vertex colours and a .data table (III).
- Fix (state3.cpp RemapMemory): ScriptSpace is skipped; a texture/surface match counts in a raster extension
  (raster at -0x34 is its own parent, or width/height at -0x28, or original size at -0xC while a mip level is
  locked), in arrays of them, or first in a RenderWare pool block (header before it points at the pool, which
  points at itself at +0x14); an index buffer in instance data (its vertex buffer follows) or in the data range;
  vertex buffers, RW device/surfaces and held locks anywhere. Changes are collected, then applied (the rules read
  neighbours). d3d::Remap values carry kind and size. On 5 savestates every real reference is kept and every
  rejected match is chance data; chance changes per load drop ~20x. Not verified on their savestate file.
- Radio: VC's stream open callback (0x5D5620) records ADF files for XOR decoding and renames the extension in the
  name it is given to "mp3", so the tool recorded "AUDIO\VCPR.mp3", which does not exist: every load left the radio
  silent. The name is copied before the call; old savestates retry ".mp3" as ".ADF".
- Lab: AutoTestMode 7 (savefile, player warped into a new car, radio on): same session and from disk in a new
  process, stream 0 AUDIO\VROCK.ADF reopened at its position, 20 s of play after each load, PASS. VC persist
  (mode 4, savefile 1) 4/4. GTA III persist from savefile 1 crashed in Mssfast.m3d+0x36B7 (read 0x123EB020)
  right after the second load from disk, with the released DLL too (fixed below).
- Another session started a game while lab runs went: two instances block each other (shared CdStream
  semaphore); lab runs now check for any running GTA process first.
- Release: zips and D: installs updated (DLL md5 56C93A48...); previous release in work\iiivc_release_2026-11-03b.

## III/VC: ped comment slots after a load from disk; cross tests with restarts (user request, 2026-11-03)
- The GTA III crash above: the saving process's ped speech buffer was at 0x123E3020; the read was 32 KB into it.
  LoadPedComment (III 0x5674F0, VC 0x5D6AE0) loads each comment into slot n of that buffer (7 slots; III 79000
  bytes, VC 0x19C20) and records slot * size + buffer in a table (III 0x940904, VC 0xA0A218) that InitialiseChannel
  (III 0x5679F0, VC 0x5D6560) uses for the sample address. The buffer pointer was kept live, the table was not: a
  comment playing in a savestate from another process started from the old process's buffer. Fix: Addrs3
  pedSlotAddrs; the load rebases table entries inside the saved buffer to the live one (log "audio: N ped comment
  slots moved"). The census missed it: in its two processes Miles put the buffer at the same address.
- Lab: the crashing GTA III persist run 4/4. Cross scenario (mode 5, the SA-style test: savefile A + mission A,
  savestates, quit; new process: savefile B + mission B, loads from disk, same-session save/load, savefile C loaded
  in game, load again; new process: new game, loads from both earlier processes, savefile A, load), now with 12 s of
  play after each load: GTA III (savefiles 1/6/3, missions 30/54) 7/7; Vice City (savefiles 1/2/3, missions 6 =
  Riot / 12) 7/7, LAWYER4 running on after every load. Runner: tools/crossrun3.ps1 (checks for any running GTA
  game before each phase, restores the ini).
- A first VC run hung in phase 2: the pause menu opened 5 s after a load (VC opens it when the window loses focus)
  and the savefile load behind it left the game in the menu. The lab now logs focus changes and closes a pause menu
  that opens in game during a test; the rerun had no focus change.
- Release: DLL md5 744D352B...; previous release in work\iiivc_release_2026-11-03c.

## SA: the same load fixes (user request, 2026-11-03)
- SA's load from another session used the same blind rewrite (src/state.cpp RemapMemory). Measured on the user's
  three 1.01 EU savestates (5359-5786 objects: ~2600 textures, ~1900 vertex buffers, ~1000 index buffers, a few
  declarations/shaders) and a lab one (scratchpad sascan.py / sarules.py): at random object addresses a load changed
  9-10 words of game data by chance (none in script code in these samples, unlike III/VC).
- Real reference sites in SA (RW 3.6 D3D9): textures in raster extensions (raster at -0x34: own parent, or size at
  -0x28 / original size at -0xC; a locked raster shows a mip level's size, e.g. 64x64 for a 256x256 texture), right
  after their size, in arrays; index buffers in RxD3D9ResEntryHeader (+0, vertex buffer at +8, second stream +0x18);
  vertex buffers there and in RW's list of created buffers (0x14-byte records {size, flag, buffer, owner, link}:
  link = record buffer field - 0x1C, the owner slot holds the same buffer, or the neighbouring record holds one);
  declarations (a 12-byte cache table), shaders, RW device/surfaces and DirectSound objects: few keys, anywhere.
- Fix: the same rule shape as III/VC with these layouts; ScriptSpace (1.0 US 0xA49960, 1.01 EU 0xA4BFE0, found by
  main.scm's models segment in saved memory; 269000 bytes) is skipped. On the four savestates every real reference
  is kept; what is left out is pixels, packed numbers, floats and one stale texture array. Chance changes per load
  drop from ~10 to 0.05-0.3.
- The read-only page fix (MakeWritable / RestoreProtection around the data restore) is in SA's load too.
- Lab: persisttest.ps1 (mission A in savefile 3, quit; new process, savefile 1 + mission B, loads from disk,
  same-session save/load, savefile 4 through the menu, load) with 12 s of play after each load from disk: 1.01 EU
  5/5, 1.0 US 5/5; one chance match left per load (DXT pixels).
- Release: DLL md5 90723292... in dist, GTA-SA-Savestates.zip and D:\GTA SA Savestates (launcher exe unchanged);
  previous release in work\sa_release_2026-11-03.

## III/VC/SA: crash at 0x6272A5 loading the user's own savestate; pointer rules reverted (user report, 2026-11-04)
- The user's Vice City (real install, 1152x864): a savestate made in a mission (GENERL1), game restarted, load from
  disk -> "Unhandled exception c0000005 at 006272a5" 40 ms after the load; log: "544 values ... left as they are".
  The rules of 2026-11-03 had rejected real references the lab savestates never had: arrays of index-buffer
  pointers and textures in RenderWare pool blocks (header 0x30004190). The stale pointers pointed at freed d3d8
  memory (0xE0E0E0E0) and RW's texture code (0x6272A5) read through one. Lab, user's file copied in, lab at
  1152x864 (gta_vc.set byte 1489 = 8): the released DLL crashed at the same address.
- Fix: every match is rewritten again, except in ScriptSpace (III/VC/SA), which never holds graphics pointers;
  matches there are counted ("N words of script space ... left as they are"). The user's savestate has 2 such
  words (0x841194, 0x8470FC = IB address 0x05001B00 in script bytes): the pre-2026-11-03 blind rewrite would have
  corrupted the script with them too. GTA III lab cross runs show 1 per load. The kind/size remap records went away.
- Lesson: rules validated only on lab savestates fail on real play states; a missed pointer is a crash, a chance
  change outside scripts is a pixel or vertex value. Load-path changes are now also checked against the user's own
  savestates (VC slot 1; GTA III slots 1-2 at 1680x1050; SA's are 1142x854 under DxWnd and cannot be matched).
- Lab: user's VC savestate loads twice, 20 s of play each; user's GTA III savestates 2/2, 20 s each; cross tests
  VC 7/7, III 7/7; SA persisttest 1.01 EU 5/5, 1.0 US 5/5 (its phase 1 crashed once at 0x42480000 while quitting
  after the saves, before any load: not the load path). Another session ("Port two savestate-engine fixes") also
  runs the SA lab in this checkout: a run collided once (DLL in use).
- Release: III/VC DLL md5 755DDAC7..., SA DLL 4ACB39AF...; previous ones in work\iiivc_release_2026-11-04 and
  work\sa_release_2026-11-04.

## III/VC: a dialogue line loaded but not yet played, across a load (user question, 2026-11-04)
- Another session ("Port two savestate-engine fixes", gta_3d_tas) noted that a preloaded mission line (opened +
  AIL_service_stream, not started) has Miles status SMP_DONE (2) like a finished one, and RestoreAudio restored
  status 2 as finished (position at the end, started), so the line would be skipped.
- The games: GTA III PreloadStreamedFile 0x567FA0 = open, loop count 1, AIL_service_stream; StartPreloadedStreamedFile
  0x5680B0 = AIL_start_stream; IsStreamPlaying 0x5685E0 = status == 4. AIL_start_stream rewinds.
- Lab AutoTestMode 8 (savefile; LOAD_MISSION_AUDIO 03CF - VC slot 1 MONO_8, III MF1_A; save; PLAY_MISSION_AUDIO 03D1
  to the end; save; load each and play again; streams logged every second via ss::StreamStates): with the old restore
  the line was NOT skipped in either game (started from its end it showed as playing for a moment, then the script's
  play rewound it: VC 947..3950 of 4813 ms, III 969..2969 of 3830 ms).
- Restore made exact anyway: only a stream that was playing is started; every other one is reopened at its saved
  position and buffered, as the game preloads (a buffered, unstarted stream reports SMP_DONE). After the load the
  waiting line is "status 2 at 0" as saved and plays whole; a finished line stays finished. VC cross test 7/7.
- Release: III/VC DLL md5 in the install; previous in work\iiivc_release_2026-11-04b.
