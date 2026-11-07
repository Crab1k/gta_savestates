# GTA San Andreas savestates

Full savestates for GTA San Andreas (PC), like snapshots of a virtual machine: save anywhere, including in the
middle of a mission or a cutscene, and jump back to that exact moment, in the same game session or after
restarting the game. Everything comes back: the mission script and its timers, every ped and car with its AI,
physics, traffic, wanted level, weather, the camera, the radio track and position. Ordinary save files only keep a
summary of your progress and can't be used during missions; these can.

It is a separate program: **no file of the game is changed or added**. Start the game with
`GTASA-Savestates.exe` to have savestates; start `gta_sa.exe` directly to play the untouched game.

| Key | Action |
|---|---|
| **F5** | save a savestate to the current slot |
| **F9** | load the current slot |
| **F6 / F7** | previous / next slot (10 slots) |

A short message in the top-left corner confirms each action ("Savestate 1 saved (55 MB, 35 ms)"). Saving takes
about 35 ms; loading takes about 10 ms for a savestate made in the same session and about 40 ms for one from an
earlier session. Keys can be changed in `savestate.ini`.

## Requirements
- `gta_sa.exe` **1.0 US** (HOODLUM or Compact) or **1.01 EU**. Other versions (1.01 US, Steam, Definitive Edition)
  are detected and left alone; the log says so.
- Windows 10/11.

## Use
1. Unzip the folder with `GTASA-Savestates.exe`, `savestate.dll` and `savestate.ini` anywhere you can write files
   (e.g. `D:\GTA SA Savestates`; not under `Program Files`). It does not need to be inside the game folder.
2. Run `GTASA-Savestates.exe`. The first time it asks where `gta_sa.exe` is and remembers it (`[Launcher] Game=` in
   `savestate.ini`). It can also be given on the command line: `GTASA-Savestates.exe "D:\...\gta_sa.exe"`; other
   arguments are passed on to the game.
3. Play. `savestate.log` next to the launcher lists every save and load.

Your own `d3d9.dll`, `DINPUT8.dll` and other files in the game folder work as before. The launcher starts the game
paused, makes it load `savestate.dll` in memory before any game code runs, and lets it go; nothing is written to the
game folder. Some antivirus programs warn about programs that start another program this way; if yours does, allow
the launcher's folder.

`savestate.dll` also works as a classic mod: renamed to `savestate.asi` in the `scripts` folder of an ASI loader
(its ini, log and savestates are then kept in `scripts`).

## Things to know
- **Savestates are files** in the `savestates` folder next to the launcher (`slot01.sast` ... `slot10.sast`, about
  55 MB each). They stay after the game is closed and can be loaded in a later session, also after a reboot.
  Delete a file to empty its slot. A savestate loads with the same `gta_sa.exe` and at the same screen resolution
  it was made at (the message says so otherwise).
- Loading a normal save file from the menu does not delete savestates: you can load a savestate made in another
  save file and in another mission.
- Saving and loading are refused while the pause menu is open.
- Sound continues from where it was at the save: sound effects and loops (engines, ambience, voices) as well as the
  radio and cutscene soundtrack. A few loops that have a short lead-in play the lead-in again.
- Each savestate made in the current session keeps the textures and models that were loaded when it was made, so
  many savestates in different parts of the map use more memory.
- The game has to be started by the launcher: savestates cannot be added to a game that is already running.
- Other mods that keep their own state (CLEO scripts, trainers) are not rolled back by a load.

## How it works
GTA SA keeps its game state in two places: the global variables in `gta_sa.exe` and the memory it allocates. The
DLL redirects every allocation the game makes into one region it controls, at a fixed address (by replacing the
game's `HeapAlloc` family in the import table, in memory, with its own allocator). A savestate is a copy of the
game's global data plus the used pages of that region, taken between two frames with the streaming and audio
threads paused, together with the game's graphics objects (texture and buffer contents, shaders, device state).
Loading writes the memory back.

Objects that live outside the game's memory are handled separately:
- **Direct3D**: every texture, buffer and shader the game creates is tracked. Within a session they are kept alive
  while a savestate needs them, and a load restores reference counts and the device state. A savestate from an
  earlier session re-creates them from the file and rewrites every pointer to them in the restored memory (also
  the textures the game keeps locked, like the number-plate font). RenderWare's shared vertex buffers and SA's
  texture recycling are switched off so a kept object never changes its contents.
- **DirectSound**: sound channels release their buffers before a load; the restored sound manager then starts each
  sound that was playing on a fresh channel at its saved play time. The streamed track (radio, cutscene) is
  re-opened at its saved position.
- **Windows and DirectInput** state, file and thread handles, the C runtime's per-thread and per-process data, and
  the timer's link to the system clock stay as they are; the savestate's references to the old ones are rewired.

## Credits
- gta-reversed and plugin-sdk (GTA modding community) as references for addresses and structures.
- Built with Claude Code (Anthropic), as an AI-assisted modding project.
