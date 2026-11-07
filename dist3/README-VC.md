# GTA Vice City savestates

Full savestates for GTA Vice City (PC, Japanese release), like snapshots of a virtual machine: save anywhere,
including in the middle of a mission or a cutscene, and jump back to that exact moment, in the same game session or
after restarting the game. Everything comes back: the mission script and its timers, every ped and car with its AI,
physics, traffic, wanted level, weather, the camera, the radio track and its position.

It is a separate program: **no file of the game is changed or added**. Start the game with `GTAVC-Savestates.exe`
to have savestates; start `gta-vc.exe` directly to play the untouched game.

| Key | Action |
|---|---|
| **F5** | save a savestate to the current slot |
| **F9** | load the current slot |
| **F6 / F7** | previous / next slot (10 slots) |

A short message in the top-left corner confirms each action. Saving takes about 40 ms; loading takes about 5 ms
for a savestate made in the same session and about 25 ms for one from an earlier session. Keys can be changed in
`savestate.ini`.

## Requirements
- `gta-vc.exe` of the **Japanese release** (Capcom, 2004). Other versions are detected and left alone; the log says
  so.
- Windows 10/11.

## Use
1. Unzip the folder with `GTAVC-Savestates.exe`, `savestate.dll` and `savestate.ini` anywhere you can write files
   (e.g. `D:\GTA Vice City Savestates`; not under `Program Files`). It does not need to be inside the game folder.
2. Run `GTAVC-Savestates.exe`. The first time it asks where `gta-vc.exe` is and remembers it (`[Launcher] Game=` in
   `savestate.ini`). It can also be given on the command line: `GTAVC-Savestates.exe "E:\...\gta-vc.exe"`; other
   arguments are passed on to the game.
3. Play. `savestate.log` next to the launcher lists every save and load.

The game runs in a window (`[Display] Windowed=1` in `savestate.ini`; 0 = full screen) the size of the resolution
chosen in the game's display options. A resolution too large for a window with a title bar (as tall as the screen)
is shown without one, at the top of the screen. Don't let a windowing tool such as DxWnd attach to the game as well.

The launcher starts the game paused, makes it load `savestate.dll` in memory before any game code runs, and lets it
go; nothing is written to the game folder. Some antivirus programs warn about programs that start another program
this way; if yours does, allow the launcher's folder.

## Things to know
- **Savestates are files** in the `savestates` folder next to the launcher (`slot01.sast` ... `slot10.sast`, about
  35 MB each). They stay after the game is closed and can be loaded in a later session, also after a reboot.
  Delete a file to empty its slot. A savestate loads with the same `gta-vc.exe` and at the same screen resolution
  it was made at (the message says so otherwise).
- Saving and loading are refused while the pause menu is open.
- Sound continues from where it was at the save: sound effects and loops (engines, sirens, voices) as well as the
  radio, cutscene soundtrack and mission dialogue.
- Vice City skips a cutscene when its window loses focus (the game's own behaviour, also in a window).
- The game has to be started by the launcher: savestates cannot be added to a game that is already running.

## How it works
The game keeps its state in the global variables of `gta-vc.exe` and in the memory its runtime allocates. The DLL
puts every allocation of the game's runtime into one region at a fixed address (by replacing `GlobalAlloc` /
`GlobalFree` for the game, in memory, once the executable's copy protection has finished setting up the game). A
savestate is a copy of the game's global data plus the used pages of that region, taken between two frames with the
streaming and audio threads idle, together with the game's Direct3D 8 objects (texture and buffer contents, device
state). Loading writes the memory back. Things that belong to the running process (window, Direct3D device,
DirectInput, Miles sound handles, open files, thread data, the timer's link to the system clock) stay as they are
and the restored game is connected to them; graphics objects from an earlier session are re-created and every
pointer to them is rewritten.

## Credits
- Crab1k.
- reVC and the GTA modding community's documentation as references for structures.
- Built with Claude Code (Anthropic), as an AI-assisted modding project.
