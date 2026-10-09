# RE3-1999CabbyCodes — project guide

A client-side mod for **Resident Evil 3: Nemesis (1999)** — Steam app 4249120 (`4249120_Biohazard3`),
the 1999 PC `ResidentEvil3.exe` wrapped by Enigma Protector, drawn through GOG's DirectDraw →
Direct3D 9 wrapper — cross-built on Linux with mingw-w64. It draws a Dear ImGui panel beside the
game's **status screen** with God mode, One hit kills, Infinite ammo, Freeze countdown timer (the
Mercenaries' clock, the main game's timed scenes and the missile on the radar at the end alike),
Infinite ink ribbons, Save without
counting, Freeze play time, Heal without counting, save-count,
play-time and **recovery total** editors (the three things the ending ranks on), a **clear count**
editor (the epilogues the ending shows and the title menu's EPILOGUE screen unlocks; main game only), an
**outfit** picker (Jill's costume, by the names the title's COSTUME SELECT gives them, worn from the next
door; main game only), a
**Mercenaries money** editor (the run's takings and the bank; its section of the panel is only there
in the Mercenaries), a **reloading tool proficiency**
editor (the level each kind of ammo is made at), an
**inventory editor** for the ten slots of whoever is in play and the game's 64-slot **item box**,
laid out as RE1's panel is; beside the game's **load and save screens** (the memory card screen) a
**save file manager** that deletes a save or copies one into another slot, as RE1's and RE0's do on
their load lists; and, on every screen, a **cutscene skip** (F8 or the pad's Start holds a
cutscene or movie, picture and sound, and asks first). The same DLL runs in **The Mercenaries**,
which on PC is an exe of its own (`RE3_MERCE.exe`, see "The Mercenaries"). Every
cheat switch is written back to the ini when it changes, so they survive a relaunch. `README.md` is the user-facing doc; this file records
**what was reverse-engineered**. It is a sibling of `../RE1-1996CabbyCodes` (same architecture:
proxy DLL, IAT hooks in `src/mem.h`, a main-thread tick, ImGui from the D3D9 device) and most of the
infrastructure came straight across; everything under "What the game does" is RE3's own.

## Build & deploy

```sh
make            # -> build/ddraw.dll   (config.mk sets GAME_DIR = .../4249120_Biohazard3/english; gitignored)
make install    # rename stock ddraw.dll -> ddraw_orig.dll (once), deploy ours atomically
make uninstall  # restore the stock DLL
make version X.Y.Z   # set the version;  make package -> dist/RE3-1999CabbyCodes_vX.Y.Z.zip
make proxy      # regenerate ddraw.def + src/proxy_exports.inc from the stock DLL's export table
python3 tools/check_sigs.py   # every signature game.cpp uses, against the dump, before a launch
python3 tools/check_sigs.py --dump ghidra_proj/RE3_MERCE.dumped.exe --any-build   # ...and the Mercenaries'
# savefiles.cpp against a stand-in for the game (a card in memory, its file in tests\build\saves),
# under Wine, from the repository's root; a card file as an argument adds a copy and a delete on it:
i686-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -static -Isrc \
    tests/test_savefiles.cpp -o tests/build/test_savefiles.exe && wine tests/build/test_savefiles.exe
# reverse engineering (DumpImage=1 in the ini writes ResidentEvil3.dumped.exe + .imports.txt, whichever
# exe it ran in - RE3_MERCE.exe's dump is kept as ghidra_proj/RE3_MERCE.dumped.exe):
python3 tools/fix_dump.py --dump ghidra_proj/ResidentEvil3.dumped.exe \
    --imports ghidra_proj/ResidentEvil3.imports.txt --out ghidra_proj/ResidentEvil3.fixed.exe
MAXMEM=8G /opt/ghidra/support/analyzeHeadless ghidra_proj RE3 -import ghidra_proj/ResidentEvil3.fixed.exe \
    -overwrite -loader PeLoader
tools/ghidra/run.sh xref 0xa61f58          # what refers to an address, and from which function
tools/ghidra/run.sh dec 0x44f0f0 x:0xa5ccc0  # decompile; "x:"/"d:" mix modes in one run
i686-w64-mingw32-objdump -d -M intel --start-address=0x44f0f0 --stop-address=0x44f140 \
    ghidra_proj/ResidentEvil3.fixed.exe
```

**A full disassembly is worth keeping**: `objdump -d -M intel --start-address=0x401000
--stop-address=0x50a000 ghidra_proj/ResidentEvil3.fixed.exe > ghidra_proj/re3.asm` (gitignored) makes
"where is this global touched" a `grep`, which is seconds rather than the ~40 s a headless Ghidra
query costs.

The log (`RE3-1999CabbyCodes.log`, the run before it in `.prev.log`), the ini
(`RE3-1999CabbyCodes.ini`) and the ImGui layout file sit beside the DLL in the game's `english`
folder. `Trace = 1` adds diagnostics (every name Enigma resolves through the exe's GetProcAddress
slot, state transitions); `AlwaysShow = 1` draws the panel on every screen; `DumpImage = 1` +
`DumpDelay = 45` writes the unpacked image once the game has reached its title screen;
`Watch = 0xA61F58:4,...` logs values as they change; `Disable = overlay,dispatch,game,gpa,input,cursor`
bisects a fault. Keep them on only for a diagnostic run and reset them after.

`tests/test_adopt.cpp` (with `tests/fake_ddraw.cpp`, a stand-in for GOG's wrapper) runs the D3D9 hooks
under Wine, with the Steam overlay's way of hooking or Windows' compatibility shim for
`ResidentEvil3.exe` played by the test (build and run commands in their headers; `RE3CC_ADOPT=1` makes
the proxy take the Windows path under Wine). Keep its scratch
folder in `~/.cache/re3cc-test/adopt`, not in `/tmp`. See "The panel's hooks on Windows".

### Testing without playing

The game can be brought up headless, on this machine, without taking the screen: Steam must be
running (the Enigma wrapper calls `SteamAPI_Init` and exits if it fails — plain `wine` gets
`"[S_API] SteamAPI_Init(): Could not determine Steam client install directory."` and quits inside a
second), and then

```sh
env STEAM_COMPAT_CLIENT_INSTALL_PATH=~/.local/share/Steam STEAM_COMPAT_DATA_PATH=<scratch pfx> \
    SteamAppId=4249120 SteamGameId=4249120 WINEDLLOVERRIDES="ddraw=n,b" \
    timeout -k 5 90 gamescope --backend headless -W 1024 -H 768 -- \
    "$PROTON/proton" run <scratch>/ResidentEvil3.exe
```

runs the real game at 60 fps against a copy of `english/` (the `Rofs*.dat` symlinked, `bio3.ini` set
to `Mode=Window` and `DisableMovie=on`). That is enough to see the whole mod work: the unpack, the
import hooks, the D3D9 device, the tick, discovery and the overlay. The game starts its attract-mode
demo by itself after ~25 s, which is a running game session to test against.

**Run bare like that, keep the gamescope desktop 4:3** (2026-10-05). Outside Steam's container,
Proton's 32-bit side cannot load `libXi.so.6` (this host has no `lib32-libxi`), so Wine has no
XInput2 and refuses every cursor clip. On a 16:9 desktop (1920x1080, 2560x1440) GOG's wrapper clips
the pointer to the pillarboxed 4:3 picture, and the game's DirectInput `Acquire` (0x407846, in its
device enumeration) then spins inside Wine's refuse-and-reclip loop for good: a white window, the D3D9
device made, no tick, no Present. At 4:3 the clip is the whole screen and nothing notices. Steam runs
Proton inside Steam Linux Runtime 4, which brings its own 32-bit libXi, and 2560x1440 runs there
with either presentation: put `"$SLR4/_v2-entry-point" --verb=waitforexitandrun -- "$PROTON/proton" waitforexitandrun`
in place of `"$PROTON/proton" run`, with `STEAM_COMPAT_TOOL_PATHS="$PROTON:$SLR4"` exported
(`SLR4=~/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4`). `bio3.ini`'s `Mode` does not
decide windowed-or-not with the wrapper loaded - `dxcfg.ini`'s `presentation` does. To see the
picture, `GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 gamescopectl screenshot <path>.png` (the headless
gamescope's socket; the extension is required).

**Input can be driven, too** (2026-10-06): the headless gamescope runs Xwayland on `:1` (its log
says "Starting Xwayland on :N"), and XTEST key and pointer events sent there through `libXtst.so.6`
(ctypes; no xdotool on this host) reach the game's DirectInput and the panel. From launch: Enter,
Space, Enter, X to get through the logos and opening movie to the load prompt; Down, Enter for "Do
not load"; Left, Left, Enter for Easy; Enter for the costume - and a new game starts with its intro
narration (`0xA61F5C` = 0x28), a movie, then the session and, ~9 s in, the opening in-engine scene
(0x88, Jill through the window; ~8 s). That is a repeatable cutscene test without a save.

**Clicking the panel needs Steam Linux Runtime 4** (2026-10-08; `~/.cache/re3cc-test/run_slr.sh`).
Under the bare `proton run` above, the first click on the panel froze the game for good: ImGui's
Win32 backend answers the button press with `SetCapture`, and Wine without XInput2 then spins in the
same refuse-and-reclip loop as above - the main thread inside one `PeekMessageA` call (in the game's
pump at 0x408450, so no tick and no frame), every task thread waiting at the hand-off (0x4133FF), no
log line, the pointer and keys ignored. Hover still works; it is the press. Inside SLR4 every click
worked. The 2026-10-07 checkbox clicks under plain Proton (Freeze Mercenaries timer, now Freeze
countdown timer) did not hit it -
why is not known, so test clicks in SLR4. The memory card screen's own timing is fussy as well:
step through the logos with screenshots rather than a fixed key sequence (`step.sh NAME keys...`);
after the opening movie (F8, Enter skips it) the load prompt comes up by itself.

**A stack of a stuck game** comes from Proton's own winedbg, through the wineserver - `gdb -p` gets
"ptrace: Operation not permitted" here: `WINEPREFIX=<scratch pfx>/pfx WINESERVER="$PROTON/files/bin/wineserver"
"$PROTON/files/bin/wine" winedbg` with `attach 0x<pid>`, `bt all`, `detach` on its stdin (`info proc`
lists the Windows pids). The mod's hooks are sibling calls, so a thread inside one shows the game's
call site, not the hook.
Keep the scratch copy and prefix on disk (`~/.cache/...`), not in `/tmp`: `/tmp` is a 32 GB tmpfs that
other tools fill, and the tool output itself goes there. `pkill -f` with the plain pattern matches
the shell running it - use `pkill -f "[g]amescope --backend"`.

**The Mercenaries runs the same way** (2026-10-07; `~/.cache/re3cc-test/run_merce.sh` and
`merctest.sh`): `RE3_MERCE.exe` in place of `ResidentEvil3.exe`. Its title (GAME START / RESULT /
EXIT): Enter; player select: Enter for Carlos. A ~27 s intro narration follows (`0xA61F5C` = 0x88,
0xA8), then the game opens the status screen itself (0x288, the letterbox bit still set), and the
run and its countdown start when that closes. On the default controls (type 0 of the remap table at
0x517DAC) **V** is cancel and closes the status screen, **Z** (raw Start) opens it, Enter is Square.
**Send Escape only to the skip prompt**: anywhere else it brings up a Windows "Are you sure you want
to quit?" box that swallows every key after it. And move the pointer before clicking the panel: it
forgets the mouse while it is hidden, so a click with no move first lands nowhere.

**A state no headless run can play to takes a test build** (2026-10-08, the missile at the end).
1. Copy the repository into the scratchpad and add a hook at the top of `cheats::tick` there. The
   hook reads a command file (`~/.cache/re3cc-test/game/poke.txt`, opened through Wine's `Z:` drive;
   deleted once read), writes the fields the state needs, and logs what to watch once a second.
2. Build it there and put it in the scratch game in place of the harness's `ddraw.dll`. Put the
   harness's DLL back afterwards; the test code never goes into the repository.
3. Anything the gameplay loop does can run in the **attract demo**. Its session starts by itself
   some 25 s after the title and again about every minute; wait for "game session started".
4. Each demo opens with a black-screen narration during which the gameplay loop does not run. Wait
   for whatever you watch to move before testing.

Both times this was tried, Return on the title's LOAD GAME was followed by the demo, not the load
screen. `step.sh NAME keys...` sends keys and takes a screenshot; the file only appears a moment
after it returns.

**⛔ While the user has the game open, test nothing headless.** A second gamescope cannot start then
(`vkCreateDevice failed (VkResult: -3)`), and `:1` and `gamescope-0` are *the user's own* display
and socket: on 2026-10-07 a script whose headless run had failed went on to type six keys into the
user's Mercenaries run and screenshot it three times. Every script in `~/.cache/re3cc-test` sources
`guard.sh` since: `run.sh`/`run_merce.sh` refuse while the game is open (and clear the last run's
log and `gs.log` first, so nothing stale passes for this run), and nothing sends a key or takes a
screenshot until our own headless gamescope is running and its log has named the display and socket
to use. Keep it that way in any new script.

**`vkCreateDevice failed (VkResult: -3)` with no game open is the GPU, not the game.** In the same
minute as that test's gamescope start (12:24 on 2026-10-07) the kernel logged Xid 51 (`BAD_TSG`) and
Xid 154 (recovery action PF FLR), and from then on every new Vulkan device failed with
`NV_ERR_RESET_REQUIRED` - `vulkaninfo` included - while the programs already running carried on.
Only a reboot clears that. Check `journalctl -k | grep NVRM` before blaming the scripts; there is no
software Vulkan on this host (no lavapipe) to test with in the meantime.

## ⛔ Rules

- **Never patch `ResidentEvil3.exe` on disk.** It is Enigma-wrapped, its code is compressed on disk
  (entropy 8.00 in every section), and the wrapper unpacks it in memory after our `DllMain` has run.
  `DllMain` only loads the original wrapper, patches import slots and starts a thread; nothing reads
  game memory before the unpack gate in `mem::looks_like_code`.
- **Never use absolute addresses in code.** Everything is found at run time by byte signature
  verified at the match, with globals read out of the matched instruction's operands and
  cross-checked between sites; the VAs in this file document build 21774525 (English) only.
  `tools/check_sigs.py` runs every one of them against the dump — run it before a launch.
- **Game memory only from the main thread** (the PeekMessageA hook's tick, `dispatch.cpp`). The
  panel flips atomics and posts requests; the tick applies them.
- **On Windows, no function of the mod's in a vtable another hooker reads** (The panel's hooks on
  Windows): the D3D9 device and its swap chains get vtables of their own, and the device answers
  `GetSwapChain(0)` itself.
- **Never call a game function.** Every feature is a field write, a hold, or a byte patch of a site
  whose bytes were verified first (`mem::Patch::prepare`); patches are reverted on toggle-off and on
  unload. RE1 needed one call (its inventory icon rebuild); RE3 has not so far - its status screen rebuilds
  its icons as it opens when its state's `+0x14` bit 0x40000000 is set, which is a field write, so an
  edit shows the next time the screen opens (not at once - see "The inventory").
- **The inventory editor writes only while the status screen is up** (`game::status_screen()`, which
  is also the only screen the panel is drawn on, so a request can hardly be made anywhere else).
  The typewriter's save screen copies the ten slots to `0xA3FC00` and the equipped-slot byte into its
  own frame as it opens and copies them back when it closes (`0x44DA6E` / `0x44DBC6`); an edit made
  meanwhile would be thrown away by that restore rather than corrupt anything. That screen is the
  memory card screen, another entry on the same task 1 (`game::card_screen()`, see "The memory cards
  and the save and load screen"), so it can never be up at the same time. The infinite-ammo hold deliberately runs on every
  tick, screens included: gating it on "no other task-1 screen is up" was tried and dropped, because
  the gameplay task is chained onto task 1 too (`Task_chain(1, 0x469400)` at 0x42C2E0), so that test
  may well be true throughout play and would silently turn the cheat off.
- Back up `english\saves\` before experiments that write saves.
- The user commits every repo themselves — do not `git commit`/`push` unless asked.

## Why ddraw.dll, and the launch option

The Steam exe is Enigma Protector-wrapped: its on-disk import table holds only the wrapper's loader
imports (one function per original DLL — `DDRAW!DirectDrawEnumerateA`, `DSOUND!DirectSoundCreate`,
`DINPUT!DirectInputCreateA`, `WINMM!timeGetTime`, `dshow!CoUninitialize`, …). Of those, `ddraw.dll`,
`dinput.dll` and `dshow.dll` ship with the game, and `ddraw.dll` is the one that ends up in front of
Direct3D 9, so it is the one worth standing in front of. GOG's `ddraw.dll` (base 0x18000000, the same
wrapper family as RE1's) imports `d3d9.dll!Direct3DCreate9` by name, has 14 named exports and never
refers to itself by name, so it runs renamed as `ddraw_orig.dll`; `tools/gen_proxy.py --from-dll`
thunks all 14.

**Unlike RE1, Proton does not load a native ddraw for this app.** Proton Experimental's `proton`
script sets `dlloverrides["ddraw"] = "n,b"` for 500810 (Arcanum), 4249100 (RE1), 4249110 (RE2 1998),
4249130 (Dino Crisis) and 4249150 (Breath of Fire IV) — 4249120 is not in the list. Verified on
2026-09-23: without `WINEDLLOVERRIDES="ddraw=n,b"` the game runs on Wine's builtin ddraw and the
mod's DLL is never loaded at all (no log file is even created). The launch option is therefore part
of the install on Linux, and `make install`, `INSTALL.txt` and the README all say so.

The override is also what makes the launcher's graphics settings count. `4249120_Launcher.exe` (.NET)
writes them to `english\dxcfg.ini`, which only GOG's wrapper reads, so on Wine's builtin ddraw its
"Windowed" choice does nothing and the game opens full screen. With the wrapper, "Windowed" and
"Set by application" both give a window the size of the desktop (`display=desktop`). For a resizable
window the user runs it nested in gamescope like RE1 (set up 2026-10-05):
`WINEDLLOVERRIDES="ddraw=n,b" __NV_DISABLE_EXPLICIT_SYNC=1 GAMESCOPE_ARGS="-W 1600 -H 900 -w 2560 -h 1440 -r 60 -S fit" ~/.local/bin/gamescope-launch %command%`.
The nested desktop is 16:9 so the wrapper pillarboxes and the panel has the bars to sit in; the
launcher shows in the same window first.

## The panel's hooks on Windows (src/adopt.cpp, overlay_dx9.cpp; not yet seen in game)
Brought over from RE1-1996CabbyCodes on 2026-10-08, before the first Windows test: this repository
was forked from RE1 on 2026-09-23, and RE1 got the fix on 2026-09-28 (its `CLAUDE.md` and
Fear3CabbyCodes' have the Steam overlay's log and `gameoverlayrenderer.dll`'s code).
- **The Steam overlay's hooks.** The 32-bit overlay hooks D3D9 by writing a jump into whatever
  function a slot points to at that moment. It keeps ONE saved original per hook and skips a slot
  that is its own detour already.
  - Inside its `CreateDevice` wrapper it hooks the new device's Release 2, CreateAdditionalSwapChain
    13, GetSwapChain 14, Reset 16, Present 17 and more.
  - In its GetSwapChain and CreateAdditionalSwapChain hooks it hooks the Present (3) of the swap
    chain handed out, **on every call**.
- **GOG's wrapper here** (`ddraw_orig.dll`, base 0x18000000; another build than RE1's, the same
  family) makes its device at 0x1801EEC0 (three more tries follow it, 0x1801EF27..). It then calls
  `GetSwapChain(0, &global)` (0x184739B4) at 0x1801F191 and 0x18020A00, and again after a Reset at
  0x1802269A, and presents only through that swap chain.
- **The crash.** With the mod's Present in the swap chain class's vtable (0.1.0's design, which Wine
  keeps), the overlay would take it for its original, while the mod's own way on is D3D9's Present,
  the overlay's detour. The two call each other at the first frame until the stack runs out. Under
  Wine, `tests/test_adopt.cpp` reproduces it on the class path with its stand-in overlay: the frames
  and Resets fail and the run hangs.
- **The fix, on Windows only** (`adopt::enabled()`: not Wine, or `RE3CC_ADOPT` in the environment):
  - The device made for **the game's window** gets a private copy of its vtable (192 slots) with
    Present, CreateAdditionalSwapChain, **GetSwapChain**, EndScene and Reset. The game's window is a
    window of this process made by the thread that loaded the mod. That is the main thread, which also
    makes the device and presents (one thread throughout in the Linux log of 2026-10-08).
  - Its implicit swap chain, taken once through the class's GetSwapChain, gets a copy (32 slots) with
    Present. The adopted device answers `GetSwapChain(0)` itself from then on, so the overlay never
    reads the copy.
  - Additional swap chains are adopted as they are made.
  - **Every IDirect3D9 the wrapper makes** (through its Direct3DCreate9 import) gets a copy too (32
    slots) with CreateDevice, because of the shim below.
  - The hooks call on through the vtable each object had, as it is at the time, and so does the
    cutscene pause's `present_still`. The classes' vtables are left alone.
  - Under Wine the class vtables are hooked as before.
- **Windows' compatibility shim for `ResidentEvil3.exe`** (found 2026-10-08, the first Windows run).
  Windows' compatibility database knows the exe by name, and the shim engine (`apphelp.dll`) sits in
  front of D3D9 for it:
  - The wrapper's Direct3DCreate9 import leads to `apphelp.dll` (+0x80DA0 on the laptop's Windows),
    not `d3d9.dll`.
  - Every call writes the shim's own CreateDevice (`apphelp.dll`+0x808C0) into `d3d9.dll`'s IDirect3D9
    class vtable, over whatever is there. That CreateDevice calls d3d9's own, not what the slot held.
  - The wrapper makes an IDirect3D9, releases it and makes another before its device
    (0x1801EC03..0x1801EC43; the second often at the first's address). 0.1.0's in-place CreateDevice
    hook (and RE1's, which this port first kept) was gone by then.
  - So the first Windows run never saw a device: no panel, no crash. The log had one "the wrapper
    called Direct3DCreate9" line and no device line.
  - Found with a probe in the laptop's desktop session (a scheduled task with `/IT`, a hidden window).
    Named `probe2.exe` it got `d3d9.dll`'s Direct3DCreate9 and kept a hook in the class's slot; renamed
    `ResidentEvil3.exe` it got the shim's, which overwrote the hook. `RE3_MERCE.exe` was not probed.
  - `tests/test_adopt.cpp`'s "shim" mode plays it: the class path fails as the game did, the fix
    passes.
- **Windows' D3D9 devices have vtables of their own** (in heap memory, one per device), unlike
  Wine's. IDirect3D9 objects and swap chains share their classes' vtables in `d3d9.dll`.
- **Exit.** DllMain does nothing on a process exit. The teardown that ran there before (ImGui's D3D9
  objects and DirectSound, released under the loader lock) is what hung RE0CabbyCodes on close on
  Windows (2026-09-25).

## What the game does

Everything below was read out of the mod's own dump of the unpacked image; there is no RE3 decomp to
lean on the way RE1 had one. Addresses are build 21774525 (English).

### Executable
- Enigma keeps the 1999 image's sections at their original addresses with blank names — `.text`
  0x401000 (vsize 0x109000), `.rdata` 0x50A000, `.data` 0x514000 (vsize 0x550000, `.bss` included),
  `.idata` 0xA64000, `.reloc` 0xA67000, `.rsrc` 0xA95000 — and appends its own from 0xA97000 (entry
  0x144FC0C, SizeOfImage 0x1053000). Image base 0x400000, no ASLR. The dump is those six sections.
- **The unpack gate had to be rewritten.** RE1's heuristic counts `55 8B EC` prologues and `CC CC CC
  CC` padding; RE3's 1999 build omits frame pointers and pads with `90`, so the first 256 KB of its
  unpacked code holds *3* prologues and 10 pads — RE1's gate never fires. `mem::text_stats` measures
  entropy instead: packed sections sit within a hundredth of 8.00 bits/byte, the unpacked code is
  6.49 with 14.7% zero bytes and 3161 intra-section `E8` calls. The gate is `entropy < 7.2` plus one
  structural test. Enigma unpacks within ~200 ms of the process starting.
- **The game's import table is at the front of `.rdata`, not in `.idata`.** MSVC put the IAT there
  (0x50A000..0x50A2CC, 179 slots, 169 filled) and the descriptors in `.idata`, which Enigma clears
  once it has bound them. `image::find_iat()` finds it by content: the longest run in `.rdata` whose
  dwords are addresses inside some other loaded module. Enigma leaves the **real** API addresses
  there, so the tick re-points the game's `PeekMessageA` slot by value, as RE1 does.
  `find_iat()` must run before anything asks for a slot — `dllmain` calls it right after the unpack.
- A cooperative task scheduler: `Task_chain(id,entry)`
  (0x42AF90) writes the entry to `0xA59128 + id*0x80` and marks `0xA59124 + id*0x80` runnable;
  `Task_sleep(frames)` (0x42AFB0) yields through the running task's block (`[0xA592A4]`);
  `Task_state(id)` is 0x42B0C0. WinMain's pump calls `PeekMessageA` every time round, which is where
  the mod's tick runs. **The tasks are real threads**, handed the CPU one at a time through events
  (0x413390 creates them with entry 0x4136C0; 0x4133F0 is the hand-off's `WaitForSingleObject`
  (INFINITE), where a stack sample finds the main thread almost always). The game's logic runs on
  one of them (entry 0x4237B0) - so game memory is still only touched while the main thread holds
  the turn (the tick), but **anything that paces the game happens on the logic thread**, not the
  main one.
- **Frame pacing is `timeGetTime` alone.** The logic thread spins on it (~130,000 calls a second)
  until the next emulated vertical blank is due; each frame (0x423A20) then waits on the vsync count
  `0xA629F0` reaching the divider `0xA628B4` (1 at 60 fps, 2 at 30) through 0x506E00, which runs the
  ten subsystems at 0x5349B8 (the pump among them). `Sleep` and `SetTimer` are not involved (the
  latter is the config dialogs'). A histogram of the hooked import's callers (2026-10-06) found one
  hot reader - **0x42097F**, the spin, millions of reads - and one other site with two reads in a
  whole session (0x420A01): no timeouts, no I/O waits. The emulated vblanks are also what the
  play-time clock counts: the spin raises a count at `+0x5AC` of the system object `[0x5349BC]`
  (0x42098B) each time the clock passes the next vblank due, and the PlayStation-style `VSync(-1)`
  (0x507F60, a call through that object's vtable slot `+0x78` to 0x420AE0) answers it. See "The play
  time".
- Input is **DirectInput**, not `GetAsyncKeyState` (that export is not in the table at all). The poll
  (0x42CAD0) publishes one frame's buttons as three dwords the whole game reads: `0xA5C85C` held,
  `0xA5C868` newly pressed, `0xA5C860` auto-repeating.

### Screens
- **Status screen** — the inventory/file/map screen, and the mod's equivalent of RE1's option screen.
  The gameplay task chains it onto **task 1** with entry **0x45E2F0** when bit 0x200 of the save
  block's flags dword is set (0x42B261); the entry calls the screen with its own state struct at
  **0xA35EE0**, whose `+4` says which page it is on. `game::status_screen()` is "task 1's entry is
  that address and its state word is not 0".
- **The save and load screen is the memory card screen** - the PlayStation game's `mem_card.bin`
  overlay, run as **task 1** with entry **0x44CEC0** from the title's LOAD GAME, the load prompt after
  the opening movie, and the gameplay loop when the state flags have 0x80000 (a typewriter's "yes",
  0x42B2C9). Its state struct is **0xA3FEA0**. `game::card_screen()` is "task 1's entry is that
  address, its state word is not 0, and the screen's pointer names its struct". See "The memory cards
  and the save and load screen".
- The results screen (0x445520) is where the play-time units were pinned down: it divides by 216000
  for hours, so the clock counts sixtieths of a second.
- **The title menu** is EASY MODE, HARD MODE, LOAD GAME, **EPILOGUE**, EXIT (0x450B80); EPILOGUE runs
  `epilog.bin` (overlay 10, entry 0x4423E0) on task 1. The mod draws nothing there. See "The clear
  count and the epilogues".

### Cutscenes (src/cutscene.cpp)
- **A scripted scene sets bit 0x8 of the save flags `0xA61F5C`.** It is the letterbox: 0x4246D0 ramps
  `0xA628BA` up to 0xFF while the bit is set and draws the two bars, and the status screen's gate
  (0x42B84F, `test eax,0x108` on the save flags) refuses to open under it. Watched in
  play: the new game's intro narration runs as 0x28, the opening in-engine scene as 0x88 (0x8A for a
  moment mid-scene), and both clear when control comes back. The stop flags `0xA5CBD0` (the scripts'
  flag group 2 - `0x52B784` is the script engine's group table: 0 = `0xA5C880`, 1 = `0xA61F5C`,
  2 = `0xA5CBD0`, ...) carry 0x1000000 during the opening scene; their top bits are what freeze the
  player (0x80000000 skips the player's update in the gameplay loop) and the enemies (0x20000000).
- **Movies (FMV)**: 0x42F600 starts one from the 0x18-byte table at 0x51B978 (`opn`, `ins01..09`,
  `enda/endb`, `roopne`, `bdino`) and sets `0xA5C880 |= 0x18000`; each frame runs the movie player
  (0x42F7D0, state byte `0xA4FD54`: 0x42F830 open, 0x42FA20 play, 0x42FC40 close) while 0x10000 is
  set (0x423AC2). The play frame ends the movie when its end byte `0xA4FD5A` is non-zero: the stream
  running out writes 1 and `0xA4FD6D = 0` (0x42FB2F); the game's own skip (the movie's mask, entry
  +8, against the raw pad word `0xA5C858`, from frame 0x3D on) writes 2 and 2. **The mod ends a movie
  by writing 1 / 0**, exactly the ran-out path. The game's own skip did not answer Enter, Space or X
  headless; it was not pursued.
- **The message box** is the object at `0xA4FEA0`, run by 0x42DB30 while its flags (`+0x75C8`,
  `0xA57468`) have 0x80; its state (`+0x75CA`, `0xA5746A`) drives 0x42DB70: 1 prints, 3 and 6 count
  down on their own, **2 and 5 wait for a press, 4 is a choice** (0x42E060, the typewriter's yes/no
  and the live selections). The intro narration is timed message text (3, 1, 6, ...), so "a message
  is up" is not "the player is needed" - the skip waits only in 2, 4 and 5.
- **Voices** are WAVs (`/data_a/voice/` + a name from the room's table at `[0xA5C94C]`, or the
  intro's `S000A010..S001A130` / `M000A010/020` tables at 0x517350 / 0x517288), played by 0x427220
  through stream slot 0 of the sound object `[0x5349E0]` (methods: `+0x18` load, `+0x1C` play,
  `+0x30` status = DirectSound `GetStatus` & PLAYING). It sets the voice state `0xA59E51` = 1 and
  **bit 0x20 of the save flags** for as long as the line plays - the 0x20 in the narration's 0x28,
  and what a scene waits on. The state machine 0x4272F0 (table 0x5173F8): 2 runs until the slot stops
  playing (or a fade or the `0xA59E74 < 0xA59E70` check ends it), 3 stops the sound and goes to 0,
  which clears 0x20. **The game cuts a line short with `if (state) state = 3`** (0x42B69C, 0x448F07)
  - the mod does exactly that during a skip. That is what held the intro narration: its lines keep
  time with real audio, which no clock speed-up hurries.
- **The sound** is all DirectSound secondary buffers (the game's 22 stream slots, effects, music)
  except a movie's soundtrack, which GOG's `dshow.dll` (an 11 MB DirectShow stand-in that the game's
  COM calls go to) writes to a **waveOut** device. `DirectSoundCreate` is bound by Enigma without
  `GetProcAddress`, and the game calls it during the 2 s the mod waits before hooking, so its import
  is no use; but every secondary buffer of one DirectSound implementation shares a vtable, so
  `sound.cpp` makes one buffer of its own (DSSCL_NORMAL on the game's window), hooks Play/Stop/
  GetStatus/Lock/SetVolume/Release in that vtable, and so sees every buffer the game touches. One
  ~10 s ambience/music buffer (910,800 bytes in the opening alley) is played *non-looping* and
  re-`Play`ed by the game on every frame it is not playing - holding its Play back simply mutes it,
  and it comes back by itself.
- **The skip** (src/cutscene.cpp, src/sound.cpp, src/pad.cpp): the key (WndProc) or the pad button
  (XInput, read from the tick) during bit 0x8 or a movie asks on the next `PeekMessageA` - the mod's
  hook does not return until it is answered, which holds the game (the main thread never hands the
  turn on). While it asks: every playing buffer is `Stop`ped and the movie's waveOut
  `waveOutPause`d, the game's clock stands still (`hold_clock`; resuming takes the pause back out of
  it), and its own loop pumps the window and re-presents a copy of the last frame (a render-target
  texture filled by `StretchRect` in the present hook while a cutscene is up) with the prompt over
  it. "Keep watching" restarts what it stopped. "Skip": a movie is ended as above; a scene runs with
  **the clock moving 1 ms per read** (the spin is over at once, and as many vblanks pass as the
  frames take - the measured opening scene: 209 frames, 30 fps, clock +7,029 ms against 6,967 for
  209 frames - so play time moves exactly as it would have), every non-looping `Play` held back,
  paused one-shots left stopped and wound back, looping sound resumed, the voice cut with state 3,
  the game's presents dropped and the dimmed still re-presented with "Skipping…" every 100 ms. A
  page or choice (message state 2/4/5) puts everything back to normal until it is answered. The
  status screen ends the skip outright, and is never offered one: the Mercenaries' intro opens it
  with the letterbox bit still set (0x288) and drops the bit as it closes, so the scene is over by
  the time the player is done with it (`on_screen()` is "a scene, and not the status screen";
  `scene_running()`, the end test, leaves the screen in). Measured (headless): opening scene 209
  frames in ~256 ms; the intro narration after a pause / keep watching / skip, voice cut and all, in
  ~62 ms; the boot movie in 3-13 ms. It also stops on death, after 30 s at full speed and after
  9,000 frames (five minutes of the game's time). Before the status screen stopped it, the
  Mercenaries' intro ran into the 30 s cap behind it every time (145,996 and 139,920 frames, the
  screen black and the keys held; the play-time clock does not run in the Mercenaries - 0x80 skips
  its update - but in the main game that many frames would add some 80 minutes to it), and the frame
  cap bounds anything like it still unknown to a second or two.

### The save block
`FUN_0042C460` (0x42C460) is the load: `memcpy(0xA61F58, <card buffer>, 0x708)`, then it unpacks the
position, angle and HP that follow at buffer+0x708. So the live block is **0xA61F58, 0x708 bytes**,
and a save is the 0x980 bytes at 0xA61D58 written into a 128 KB PlayStation memory-card image
(`saves\bu00.sav` / `bu10.sav`: the game emulates a card — `bu00:`/`bu10:` + `.sav`, `mem_card.bin`).
The first 0x200 of those are the card file's own header (`SC`, the Shift-JIS title, the icon), so the
block - the play time first - sits 0x200 into the save's 8 KB card block: in a `bu00.sav` holding one
save (directory entry `BASLUS-0092300` at 0x80, block 1), the play time is the u32 at **0x2200**.
`saves\data.sav` is not a card: it is the Mercenaries' own profile file (see "The money"). `FUN_0044F0F0` fills
the block from the live state before a save, `FUN_0044F1E0` does the reverse.

| what | where | notes |
| --- | --- | --- |
| play time | 0xA61F58 (u32) | sixtieths of a second; the block's first field. **Only brought up to date as a save starts and as the ending starts** - see "The play time" |
| play time's base | 0xA5C944 (u32) | *not* in the block: the vblank count the total was last brought up to date at |
| save block flags | 0xA61F5C (u32) | 0x200 asks for the status screen, 0x400 the save screen |
| angle / x / y / z | 0xA61F64..0xA61F6A | copies of the live values, for the save |
| HP (saved copy) | 0xA61F6C (u16) | |
| **clear count** | 0xA61F6E (u16) | `+0x16`: the endings seen; each ending adds one, and the EPILOGUE screen unlocks up to it - see "The clear count and the epilogues" |
| save count | 0xA61F70 (u8) | +1 at 0x44DAE3, capped at 99; a new game sets it to 0xFF; the ending's rank is written over it (see "The ending's rank") |
| stage / room | 0xA61FA6 / 0xA61FA8 (u16) | `+0x4E` / `+0x50` |
| character | 0xA61FB6 (s16) | `+0x5E`: who the room loads put in play - Jill below 8 (1 with the backpack, 2..7 the boutique's outfits), Carlos 8; see "Jill's outfit" |
| Mercenaries seconds played | 0xA61FC2 (s16) | +1 with each second the countdown takes off, in the Mercenaries only |
| **countdown** | 0xA61FC8 (s16) | whole seconds left, the Mercenaries' clock and the main game's timed scenes'; see "The Mercenaries" |
| Jill's kept type | 0xA61FCC (s16) | `+0x74`: her character while another is played |
| per-room flags | 0xA6207C | 32 dwords, indexed by room (cleared at 0x431490) |
| **recovery total** | 0xA625FC (u32) | `+0x6A4`: the healing every item used has given, in full; the ending ranks on it |
| **outfit** | 0xA6262C (u32) | `+0x6D4`: Jill's costume, NORMAL 0 and SPECIAL 1..7, as the title's COSTUME SELECT sets it |

### The memory cards and the save and load screen (src/savefiles.cpp, the save file manager)
Traced 2026-10-08 from the dump, and run headless the same day (see the end of this section).
- **The cards are in memory.** The game's memory card container (0x40AC4 bytes, vtable 0x50A760,
  built at 0x403CA4) is the app object's `+0xFC`, copied into **`[0x5349D4]`** with the other
  subsystems (0x403EB3). It holds two card objects of 0x2055C bytes at `+0xC` and `+0x20568`
  (constructor 0x414190, vtable **0x50AE34**, destructor 0x4141D0): the name `"bu00:"` / `"bu10:"`
  at `+4` (the virtual at vtable `+0x10`, `lea eax,[ecx+4]`), **the 128 KB image at `+0x18`**, a
  firstfile/nextfile index at `+0x20018`, 16 file handles of 0x14 bytes from `+0x2001C` (in use,
  mode, directory entry, position, size), and the folder at `+0x2015C` - bio3.ini's `Save=`
  (`.\saves`), read at 0x403EFF and handed to both cards before the container's init (0x4140E0)
  loads them. A card reads its file once, at that init (vtable `+4`, 0x414230 → 0x4142D0: folder +
  name with `.sav` over the colon, fopen "rb", one fread of 0x20000, or zeroes when there is no
  file), and writes the whole image back (0x4143D0, fopen "wb") after every write (vtable `+0x28`,
  0x4144C0) and **from its destructor as the game closes** - so a file changed behind the game's
  back is put back the way the game has it at its next save or exit. The PlayStation calls the game
  makes (0x507CD0.. open, close, lseek, read, write, firstfile, nextfile, format: the system object
  `[0x5349BC]`'s vtable `+0x90..+0xAC`) end up in the card by device name. Nothing in the game
  deletes a save, and the card's one method past nextfile (vtable `+0x34`, 0x414C70) only answers -1.
- **The image** is a PlayStation card's sixteen 8 KB blocks, without the real thing's frame-0 `MC`,
  checksums or broken-sector list (all zero). Block 0's entries 1..15 (0x80 bytes each) are the
  directory: `+0` state (**0x51** a file's first block, **0** free; 0x52/0x53 later blocks of a longer
  file), `+4` the file's size, `+8` the next block (0xFFFF none), `+0xA` the name (up to 0x15
  characters); entry *i*'s file starts in block *i*. Creating a file (0x414640) takes the first
  entry from 1 whose state is 0 and writes 0x51, 0x2000, 0xFFFF and the name; a write past the size
  chains more blocks (0x414690).
- **The PC port only ever uses card 0** (`bu00.sav`). The screen's card question became
  "Load/Save the game" / "Do not ...", and its cursor skips the middle row (state 2 in 0x44CFD0), so
  card 1 is never chosen; `bu10.sav` is only ever written back as it was read.
- **A save** is a one-block file named **`BASLUS-00923` + the slot in two digits** (00..14, so 15
  slots). The game writes 0x980 bytes of 0xA61D58 (0x44F490: open with `O_CREAT | 1 block << 16`,
  then for writing, write, read 0x200 back): the 0x200-byte `SC` header - icon, block count, and a
  Shift-JIS title at `+4` built by 0x44F2F0, `ＲＥ３　NN．「<location>／SS－CC」　H` (slot number,
  location, save count, clear count when not 0, `Ｅ` for Easy) - then the save block (0xA61F58..).
  Nothing reads the title back. A load (0x44DC90) reads 0x980 bytes and copies 0x908 of them to
  0xA61D58.
- **The screen** is the PlayStation game's `mem_card.bin` overlay: overlay 2 in the table at
  0x517970 (`{name, entry, init}`; 0x42AF30 runs an init and `Task_chain`s the entry), entry
  **0x44CEC0** (runs while task 0's state is 1, suspending it), init 0x44CE30, loop 0x44CEF0. Its
  state struct, **0xA3FEA0** through the pointer `[0x528DB8]`, 0x12C0 bytes: `+0` state, `+2` mode
  (1 = load, under state flags 0x40000), `+4` cursor, `+0xE` card, `+0x10` scroll, `+0x20` the slot
  map (15 bytes: 0x80 empty, 0x40 "Other data", else a place in the list), `+0x34` the list of the
  card's files (0x16-byte names, each at the place the blocks before it give), `+0x17E` places,
  `+0x17F` saves of this game, `+0x180` bytes used, **`+0xB40`** 15 × 0x80 bytes of each save read
  from its file's `+0x200` (what the list draws: location `+0x1B` - names through the u16 table
  0x528D62 into the messages at 0x5287C8, 0..13 rooms, 0x1C "Next Game" - save count `+0x18`, clear
  count `+0x16`, Easy bit 0x1F of `+8`). The loop runs the handler `[0x528DFC + 4 * state]` (twenty)
  and then the draw `[0x528E4C + 4 * state]` for the state it ended in. States: 0 check the cards,
  2 the Load/Save menu, 3 an error box (back to 2), 4 one frame's sleep, **5 read the card**
  (0x44D470: clear the list, read it through 0x44EDD0 - listing 0x44EEA0, slot map 0x44EF70, the
  saves' bytes - then to 6, the cursor from the remembered `0xA5CBF1`), **6 the list waiting for the
  player** (0x44D5A0), 7 a slot picked, 8/0x13 the "Saving/Loading data..." waits, 9 the save
  (0x44DA60), 0xB the load, 0xE an error over the list, 0xF "Overwrite?", 0x10..0x12 what follows
  a save made under state flags 0x4000 (the ending's).
- **The save file manager** changes the card only while the list waits (state 6). A change is
  built on a copy of the game's image: Delete zeroes the save's directory entry and block (what a
  never-used slot is on these cards); Copy writes the source's block over the target's - into the
  first free entry, with a new directory entry, when the slot is empty - and rewrites the title's
  slot digits for the new slot. The card is mapped exactly as 0x44EF70 maps it, "Other data"
  included, and a card that read would misread (more places than blocks, a save named for slot 15+)
  is not touched; nor are two files claiming one slot, or a save that is not one block. Then **the
  file first** (temp file and rename, read back, put back on a mismatch), **then the game's image**
  (the entry and the block), **then state := 5**: the screen reads the card again in its next frame
  and is back on its list in the same frame (no flicker), the cursor where it was, the scroll worked
  out again. Every request carries a hash of what the panel saw of the slots it names.
- **Run headless, 2026-10-08** (SLR4, the scratch card): copy 1 → 3 showed at once in the game's own
  list ("3. Warehouse /00"; the log: state 6 → 5 → 6) and on disk (entry 2 `BASLUS-0092302`, title
  `３．`); delete 3 left the file byte for byte as it started; copy again, the game's cursor to 3,
  Enter: states 6 → 7 → 0x13 → 0xB, the session started from the copy; quitting through the game's
  own box rewrote both card files from memory, and `bu00.sav` matched the mod's file byte for byte.
  Then a typewriter's save mode (the scratch save loads beside the warehouse office's typewriter;
  Infinite ink ribbons on): copy 1 → 2 showed at once in the save list ("2. Warehouse /00"); the
  game's own save over it ("Overwrite?" - its cursor starts on No - Up, Enter: states 0xF → 8 → 9)
  wrote its save into the file the mod had made (`BASLUS-0092301`, title `２．…／０１`, 1 save), slot
  1 untouched, and the exit write matched again.

### The play time (src/game.cpp, src/cheats.cpp; the panel's "Freeze play time")
Worked out on 2026-10-07, after the user found a save made under the old freeze carrying more time
than it was frozen at. Not yet run in the game (see "What is not done yet").
- **The total at 0xA61F58 is not a running clock.** The game keeps the play time as the PlayStation
  game did: the total, and the vblank count it was last brought up to date at, **the base
  `0xA5C944`**. Three places touch the base, nothing else does: the session start (0x42C1E4, after
  both a load's `memcpy` at 0x42C09F and a new game's setup) sets it to `VSync(-1)`; **a save**
  (0x44DAE9 calls **0x44F0F0** first) adds `VSync(-1) - base` to the total and moves the base on -
  under state flag 0x80 (the Mercenaries) it does neither, under 0x4000 it clears the total instead;
  and **the ending** (task 0x442C80, outside the Mercenaries, calls **0x442CD0**) adds the same
  without moving the base, then sleeps five frames and plays on to the results, which read the total
  (0x445520, the rank 0x446103). Between those the total does not move, so **the play time is the
  total plus the vblanks since the base**, and every vblank counts: menus, the status screen, cutscenes.
- **The save writes the card in the same frame it adds**: 0x44DAE9 calls 0x44F0F0, then copies 0x80
  bytes of the block into the save screen's list (0x44DAFA) and packs the 0x980 bytes for the card
  (0x44F2F0) with no sleep between, so no tick can come between the add and the write.
- **`VSync(-1)`** (0x507F60): `[0x5349BC]`'s vtable slot `+0x78`, 0x420AE0, which for a negative
  argument answers the u32 at `+0x5AC` of the object. The object (0x8CC bytes, 0x403AA4) is built by
  0x410190, which calls the base constructor 0x4208E0 (vtable 0x510E00, the vblank fields zeroed) and
  sets its own vtable 0x50AC90; both have 0x420AE0 at `+0x78`. The pacing spin raises the count
  (0x42098B). `game::vblanks()` reads it straight from the object - once the object's vtable entry is
  seen to be the method found by signature - which is not a call into the game.
- **Freeze play time** NOPs the two stores to the total (`89 15 <0xA61F58>`, six bytes each: the
  save's at 0x44F12B, the ending's at 0x442CE9). The save still moves the base on. The tick also holds
  the base at the present while it is on, so turning it off counts nothing from the frozen stretch;
  turning it on mid-game first does what a save would (`settle_game_timer`), so the time up to then is
  kept. A load or a new game brings its own total and the freeze holds that. The editor writes the
  total and moves the base to now, so the time since the last save is not added on top of the new value.
  The panel shows `total + (vblanks - base)` (just the total in the Mercenaries).

### The ending's rank and the recovery total (src/game.cpp, the panel's "Recovery")
Found 2026-10-07 from the dumps; not yet run in the game (see "What is not done yet").
- **The rank** is worked out once, by **0x446090**, from the results screen's init (0x4457B9; state
  struct 0xA456C0, whose `+0xC` 0x445700 fills with the save count first). Three scores out of 100,
  each ten steps of `cmp` / `ja` / `mov` with the totals going down, so the last test that holds wins:
  - saves (`0xA61F70`): 0 → 100, 1 → 90, 2 → 80, 3 → 70, ≤5 → 60, ≤8 → 50, ≤12 → 40, ≤17 → 30,
    ≤23 → 20, ≤30 → 10, more → 0;
  - play time (`0xA61F58`, sixtieths): up to 2:29:59 → 100, 2:39:59 → 90, 2:59:59 → 80, 3:29:59 → 70,
    4:09:59 → 60, 4:59:59 → 50, 5:59:59 → 40, 7:09:59 → 30, 8:29:59 → 20, 9:59:59 → 10, longer → 0;
  - **the recovery total `0xA625FC`** (u32, save block `+0x6A4`, read at 0x446182): up to 1,500 →
    100, then 10 less for every 200 more, 3,300 → 10, more → 0.

  The sum (state `+0xE`) sets the rank (`+4`) to 1..5 from 50, 120, 180, 230 and 270 points. The
  results screen (0x445520) draws "Time", "Number of Saves" and - unless the state flags have 0x100,
  Easy - "Grade", the rank through `%c` from **"FEDCBA" at 0x52635C**: so grade A is 270 or more and
  F under 50. The rank function then writes the rank over the save count (2 on Easy) and hands
  **0x405EE0** a letter from its own list, "EDCBAS", which records the clear under `[Data]`,
  `Complete`: one value for its 'S' or 'A' (grades A and B), another for anything else, and never
  takes the first back down. That key lives in `saves\data.sav` like the rest (the user's file has
  it): the PC port's profile stubs 0x5096D0 (read) and 0x509700 (write) hand every
  Get/WritePrivateProfileStringA the path 0xA63D90 (`.\saves\data.sav`), whatever the call passed -
  0x534980, which this file once took for the settings ini's path, is only the empty default.
- **The healing** is the status screen's own (item kind 3 → 0x4DA6CE, states through 0x4DA9B4). For
  ids 0x20..0x2A (table 0x4DA9C4) it puts the amount in the screen state's `+0x35`, a byte worked out
  from the maximum HP `0xA5CCC2`, and the cure in `+0x36`: the whole maximum for the spray (0x20), G+R
  (0x26), G×3 (0x27), G+R+B (0x29) and the aid box (0x2A, while it has uses); half for G+G (0x24) and
  G+G+B (0x28); a quarter for a green (0x21) and G+B (0x25); nothing for a blue (0x22, the cure only);
  a red alone (0x23) only gets message 7. Once the animation is far enough on (0x4DA80C, `+0x32` past
  0x50) it cures the poison (`and [0xA5CCC6],0xFDFF`), adds the amount to HP (then holds HP to the
  maximum) and **adds the same amount to the recovery total (0x4DA864)** - all of it, not what was
  missing. That store is the total's only writer in the code (the two indexed accesses at
  `[reg+0xA625FD]`, 0x426354/0x4264A4, are the Mercenaries records loop: the index starts at 0x13, so
  they reach 0xA62610 on). It is inside the 0x708 bytes a save carries, and a new game loads
  `Init_tbl.dat` (`Rofs3.dat`, 0x908 bytes unpacked) over 0xA61D58..0xA62660, which holds 0 there
  (read with the archive format below).
- **Heal without counting** NOPs the store (`89 2D <0xA625FC>`, six bytes; the `jle` after it tests
  the `cmp` before it). **The editor** writes the total from the tick, held to 0..99,999
  (`game::kRecoveryMax`); the panel scores it with the rank's own steps, which discovery reads out of
  the "rank: recovery" signature (`game::recovery_scale`) - except in the Mercenaries and on Easy,
  where it says there is no rank to score it for.
- The Mercenaries exe has both (healing 0x4DA8F7, rank 0x4461D2) and its healing adds to the total
  too, but nothing ranks on it there: its results rank the takings.

### The clear count and the epilogues (src/game.cpp, the panel's "Clears & epilogues")
Found 2026-10-08 from the dumps, when the user asked whether setting the playthrough count to 8 would
unlock every epilogue at the next ending; not yet run in the game (see "What is not done yet").
- **The count** is a u16 at **0xA61F6E**, save block `+0x16`: the endings seen, 0 on a first
  playthrough. The save list draws it (`+0x16` of each save's 0x80 bytes) and a save's title spells it
  out (0x44F3D9: two digits, a fixed string from 100 on).
- **The ending** is `ending.bin` (overlay 9, entry 0x442C80; outside the Mercenaries it runs
  0x442CD0). The gameplay loop switches its own task to it when the state flags have **0x4000**
  (0x42BC31, `0x42AF60(9)`). In order:
  - the play time settled, the ending movie (`enda` or `endb` by flag 0x58 of 0xA61FD0, 0x442E30),
    the staff roll (overlay 11) and the results (`result.bin`, 0xA5C875 = 3);
  - **while the state flags lack Easy (0x100) and the count is under 8** (0x442D97), `result.bin`
    again with **screen 5, the epilogue** (0x445A60): "EPILOGUE" and picture *count* of
    `data_u/etc2/ombg_u.dat` (320x240 TIMs, `{sector, size}` table 0x5261A8), which also sets bit 5
    (from the top) of 0xA61F60. When the count is 7, a press shows **picture 8** after it, loaded
    through the port's stub 0x509750;
  - 0x442EC0: the save count := 0xFF, the location := 0x1C ("Next Game"), and **`inc word
    [0xA61F6E]`** (0x442EEB), whatever the difficulty or the count;
  - state flags |= 0x4000, and **0x4262A0 writes the profile**: key 10 := the count, whenever it is
    not 0. Nothing compares, so finishing an older save with a lower count takes data.sav's down. The
    "Next Game" save the game offers after that carries the new count.
- **The EPILOGUE screen** (the title menu's fourth item, 0x450C8E → title state 0xD, 0x450FD0 →
  `epilog.bin`, overlay 10, entry **0x4423E0**, with 0xA5C875 = 1) first **reads data.sav's key 10
  into the count** (0x442483; only when the key is there and not negative). Then a menu of nine cells,
  eight epilogues and EXIT (positions 0x5260FC; state struct 0xA456E0: `+0` the state, handlers
  0x526120 - 0 fade in, 1 the menu 0x4426B0, 2 an epilogue, 3 out - and `+5` the cursor). **An
  epilogue opens while the cursor is under min(count, 8)** (0x4426BC); the rest buzz, and 0x442C0C
  draws the lock over cells count..7. The picture is *cursor* of `ombg_u.dat` (table 0x5260A0,
  0x442C50).
- **A new game** reads data.sav (0x426380, at 0x45137B) after `Init_tbl.dat` and the opening movie
  (or, from a "Next Game" save, the values that save carried: 0x426FA0 under state flag 0x20), so key
  10 overrides the count there too, before the new game's inventory (0x45D750) uses it. From 1, the
  starting inventory leaves out Game Inst. A/B (0x83/0x84, 0x45D8AA), and file bits 0 and 0x1C of
  0xA6215C are set with a file count 0xA62160 of 2 (0x45DA58, not in the Mercenaries). LOAD GAME
  does not read data.sav: a loaded game has the save's own count.
- **Nemesis's drops** (room script opcode 0x3B, 0x46C030; none on Easy): drop *n* is item *n* of
  0x52B718 - EAGLE Parts A, EAGLE Parts B, F. Aid Box x3, M37 Parts A, M37 Parts B, F. Aid Box x3,
  Inf. Bullets - except that **the seventh is an Assault Rifle (auto) with 100 rounds while the count
  is 0** (0x46C4E0; 0x46C32E puts a drop left lying in a room back the same way).
- The Mercenaries exe has the same code (the ending's test 0x442DE7, the increment 0x442F10, the menu
  0x442700) but never ends a game. It reads data.sav as a run starts and writes the count back after
  every run's results and every purchase (0x4270D0; 0x426F30/0x426FA0 carry it across the save
  block's reload).
- **data.sav's values** decode as 0x406060 does (a read-only Python decoder was written for this and
  not kept):
  - ten bytes from the 20 hex digits, the first digit of each pair the low nibble;
  - a checksum `b6<<24 | b1<<16 | b4<<8 | b9` (those four then zeroed), XORed with `b[i] << 3i` over all
    ten, must come to 0x2D8B50D2;
  - b2, b3, b5 and b8 are XORed with the 2nd to 5th draws of the LCG at 0x41F7E0 (`s = s * 0x5D588B65
    + 0x8000000B`, the draw `(s >> 16) & 0xFF`). Its seed is `(0x63A0 >> (15-k)) ^ (b7<<8 | b0) ^
    ((0x63A0 << k) & 0xFFFF)`, with k = key % 16;
  - the value is `b3<<24 | b5<<16 | b2<<8 | b8`. The encoder (0x406270) seeds b0/b7 from `timeGetTime`.

  The user's file of 2026-10-08 decodes to Data10 = 1, Data00 = 983,864 and Data01..04 = 8, 7, 6, 9.
- **The editor** (`game::set_clears`, from the tick) writes the count, held to 0..99
  (`game::kClearsMax`). It is the main game's only: the panel leaves the section out in the
  Mercenaries, and the tick refuses there. The panel says what the ending will make of the count. So
  **7** before the ending shows the eighth epilogue and the extra picture and records 8, and **8 or
  more** records 9+ with no epilogue at the ending; either way the EPILOGUE screen has all eight from
  then on. The user asked for a UI on the EPILOGUE screen only if that did not hold, so there is none.
  Unlocking them without an ending would mean writing data.sav's key 10 (the encoder above), since the
  screen reads it every time it opens.

### The player and the other entities
- **Player entity 0xA5CBF4.** `FUN_00463830(hit, mode)` is the damage routine: `HP -= hit` (+10 for
  a hit of 10..100: 0x4254F0 says no only to a Japanese build (the region byte 0x425530 returns,
  `'u'` here) with `0xA62620` clear), then in mode 0 a result of -10..-1 is held at 0 with bit
  0x800 of the condition word set (it answers 1, dying), and anything further under - or any hit while
  that bit is already set - sets `0xA5C880 |= 0x20000` there and then (it answers 2, dead). Mode 1
  only holds at 0 with the dying bit; any other mode answers the mode. The callers put the player's
  state `0xA5CBF8` to 3 (dead) on a 2. **So one hit of more than HP + 10 - 211 at Jill's full 200 -
  kills in the frame it lands**, and a hold that puts HP back between frames cannot stop it: a barrel
  going off at point blank did exactly that through God mode's old hold (2026-10-07, the
  Mercenaries). Every hit on the player comes through this routine: some fifty call sites
  (0x46FCCE..0x4CDD46) and **the blast walk 0x454EF0** (explosions: the entities inside the
  descriptor's box around the blast - the player and partner only when its first byte has 0x80 - the
  player through here with half the descriptor's damage, the partner through 0x4E33B0, the room's own
  straight off `+0xCC`). God mode NOPs the `sub cx,ax` (0x46385A): HP is stored back unchanged and
  the answer is 0, so the hit still plays (the callers looked at set the hurt state 2 before the
  call) but takes nothing. Deaths that do not ask it: 0x495F69 (state 3 when HP < 0 - never, with HP
  held), the room-script opcode 0x28 (0x46F2A0, opcode table 0x51BF50), which sets the death flag
  outright for a scripted death and which God mode leaves to the game, and 0x4E2BB1, the death flag
  again from an entity routine (0x4E2B40, reached through a table) not traced.
  - HP **0xA5CCC0** (s16), max HP **0xA5CCC2** (s16, 200 for Jill — set side by side at 0x42BFB4 on a
    new game), condition **0xA5CCC6** (u16: 0x200 poison, 0x800 dying).
  - Position 0xA5CC28/2C/30, angle 0xA5CC62. The low byte of **0xA5CC3A** is the weapon in the
    player's hands (an item id; the equip at 0x46BCF2 and the status screen's close at 0x45E475
    copy the inventory block's +0x129 into it, then reload the model through 0x452620/0x452310).
- **`0xA5CDC4` is an array of entity pointers**: [0] the player, [1] its partner, [2..] the room's
  own. `[0xA5CE6C]` is one past the live end (the walk at 0x42ABFB..0x42AC56 runs from index 2 to
  it, and 0x42AC4D's `lea eax,[ecx*4+0xA5CDC4]` names the base). Every entity keeps HP as an s16 at
  **+0xCC**, its type at +0x4A. One hit kills holds each of them at 1; the player is never in range.
- **`0xA5C880` is the game state flags word**: 0x80 is the Mercenaries (`RE3_MERCE.exe` sets it from
  its title on), and it and 0x4000 turn the ink-ribbon cost and the save counter off; 0x100 is Easy
  (the new game's inventory setup, 0x45D750, gives the Easy start - assault rifle, first aid box -
  under it, and every reloading-tool batch is doubled); 0x200 is the
  Mercenaries' time running out, 0x20000 is death, 0x8000000 is set once a session is running. The
  game often tests these through its bit test 0x426AB0 (`bit n` of a dword array, **counted from the
  top bit**: bit 0x17 is 0x100, bit 0x18 is 0x80).

### Jill's outfit (src/game.cpp, src/cheats.cpp; the panel's "Outfit")
Found 2026-10-08 from the dumps and the archives, when the user asked for a way to change outfits in
the middle of a game; run headless the same day (the end of this section), then in play by the user.
- **The character** is an s16 at **0xA61FB6** (save block +0x5E; the scripts' array 0xA61F76, index
  0x20): what the room loads put in play. Jill's own types are 0 and 1, and **type 1 is Jill with the
  backpack**: `pl01.pld` is the regular outfit with its straps, and a room load that brings type 1 in
  sets the shared block's usable slots to 10 (0x468699; a new game gives 8 on Hard, 10 on Easy,
  0x45D813). R20C's and R310's scripts set the character to 1, and the boutique's goes back to 1
  rather than 0 when flag 3/0x20 is set (the top bit of 0xA61FD4) - the backpack, it would seem; not
  traced further. Types 2..7 are the PlayStation boutique's outfits, **8 is Carlos** (the first that
  is not Jill, as in the inventory select 0x45EC80), 9 and 10 the other mercenaries. **0xA61FCC**
  (+0x74, index 0x2B) keeps Jill's type while another character is played. The loaded model's type is
  the player entity's **+0x4A, 0xA5CC3E**; a save carries it (0xA61F71, 0x44F155) and a load puts it
  back (0x42C504, 0x44F1FD).
- **The outfit** is a u32 at **0xA6262C** (save block +0x6D4, so a save carries it), the PC port's
  own. The title's main-game EASY and HARD (title state 1, 0x451050) go to **state 12, the COSTUME
  SELECT grid** (0x4513B0: 2 x 4 cells, the cursor from 0). Its cells are sprites out of
  `data_au/etc2/titleu03.tim` (`Rofs2.dat`; the PC title sheet, `bio19/Data_aX/Etc2/TitleX03.tim`
  with the language letter), labelled **SPECIAL 1 .. SPECIAL 7, NORMAL** (the sprite records from the
  title state's +0x11C on, set at 0x44FEC7; u 0/0x40/0x80/0xC0, v 0xC0 and 0xD0, 46 x 12). A pick hands
  `[0x5114C4 + 4 * cell]` - {1, 2, 3, 4, 5, 6, 7, 0} - to the setter 0x4254C0 and starts the game
  (0x4510A0), and a new game keeps the value across `Init_tbl.dat` (0x451291 / 0x4512B7). So **NORMAL
  is 0 and SPECIAL n is n**. Nothing else writes it, and the game describes none of them: no text in
  the exe, the archives or the launcher says more than those labels.
- **The model loader 0x45F880(entity)** builds `bio19/data/pld/pl%02x.pld` (0x52A1F0; the digits at
  0x52A201/0x52A202, both written afresh on every call) from the entity's type, except that for types
  0 and 1 the outfit (the getter 0x4254D0) picks: 1..5 → `pl02`..`pl06` (`add al,'1'`), 6 → `pl00`
  with `data_a/pld/Pl000.tim` (`Rofs6.dat`) over its texture, 7 → `pl02` with `Pl006.tim` (uploaded to
  VRAM 448,256, CLUT row 481, 0x45F9D3); 0 and anything over 7 the type's own. What they look like
  (their textures rendered from the files, and seen in the game): pl00 Jill's tube top and skirt, pl01
  the same with the backpack, pl02 biker leathers, pl03 her S.T.A.R.S. uniform from the first game,
  beret and all, pl04 a white trouser suit with a red collar, pl05 a police uniform, pl06 Regina from
  Dino Crisis (red hair, her tactical gear), Pl000.tim Normal with a black halter top, a denim jacket
  and a blue skirt, Pl006.tim the biker with a blue sleeveless top. `pl07.pld` is `pl00.pld` byte for
  byte. The weapon files are `pl00wXX.plw` for every Jill type (0x452310), so the outfit leaves them
  alone. Jill's cutscene doubles (EMD 0x5B/0x5F) follow the outfit too, picked as a room loads
  (0x4382D0: 1..5 → EMD 62..66, 6 → 70, 7 → 71; the script's spawn 0x46ABA0 takes type + 0x60 for a
  boutique type).
- **The model is loaded** only as a session starts (0x42C18B, after the room load, whatever the type)
  and by **the room load 0x4684D0** (from the session start, 0x42C172, and the room change, 0x42C3CE)
  when the model's type byte is not the character (`cmp dx,cx` / `je` at 0x468613/0x468616, past the
  load). That load: from Jill to 8 or more, HP 200, the poison cured and Jill's type kept; back from 8+,
  the kept type becomes the character; Jill to Jill, slots 10 for type 1, and a kept type of 2 or more
  put back. Then save flag 0x8000000 (another character in play) set or cleared, 0x42A5B0(0, 2 or 8,
  ...) (the character's own data, not traced), the type byte := the character, max HP := 200,
  0x45EC80, 0x45F880, and the player's state 0xA5CBF8 := 0, so its animation starts afresh on the new
  model; the `je` lands just after, at 0x468731.
- **The panel's change** (`game::set_outfit`, from the tick; refused in the Mercenaries) writes the
  outfit, and a boutique type in the character or the kept type goes back to 0 (the boutique's own
  script sets the two together). While Jill is in play it then marks the loader's file name (0x52A201
  := '#', which nothing reads before the loader writes '0' over it) and takes out the room load's `je`
  (six NOPs): the next room load takes the path above with only the model to change, and the tick puts
  the `je` back once the mark is gone. With Carlos in play there is nothing more to do: going back to
  Jill is a change of type. Loading a save brings that save's own outfit, which the session start loads.
- **The PlayStation boutique** is still in the rooms: R10F's script sets the character and the kept
  type to 2..6 (or back to 0/1) and fires a door into R10F itself (stage 0, room 0x0F, door animations
  0x3E..0x42), so the room load sees a change of type. Its key (the Boutique Key, 0x80) is in no
  starting inventory of the PC port, and `jill_sel.bin` (overlay 7, 0x446300) is the reward screen
  after the ending ("YOU'VE GOT THE BOUTIQUE KEY AND N COSTUMES", bits 0..4 of 0xA61F60 by the save
  count). Every door plays an animation file (`data/door/doorXX.do2`, 0x469E80; there are 78, none
  blank), so a reload in place, as the boutique does it, would show a door wherever Jill stood.
- **Run headless, 2026-10-08** (a test build: the requests through `request_outfit`, each room load
  forced by a door record of the player's own position in `[0xA5C8B8]` and `0xA587A0 = 1` - the
  test-only warp): from the scratch save in the warehouse office, SPECIAL 1, 6, 3 and 5 were each worn
  after the room load (the log: the patch in and the mark, then the loader's digits `02`/`00`/`04`/`06`
  and the patch out), and Jill walked normally after. The user then changed outfits in play.

### The inventory (src/inventory.cpp)
`FUN_0045EC80` picks whose inventory is in play: character 8 has one of his own, everyone else shares
the first. `PTR = 0xA623E4 = 0xA62164 + [0xA623E8] * 0x140`. Inside that 0x140-byte block:

| what | offset | notes |
| --- | --- | --- |
| ten slots | +0x00 | four bytes each: **id, count, flags, spare** (0x46F212 reads id at +0, count at +1; 0x4D9E28 tests bit 0x10 of +2) |
| **item box** | +0x28 | **64 slots** of the same shape. The box screen (0x4E4000..0x4E5900) draws them as a ring - the cursor less three, `+0x40` when negative (0x4E5751) - and swaps a whole slot with an inventory slot (0x4E4904) |
| equipped slot | +0x128 | a **slot index**, 0xFF for none (0x4DA649 writes the cursor there) |
| equipped weapon | +0x129 | the equipped weapon's **item id**, 0 for none. Every equip writes the pair: 0x4DA643, 0x46BD22, and the box screen clears both (0xFF/0) when the equipped weapon is stored (0x4E4810). *Not* the character, as this file once said |
| usable slots | +0x12A | how many of the ten this character may use (`find_item_slot`, 0x4DE9D0, stops there) |
| **batches made** | +0x12C | four u16s: the reloading tool's count per kind of ammo - handgun bullets, shotgun shells, magnum bullets, grenade rounds. See "The reloading tool's proficiency" |

The box is per block, so Carlos's box is his own. The box screen combines two slots only when the
ids match and the kind is 2 (ammo), 5 or 6 (keys hold one, so in practice ammo, ink ribbons and the
reloading tool) and the sum fits the table's maximum, or a First Aid Spray with a First Aid Box
(0x4E4859..0x4E48CB); everything else is swapped. The mod's moves follow that: a stackable item tops
up stacks of the same item first, anything else takes the first free slot.

**The inventory is packed**, as RE1's is. `remove_item_slot(i)` (0x4DEA60, called when an item is
used up through 0x4DEA10) moves slots i+1.. up one, zeroes the last usable one, decrements the
equipped slot if it was after i (or the status screen's saved copy at state `+0x2F` while `+0x2E` is
set), and sets **0x40000000 in the status screen's state `+0x14`**. The mod's emptying and storing go
through `game::remove_slot`, which does the same.

**The status screen's icons are in (emulated PlayStation) video memory**, uploaded per slot by
0x4DCCE0 (`0x425330` copy + `0x423200` LoadImage) only as the screen opens (0x45DD8B), and only when
that `+0x14` bit asks for it - it then clears it. While the screen is up the game moves icons itself
with calls (0x4DCE20, 0x4DCDD0/0x4DCBF0). Writing the slots alone therefore leaves stale icons with
no count and no description, *even across reopenings* (the first inventory editor did exactly that).
Every inventory write sets the bit (`game::mark_icons_stale`), so the next opening is right; a live
redraw while the screen is up would need a call into the game and is not done.

**Equipping goes through the status screen.** It saves +0x129 in its state struct (`+0x28`, at
0x45DE78) when it opens and, when it closes (0x45E45D), copies +0x129 into 0xA5CC3A and reloads the
weapon model if it changed. So an edit made while the screen is up only has to write +0x128 and
+0x129 as a pair (`game::set_equipped`). One trap: for an item that needs both hands the screen
puts the weapon away - state `+0x2E` = 1, the pair saved at `+0x2F`/`+0x30` and cleared (0x4DA8DA) -
and gives it back the next time it opens (0x45DD12); the editor refuses equip changes while `+0x2E`
is set rather than have them undone.

The status screen loads ten icons from `PTR + i*4` (0x45DBFA) and the equipped marker from +0x128.
`find_item_slot(id, from)` at 0x4DE9D0 is the game's own "which slot holds this", and both ink
ribbon tests go through it: the typewriter's and the save's.

**The typewriter** is a script command (entry 0x45B940, continued at 0x45B980 through
`0xA61D48`): `find_item_slot(0x81)`, and without a ribbon (`jl` at 0x45B9F7) it shows the message that
offers no save; with one it asks (0x42DA10 with 1) and a "yes" sets 0x80000 in the state flags,
which opens the save screen. The save screen then takes the ribbon at 0x44DA9F - unless the state
flags hold 0x4000 or 0x80. Infinite ink ribbons patches both: `7C 1A` → `90 90` at the typewriter,
`75 20` → `EB 20` at the take.

**The item table is 0x52AC50**, four bytes per id for ids 0..171: `+0` kind (1 weapon, 2 ammo,
3 heal, 4 other, 5 key, 6 ribbon/reloading tool/game instructions, 7 file, 8 map - `items::Kind` has
the same values), `+1` how many of it one slot holds, `+2` the name to
show once it has been examined, `+3` which ammo a weapon takes. The pickup at 0x46F212 caps a stack
with column +1; the name lookup at 0x42F292 does
`name = 0x51AF10 + u16[0x51B7EC + id*2]`, swapping `id` for `table[id].b2 - 0x55` once the "examined"
flag (checked against 0xA62078) is set.

**The game's text is stored shifted**: every character is its ASCII code minus 0x24, 0x00 is a space,
0xEA introduces a glyph from a second set (`S`,`T`,`A`,`R`,`&`), and 0xF7/0xFE/0xFF are separators.
A few punctuation slots do not follow ASCII: 0x01 is a full stop, 0x18 a comma, 0x1A an exclamation
mark, 0x3A an apostrophe, 0x3B a hyphen, 0x09/0x0A the quotation marks. `src/items.h` was generated
from the table and the name blob with that decoding; the same trick reads every item description and
every in-game message out of `.data`.

### The reloading tool's proficiency (src/game.cpp, the panel's "Reloading tool")
Found 2026-10-07 from the dumps; the editor has not been run in the game yet (see "What is not done
yet").
- **The combination table is 0x52AF00**: eight-byte entries `{type, item, item, result, count, 0,
  0, 0}` to the first whose first item is 0xFF. The lookup 0x4DCB50 takes the two items in either
  order and answers the first match. The status screen's combine (0x4DAD2E, `[entry]` less 2, less
  3) sends **type 2 to the reloading tool's batch** (0x4DAF1C) and **type 5 to powder mixed straight
  into grenade rounds** (0x4DADFD). In the data, type 0 stacks and loads (a gun and its ammo, two
  stacks, the first aid box and a spray), 1 is a plain combine (herbs, oils, the powders into AA..CCC,
  the gun parts), 3 loads a gun with the other grade of ammo (SIGPRO E + bullets = the plain gun),
  4 swaps a grenade launcher's loading, 6 is a gun + Inf. Bullets - read off the table, not traced.
- The tool's recipes (type 2, the tool first): A → 15 H. Gun Bullets, AA 35, AAA 55, **BBA 60
  handgun bullets**; B → 7 Shotgun Shells, BB 18, BBB 30, **AAB 20 shells**; C, AC, BC, CC → 10
  grenade, flame, acid, freeze rounds; CCC → 24 Magnum Bullets. Every handgun and shotgun recipe is
  followed by the same two items again with the enhanced rounds (0x1E H.G. Bullets E, 0x1F S.G. Shell
  E, same count): the game's "yes" to enhanced moves on one entry (`add [eax+0x58],8`, 0x4DB74B).
  Type 5: grenade rounds + A/B/C → 6 flame/acid/freeze, AA/BB/CC 12, AAA/BBB/CCC 18.
- **The kind of ammo** a recipe makes is looked up in `{item id, kind}` pairs at 0x530D08 (to 0xFF)
  by the plain recipe's result (0x4DAB0A) and kept at the screen state's `+0x6A`: 0x15/0x1E → 0
  handgun bullets, 0x17/0x1F → 1 shotgun shells, 0x16 → 2 magnum bullets, 0x18..0x1B → 3 grenade
  rounds of every sort.
- **The counts are u16s at inventory block +0x12C + 2 × kind**: the batches of that kind made so
  far. Each block has its own (so Carlos keeps his), a new game clears them with the slots (0x45D9D8,
  two dwords from +0x12C), and they are saved with the save block. Nothing else in the code touches
  them: five accesses, all in the combine (0x4DAB59, 0x4DAF3D, 0x4DB027 indexed by kind; 0x4DAE0A,
  0x4DAE8F the grenade kind's directly).
- **The level table is 0x52B330**: a row of five `{threshold u8, bonus u8, 0, 0}` per kind - all four
  the same in this build: thresholds 2, 5, 10, 20 (and 250, never compared), bonuses 0, 1, 3, 5, 7
  tenths. The batch walks the row while the count is past the threshold, stopping at the fifth, so
  the **next batch after 0-2 made gets no bonus, after 3-5 +10%, 6-10 +30%, 11-20 +50%, 21 or more
  +70%**. The batch is `base + base*bonus/10`, one more when the remainder is 5 or over, in a byte;
  enhanced rounds (results 0x1E..0x1F) first switch on the bonus (jump table 0x4DB644): 0, 1, 3 → 1,
  5 → 3, 7 → 5 - the level below's. Then **state flags 0x100 (Easy) double it** (`shl` on the
  byte, through the bit test), and the kind's count goes up one (0x4DB027). Gun Powder A at the top
  level: 26 handgun bullets, or 23 enhanced.
- **Enhanced rounds are offered** (0x4DAB0A) for kinds 0 and 1 (`cmp al,1`) once the count is
  above 6 (`cmp word [...],6` / `jbe`): message 0xD, "Will you create the enhanced ammo?", and state
  5 (0x4DB720) waits for the box and reads the answer at 0xA61FB0 (0 = yes). Either way the count
  goes up.
- **Powder into grenade rounds** (type 5) uses the grenade kind's count (+0x132) and row
  (0x52B36C) by address, adds the bonus truncated rather than rounded, doubles on Easy and counts a
  batch; with 6 or fewer grenade rounds in the stack the yield is scaled by how many (0x4DAEA2).
- **The editor** (`game::set_mixes_made`) writes a count from the tick whenever a game is running:
  nothing copies the counts away the way the save screen does the slots, and the game only reads
  them in the middle of a combine, inside one frame. The panel sets a level as the fewest batches
  that give it, except level 3 of the two kinds with enhanced rounds, which it sets to 7 so the
  offer comes with it; it only names the kinds after discovery has checked the kind table files the
  ammo exactly as above. Its yields are the game's own arithmetic over the recipes the discovery
  copies out of the combination table.

### The Mercenaries (RE3_MERCE.exe)
- **On PC the Mercenaries is an exe of its own.** The launcher's Mercenaries button
  (`4249120_Launcher.exe`) starts `RE3_MERCE.exe` (`Bio3_PC_Mercenaries.exe` for Japanese) from the
  same `english` folder, so it loads our `ddraw.dll` exactly as the main game does - the user's own
  log of 2026-10-07 is how that came to light. Same engine, same Enigma wrapper: the six sections at
  the same places and every global at the same address, most of the code moved on by up to 0xE0
  (status screen entry 0x45E360, countdown tick 0x42B3E6). Every signature matches there once, with
  the same data operands; `check_sigs.py --any-build` runs them against its dump. The state flags hold
  **0x80** from its title on. The main exe has the same code behind its own 0x80 tests (`Init_sub.dat`
  loaded over the save block at 0x4511E4). The VAs below are ResidentEvil3.exe's.
- **The countdown is whole seconds**, an s16 at **0xA61FC8** (save block +0x70), drawn as `M'SS"hh`
  and in another colour under a minute. **The main game runs it too** (the next point). The gameplay
  loop (0x42B0E0, `esi` = the 0xA5A760 block, so
  the clock is `[esi+0x7868]` and the save flags `[esi+0x77FC]`) runs it while the save flags have
  **0x4000000** and not 0x2000000 (with that bit the radar runs instead, which counts the missile at
  the end in: see "The missile" below). Each frame:
  - 0x42B3D8: when the frame count **0xA58792** is past 29, or the clock was not running last frame
    (**0xA58798** is last frame's save flags), a second comes off (0x42B3EE, `dec word [esi+0x7868]`),
    the frame count goes back to 0, the hundredths **0xA58794** to 8, and in the Mercenaries the
    seconds played (0xA61FC2) go up one. That first-frame path is the only place the frame count is
    zeroed for a run;
  - below 0 the time is up: once nothing else is on screen the voice is cut, 0x4000000 cleared and
    the state flags get 0x200, and 16 frames on task 1 is handed screen 3 (0x42BC76: overlay 3,
    `diedemo.bin`, the game over);
  - otherwise, unless `0xA587A0`/`0xA587A1` pause it, the frame count goes up one and the hundredths
    round 8, 5, 2 (0x42B6EB) - 30 frames to the second; the hundredths are only for show.

  A run starts at **120**, set together with 0x4000000 as the intro's status screen closes (watched) -
  by R70C's function 5, it would seem: of every room script it is the only one that sets 120
  (`4D 01 05 01`, `1E 29 78 00`), and its function 0 clears the bit as the room loads. It loses one
  at once on its first frame. The status screen takes
  the gameplay task's place, so the clock stands still while it is up (watched). Kills pay through
  **0x455C40** (from 0x455C70, a money/seconds pair per enemy type at 0x529DEC, more for a combo):
  `[0xA61FC8] += seconds`, the "+N" read-out `0xA5C878` too, money `0xA5C87A`. The results (0x443C90,
  the tally at 0x444503) read the clock, and each mercenary's best is kept at 0xA62600.
- **The main game's room scripts start the same countdown** (found 2026-10-08 from the room files,
  after the user saw the freeze work outside the Mercenaries; see "The room scripts"). They set the
  bit with 0x4D (group 1 is the save flags; bit 5, from the top, is 0x4000000) and the clock with 0x1E
  (the `s16` array at 0xA61F76, whose [0x29] is the clock):
  - **R402**, the hospital ("It seems to be a time-based bomb." / "I must escape immediately."),
    function 16: the bit, and the clock at **20 on Easy, 7 otherwise** (an `if` on state flag 0x100;
    as read, not watched). R417, the hospital's site afterwards ("There was a hospital here, but it's
    gone now."), clears the bit as it loads.
  - **R509**, the Dead Factory, function 3: the clock at **240**, then the bit. R50A's functions 65
    and 67 set the bit again with the clock as it was - but once the radar receiver has been taken
    (the walkthroughs have it taken first) the radar runs in the clock's place and counts the missile
    in (the next point); R508's function 4 and R50E's 4, 7 and 9 clear it, and so does R600's
    function 0. What the four minutes are for was not traced.

  A time-out takes the same path as the Mercenaries' (above), so in the main game it is a death. The
  main exe's own 270 (0x451CB6) is not a new game's: it is state 11 of the table at 0x528FC8 (called
  at 0x450B22), which starts modes 0..2 from `Init_tbl.dat` in rooms of their own and mode 3 from
  `Init_sub.dat` with the Mercenaries bit, and a new game proper (0x4512A3) sets no countdown. Not
  traced further.
- **The missile at the very end is the radar's own countdown, not the clock** (found 2026-10-08,
  after the user saw Freeze countdown timer let it run on; watched headless the same day). Taking
  the radar receiver (R50A's function 10: "It's a portable radar receiver." / "You've got the radar
  receiver.") sets save flag **0x2000000** and flag 0x2F/0x44 (group 0x2F is 0xA625BC; R50A's
  functions 42, 45 and 50 set it too), and from then on the gameplay loop calls **the radar
  (0x45ECD0)** in place of the countdown's whole block, zeroing its state byte the frame the bit
  comes on (0x42B3AE). Its struct is **0xA35B60** (up to the status screen's at 0xA35EE0; nothing
  else in the code touches it): `+0` the state (handlers at 0x52A1A8: 0 the set-up 0x45ECF0, which
  loads `Radar.tim`, 1 the run 0x45F160), `+1` its own state (jump table 0x45F398, 0..9), `+0xC`
  the sweep's angle, and **`+0x10` the missile's distance, an s32** (0xA35B70). Its own states:
  - 0 waits for 0x2F/0x44;
  - 1 waits for the countdown's bit 0x4000000 (R50A's functions 65 and 67 set it; by the
    walkthroughs, examining the floor hatch starts the missile countdown) and puts the distance at
    **4,739,872** (0x4853A0);
  - **2** (0x45F234) runs the step and moves to 3 once the distance is under 474,000 (0x73B90), where
    the blip is drawn on a closer scale;
  - **3** (0x45F2D0) runs the step and, once the distance is 0 or under and no room change
    (`0xA587A0`/`A1`), no other screen (`Task_state(1)`) and nothing 0x424F40 waits on is under
    way, cuts the voice, clears 0x6000000 and sets the state flags' 0x200 - the countdown's own
    time-up, so the game over follows (watched: a white flash, then the session ended).

  **The step (0x45F460)**: unless the state flags have 0x400 - or, on alternate frames, the save
  flags 0x10000000 with `0xA58781` set (the gameplay loop's own half-rate test, 0x42B72A) - the sweep
  turns 0x40 and **the distance comes down 263** (`add ecx,-0x107`). That is 18,022 frames from the
  start, **ten minutes** at the loop's 30 a second (watched: 7,890 a second). The radar draws the
  distance as seven digits (0x45F7F0) and moves the blip from it each time the sweep passes 0x5C0
  (0x45F680). Reloading a save made while it counts should bring the missile back at its full
  distance (the struct is not in the save block, and the loop sets the radar up afresh when the bit
  appears) - as read, not watched.
- **Freeze countdown timer** (in the panel's Player section; until 2026-10-08 it was "Freeze
  Mercenaries timer" in the Mercenaries section - the ini key is still `FreezeMercsTimer`, so ini
  files written before keep the switch) is three patches, two for the clock and one for the
  missile. The clock's: the `dec` NOPed, and the frame count held still
  (`jne` → `jmp` at 0x42B6E9, past its +1 and the hundredths). With the `dec` gone a frame count past
  29 is just reset, so a freeze that starts on frame 30 loses nothing and a run that starts frozen
  keeps its 120; with the count held the hundredths stop too, so the clock looks paused rather than
  spinning. Jumping over the whole tick instead would also skip the first-frame reset and leave the
  frame count at whatever the last run left (0x8AD0 after a time-out, 0x42BCAC), which the hundredths
  then draw. Bonuses still add. Watched toggled live (2026-10-07, headless): 120 → 107 a second at a
  time, held at 107 for 94 s across two visits to the status screen, then 106, 105... from a second
  after the switch went off. Both patches are inside the countdown's block of the gameplay loop,
  which the main game runs as the Mercenaries does, so the freeze holds its countdowns the same way
  (not yet watched there; see "What is not done yet").
  **The missile's** is the step's store NOPed (`89 48 10` at 0x45F4A1): the distance stands still in
  either counting state while the sweep keeps turning, and it runs on from there when the switch goes
  off. Until 2026-10-08 the freeze had only the clock's two, which the radar never reaches - the
  user's report. Watched (2026-10-08, headless; a test build set the radar's bits in the attract
  demo, see "Testing without playing"): the freeze held the distance at 4,480,419 for 5 s and,
  in the close state, at 31,560 for 4.5 s; switched off, the latter ran out four seconds later and
  the game ended. The panel shows the time it has left at the radar's pace - the distance over 263
  × 30 - as "(missile in M:SS[, frozen])" while the save flags have both bits and the radar is
  running in its far or close state (`game::missile_running`).
- **The money** is two amounts (2026-10-07, from the dumps; not yet watched live):
  - **the takings**, a u16 at **0xA5C87A**: cleared with the "+N" read-out as a run starts (0x447B47,
    from the player select; 0x451FEA in the main exe's own start), and paid into by the kill bonus
    (0x455C57, an `add` with no cap). The pair table at 0x529DEC pays $5 for most zombie types, $10
    for 0x22/0x24/0x28, $40 for 0x34; the weapon in hand (0x455C97, 0x455D03) and the combo count
    0xA35414 scale it. Nothing shows the takings during a run;
  - **the bank**, a u32 at **0xA6261C**: what every run's results pay into and the shop spends. Every
    add stops it at **999,999** (`mov edi,0xF423F`, 0x444532 and 0x44534C); drawn as `@%6d` (0x526718).
  - **The results** are two screens of the table at 0x526364 (state struct 0xA456C0). Screen 1
    (0x443C60: init 0x443F90, state machine 0x444420, draw 0x443C90) ranks the run on the takings
    alone (0x4462B0: F under $300, a letter up every $300, A from $1,500; letters at 0x52635C), puts
    **`clock/2 + bonus + takings` into a u16** at the state's `+0x12` (0x444503; the bonus `+0x18` is
    100 when bit 0x18 of the flag array 0xA61FD0 is set, 0x4443F5) and counts that into the bank three
    a frame (0x44452D). A press (0x3000 in 0xA5C868) adds what is left at once **with `movsx`**
    (0x4445EA): past 32,767 that adds a negative number, which takes the bank down - or under 0, where
    the unsigned cap test turns it into 999,999. Then the records (0x44465C: each character's best
    total at 0xA62610, compared unsigned, its clock at 0xA62600, its rank at 0xA6260C), the ranking
    line in `regist.txt` (0x4063C0: date, character, total, clock, URL) and the shop (0x4447E0: four
    items at 0x5266B4 for $2,000 / $3,000 / $4,000 / $9,999, unlocking bits 8, 7, 6, 9 of 0xA61F60
    (0x5266BC); a purchase needs the bank to cover the price and counts it off three a frame).
    Screen 2 (0x444B70: init 0x444DE0, state machine 0x445240, draw 0x444BA0) pays **half the
    takings** (0x44532B, `shr`) into the same bank the same way; which end of a run gets it was not
    traced.
  - **`saves\data.sav`** (`.\saves\data.sav`, 0xA63D90) is a profile file, read and written with
    Get/WritePrivateProfileStringA: section `Data`, keys `Data%02d` (0x514570), each value 20 hex
    digits obfuscated with a seeded generator and checked against 0x2D8B50D2 (0x406060 decodes,
    0x406270 encodes, both worked out in "The clear count and the epilogues"; 0x405FC0 / 0x406210 read
    and write one key). Key 0 is the bank (written only when it is not 0), 1..4 the shop's unlocks, 10
    the clear count 0xA61F6E (also only when not 0), and 16 + 4n.. character n's record
    (character, clock, rank, total). 0x4262A0 writes it (from 0x4270D0, which also puts the save
    block back from its copy at 0xA5880C, and from 0x442E13); 0x426380 reads it as a game starts
    (0x45137B, before the intro) and for the RESULT screen (0x443639); the Mercenaries exe's 0x4264C0
    clears it all. Two copies (0x426F30 out, 0x426FA0 back) carry the records and the bank across a
    reload of the save block, each within one call, so a tick cannot land between them. The user's
    own file and `regist.txt` of 2026-10-07: written two minutes apart, total 3,171 at 1,494 s on a
    frozen clock - a long run's takings are in the low thousands.
  - **The editor** (`game::set_takings` / `set_bank`, only while the state flags have 0x80) holds the
    takings to 0..**9,999** (`game::kTakingsMax`: an A rank many times over, and the u16 sum above
    stays under 32,768 up to a 45,000-second clock) and the bank to 0..the cap read out of the tally.
    The panel's "The Mercenaries" section, which holds it, is only drawn while that bit is set
    (`cheats::Status::mercenaries`); outside the Mercenaries it is not there at all.

### The Rofs archives and the item icons (read offline; the mod does not touch them)
Worked out on 2026-10-07 to check the herb icons against the panel's names.
- `bio3.ini` lists `Rofs1..15` (`Rofs%d`, 0x514404). The file system is the subsystem at `[0x5349DC]`
  (vtable 0x50AE70, constructor 0x414C80) with one 0x54C88-byte archive object per file (vtable
  0x511030). A directory is a `u32` count, then `{u32 offset / 8, u32 size, name\0}` per entry
  (`Rofs3.dat`'s `ETC` starts at 0x1000).
- A file starts `{u16 header size, u16 blocks, u32 unpacked size, 8-byte type, u32 key[blocks], u32
  size[blocks]}`; the type is "Hi_Comp" or "NotComp" XORed with its own last byte. Each block (0x8000
  bytes unpacked, the last one less) is decrypted by 0x422D60: one XOR byte for a run of bytes, the
  run's length from a 64-entry table built on the stack and both drawn from an LCG (0x41F7E0:
  `s = s * 0x5D588B65 + 0x8000000B`, the byte `s >> 24`) seeded with the block's key. The key is also
  the decrypted block's CRC32 (0x423770, table 0x5110A8; 0 counts as 1). "Hi_Comp" blocks are then
  LZSS (0x4235F0): a 4 KB window filled with sixteen of each byte value, one flag bit per token MSB
  first, and a set bit a 12-bit window offset and a 4-bit length less 2.
- Details that matter when reading one (an extractor was written again on 2026-10-08 for the room
  scripts): an archive is 0x15 bytes of header, the root's name (`DATA`, `DATA_U`, ...), then one
  directory entry for its folder (`ETC`, `RDT`, ...) whose offset leads to the files' directory. The decryption takes the XOR
  byte and then the table index first, and the index (`byte % 0x3F`, so the table's last entry is
  never used) and then the XOR byte for each run after; a run is the table's entry plus one bytes.
  An LZSS reference copies out of the window as it stands and then writes what it copied at the
  window's position, which goes back to 0 at 0x1000 rather than wrapping (a copy can spill up to 17
  bytes into 256 spare bytes past the window, which later references can read).
- **The item icons are `etc/itemi.pix`** (`Rofs3.dat`): 134 records of 0x800 bytes by item id
  (0x4DCC60 loads `id << 11`), each a 40x30 image at 8 bits per pixel. Their palette is the status
  screen's, row 1 (VRAM 0,481) of `data_u/etc2/stmain0u.tim` (`Rofs4.dat`): through it 0x21 is a
  green herb, **0x22 a blue one, 0x23 a red one, 0x25 a bowl of green + blue and 0x26 green + red** -
  as the names, the descriptions, the combination table and the heal code all have them.

### The room scripts and the two Facility Keys (read offline; the mod does not touch them)
Worked out on 2026-10-08, when the user could not open the Dead Factory's card slot with a Facility
Key put in from the panel.
- **The rooms** are `data_u/rdt/Rsrr.RDT` in `Rofs13.dat` (s the stage + 1, rr the room; `Rofs12.dat`
  holds `data_aj`'s). An 8-byte header, then 23 `u32` offsets: **[13] the messages** (a `u16` table
  of offsets, then the text, shifted as the exe's is) and **[16] the script** (a `u16` table of
  function offsets, then the functions). Function 0 runs as the room loads and gosubs (0x19) the
  functions that set its spots up.
- **The interpreter** is 0x46D1B0, opcode table 0x51BF50 (144 entries); each handler moves the script
  pointer (`+0x1C` of the script state) on itself. Read so far: 0x06 / 0x07 / 0x08 if / else / endif
  (`u16` length); 0x3E takes an item away (`find_item_slot`, then `remove_item_slot`); 0x4C tests and
  0x4D sets a flag (group, bit, value: the groups are the table at 0x52B784, and bit n is counted from
  the top bit of dword n>>5; 0x4D's value 0 clears, 1 sets); 0x4E compares an `s16` of the array at
  0xA61F76 (index, test, value), and 0x1E sets one (`1E index s16`), 0x1F copies one to another
  (`1F to from`), 0x20 and 0x21 do arithmetic on one with a constant (`20 ? op index s16`) or another
  (`21 op to from`) through 0x46D5C0 (op 0 adds, 1 subtracts, up to 11);
  0x5B shows a message; 0x61 is a door (32 bytes); **0x63 sets a spot** (20 bytes: id, type,
  attribute, floor, ?, x, z, w, d, six bytes of data); 0x65 retypes one (10 bytes; type 0 is off);
  **0x66 fires a spot as if it had been examined**; **0x67 is an item spot** (22 bytes: the spot's
  first 14, then item, count and pickup flag as `u16`s); 0x6B is "has this item". A spot's type picks
  its handler from 0x529EEC: 1 door, 2 item, 4 message, 5 event (a function), **6 use an item here**
  (data: message, item id), 8 typewriter.
- **Using an item** from the status screen inside a type-6 spot writes its id to **0xA61F7A** (that
  array's [2]; 0x4DA8CC writes it, and every room load sets [0..3] to -1 at 0x430460). The room's
  loop compares it (0x4E) with what the spot wants, and any other item does nothing. **An item spot's
  pickup** opens the status screen to take it (0x45B750: save flags `|= 0x200`, the screen's `+4` = 1).
  Its flag goes in group 8 (0xA62058) from stage 2 on, and in group 7 (0xA62038) before that and in
  the Mercenaries (0x46B7E0).
- **The two Facility Keys are one card, before and after ultraviolet light.**
  - 0x7E is picked up in R501 (flag 8/0x65).
  - R506's water-check device has a type-6 spot for it. Using it there runs function 8: 0x3E takes the
    card, flag 3/0xF4 is set, and 0x66 fires item spot 8, which holds **0x7F** (flag 8/0x6D).
  - If that pickup is turned down, or there is no room, the card stays in the device. The room's
    set-up then leaves an event spot there (function 9) that offers it again.
  - **R502's card slot wants 0x7F.** It is the "sturdy-looking door" on the ground floor, which the
    room's elevator (door type 0x1F, floor 3 to 0) reaches. Function 3 compares 0xA61F7A with 0x7F,
    takes the card and sets flag 9/0x22. The elevator itself wants the Card Key (0x60, picked up in
    R509).
  - The panel used to call the keys "(1)" and "(2)"; they are "before UV" and "glowing" now. The
    user's `bu00.sav` of 2026-10-08 has flags 8/0x65 and 3/0xF4 set and 8/0x6D clear: 0x7E went
    through the device and 0x7F was never taken. They then got another 0x7E from the panel, which
    nothing in the Dead Factory takes any more.

### Signatures
All sixty-eight are unique in the code section and every operand is cross-checked against the other
sites that name it; `tools/check_sigs.py` runs them with those checks against the dump and asserts
the build-21774525 values. The countdown's three agree on the frame count, the hundredths and last
frame's save flags, and the clock its tick reaches through `esi` (the save flags less the frames
site's `[esi+0x77FC]`, plus `0x7868`) has to be the address the kill bonus names directly. The
missile's four (`discover_missile`) start from the countdown tick, which names the radar's struct
(where it zeroes the state byte) and calls the radar. The radar has to hold its pattern there,
dispatch on that struct's byte and hand the struct over (`check_sigs.py` checks it where the call
lands). Its handler table has to hold the radar's run once, and the run's own state table the far
and the close state once each; their indices are the states `missile_running` tests. Both states
have to call the step and read the distance at the step's offset. The step and the close state have
to name the state flags and the save flags discovery found, and the close state the voice state too;
the mask it clears, 0x6000000, is in its pattern. The two
results tallies (screen 1's at 0x444503, screen 2's at 0x44532B; 0x444553 and 0x44532B in the
Mercenaries exe) name the bank three times and its cap once each and must agree on both, the takings
they read must be what the kill bonus pays into, and screen 1's clock must be the countdown. The
reloading tool's seven (`discover_mixing`): the combine's dispatch has to land on the batch and the
grenade mix signatures; the batch, its count, the enhanced offer and the grenade mix all reach the
counts through the inventory pointer at one offset, the grenade mix's count and level row being the
grenade kind's place in both (+0x132, row 3); both bit tests call the one `bit test` found on its
own, on the state flags, for the same bit; the two loop bounds agree on five levels; and the kind
table has to file the ammo as `items.h` names it. The enhanced bonuses are read out of the switch
itself: each jump-table entry has to land on one of its three cases or its end. The healing names
the recovery total twice and the rank once, and all three must agree; it also names HP three times,
the maximum and the condition word, which must be the damage routine's and the new game's; and the
rank's ten steps must go down in total and up in score. The play time's four: the save's update and
the ending's must both call the one `VSync` found on its own (whose slot, `+0x78`, is in its pattern:
a wildcard there would match its neighbours) and name the same total and base, and `VSync`'s method
must answer the field the pacing spin raises, which the spin names twice; at run time the system
object's own vtable entry must be that method before its count is read. The memory cards' thirteen
(`discover_cards`): the screen's entry has to call `Task_state` and the screen's loop; the loop, its
init, the card read and the slot map have to name one state pointer, which the init points at the
struct it clears; the read-the-card state has to call the read, and the read the slot map; the
state table has to hold the read-the-card and the list handlers once each (their indices are the
states the mod uses); the loop and the list have to read the input poll's button words; the list's
row has to draw from where the read copies, with the save count at the save block's offset of it
(`save count - play time`) and the Easy bit through the one bit test. The container's maker gives
the card class's constructor and destructor, which must match their patterns where it names them;
the destructor has to call the write found on its own, with the constructor's vtable; the write, the
constructor and the file creation have to agree on the image and the folder, the container size on
`first + cards * card size`, the global's site on the app field; and the name's virtual, read out of
the card vtable at the slot the write calls, has to be `lea eax,[ecx+n]; ret`. At run time the
container and each card object are checked against their vtables before the image is touched, and
`check_sigs.py` checks the destructor where the maker names it (its pattern alone matches twice).
The clear count's three (`discover_clears`): the ending's epilogue test, the increment it calls after
the fade and the EPILOGUE screen's menu have to name one count, which has to be the play time's
address plus the offset the card list draws a save's clears from; the test and the menu (twice) have
to agree on the number of epilogues; the test's Easy bit has to be the reloading tool's, the
increment's save count the save's, and the menu's buttons the input poll's.
The outfit's four (`discover_outfit`): the room load's model test and the end of the load it skips
have to name one type byte, the player entity's (HP less 0xCC, plus 0x4A), and one character, and
the test's `je` has to land just past that load; the test has to name HP and the condition word, its
first character not Jill has to be the inventory select's first that is not shared (8), and the load
has to set the maximum HP and the player's state (+4) and call the inventory select and the model
loader. The loader has to write one file name's two digits (`.pld` six and five from its end),
load the model and both textures through one function, have its switch on outfit - 1 send 1..5 to the
"another model" case and 6 and 7 to the two texture cases (the names assume that order), and call a
getter `mov eax,[outfit]; ret`; the title grid's setter `mov [outfit],eax` has to write the same
address, and its cells have to give outfits 1..7 then 0, the order of their labels.
The patch sites are:

| site | where | patch |
| --- | --- | --- |
| save count +1 | 0x44DAE3 `FE 05 <0xA61F70>` | six NOPs |
| the save's ink ribbon | 0x44DA99 `75 20` | `EB 20` — jump over `find_item_slot(0x81)` and the take |
| the typewriter's ribbon check | 0x45B9F7 `7C 1A` | `90 90` — the "no ribbon" branch never taken (its call is verified to be `find_item_slot`) |
| button state stores | 0x42CF2A, three `mov [abs],reg` | eighteen NOPs, and the three words zeroed once |
| button repeat store | 0x42CF7F `89 3D <0xA5C860>` | six NOPs |
| the countdown's second | 0x42B3EE `66 FF 8E <+0x7868>` | seven NOPs |
| the countdown's frame count | 0x42B6E9 `75 26` | `EB 26` — past the frame count's +1 and the hundredths |
| the missile's step | 0x45F4A1 `89 48 10` (65 bytes into the "missile step" signature) | three NOPs — the radar's sweep still turns; the distance is not stored |
| the player's damage | 0x46385A `66 2B C8` (`sub cx,ax`, 16 bytes into the "player damage" signature) | three NOPs — HP stored back as it was, every test after sets its own flags |
| the recovery total's store | 0x4DA864 `89 2D <0xA625FC>` (77 bytes into the "healing" signature) | six NOPs — the item still heals; the total is not written |
| the save's play-time add | 0x44F12B `89 15 <0xA61F58>` (59 bytes into the "play time" signature) | six NOPs — the base still moves on; the total is not written |
| the ending's play-time add | 0x442CE9 `89 15 <0xA61F58>` (25 bytes into "ending's play time") | six NOPs |
| the room load's model test | 0x468616 `0F 84 <rel32>` (20 bytes into "player model test") | six NOPs, only while an outfit set from the panel waits for the loader — the next room load loads the player's model |

The input guard is the pair of button-store patches: while a panel field has keyboard focus the poll
writes nothing, so the game sees every button up. That takes Escape away from the game too for as
long as a field has focus, which is why the panel's own key goes through the ImGui backend instead. The
cutscene skip holds the same guard while it asks, while it fast-forwards (not while a page or a
choice waits for the player) and for 400 ms after, so the key that answered it is not also a press.

## What is not done yet
- **The panel has not been seen on Windows yet.** The first Windows run (2026-10-08, the user's
  laptop, Steam build 21774525, no Steam overlay in the game) loaded the mod: discovery found all 18
  anchor groups, the tick ran, and closing the game logged "process exiting". But it never saw the
  device, because of Windows' shim (see "The panel's hooks on Windows").
  - The fix (every IDirect3D9 adopted) passes the test's "shim" mode and was installed for a second
    run.
  - What that run's log should show: "it led to apphelp.dll+...", "IDirect3D9 ... adopted" (twice),
    "dx9: device ... adopted", then "implicit swap chain ... adopted".
  - Not tried yet on Windows: the Steam overlay inside the game (its log shows it attached to the
    launcher but did not follow it into `ResidentEvil3.exe`), and the Mercenaries.
- **Freeze play time as it is now has not been run in the game.** It was rewritten on 2026-10-07
  while the user had the game open, so no headless run was possible; `check_sigs.py` (both dumps) and
  the build vouch for it. To test: `Watch = 0xA61F58:4` in the scratch ini (not the base: it moves
  every frame while frozen), a new game (`saves\` backed up first), the freeze on, a minute or two of
  play, a save at a typewriter (Infinite ink ribbons on): the total must not move in the log, and the
  u32 at 0x2200 of the card file (a card with one save) must be the frozen total exactly. Then the
  freeze off: the panel's time should run on from the frozen value, not jump by the minutes it was
  frozen, and the next save should write that.
- **The recovery editor and Heal without counting have not been run in the game.** They were written
  on 2026-10-07 while the user had the game open, so no headless run was possible; `check_sigs.py`
  (both dumps) and the build vouch for them. To test: `Watch = 0xA625FC:4` in the scratch ini, a new
  game (the Easy start has a first aid box), the status screen, the box used on the game's own
  screen: the total should go up by the maximum HP (200), whatever HP was. Then Heal without counting
  on and another use: HP up, the total not; then a value set from the panel, and the Watch line.
- **The reloading tool's proficiency editor has not been run in the game.** It was written on
  2026-10-07 while the user had the game open, so no headless run was possible; `check_sigs.py`
  (both dumps), the build and an offline run of the panel's arithmetic over the dump's tables vouch
  for it. To test: `Watch = 0xA62290:2,0xA62292:2,0xA62294:2,0xA62296:2` in the scratch ini (Jill's
  four counts), a new game (Jill starts with the reloading tool), the status screen, a level picked
  for handgun bullets, Gun Powder A put in a slot with the inventory editor, then combined with the
  tool on the game's own screen: the batch should be the panel's number (26 at level 5, 23 enhanced,
  doubled on Easy - the documented start is Easy) and the count should go up one.
- **The clear count editor has not been run in the game.** It was written on 2026-10-08 while the
  user had the game open, so no headless run was possible; `check_sigs.py` (both dumps) and the build
  vouch for it. To test without playing to the end: a test build can set the count and then state
  flag 0x4000 in the attract demo, which should run the real ending (0x42BC31, see "The clear count
  and the epilogues"). Watch the epilogue screen (count under 8), the count going up, and data.sav's
  Data10 (back up `saves\` first, decode as described there), then the title's EPILOGUE screen: all
  eight open from a count of 8. Or from a save near the end: the count set to 7, then finish.
- **The outfit picker's other paths.** Run headless (a test-only warp in place of a door) and by the
  user in play on 2026-10-08, with Jill in play. Not tried: a change made in a Carlos section (only
  the value is written; Jill's return is a change of type, which loads her model), a boutique type
  (it needs the Boutique Key and R10F), and a save made while a change waits for the next door (it
  carries the new outfit, so loading it should show it at once).
- **The Mercenaries money editor has not been run in the game.** It was written on 2026-10-07 while
  the GPU was faulted (see "Testing without playing"), so only `check_sigs.py` (both dumps) and the
  build vouch for it. To test: `Watch = 0xA5C87A:2,0xA6261C:4` in the scratch ini, a Mercenaries run,
  set both amounts on the intro's status screen, close it (V), let the clock run out, and see the
  results pay the takings into the bank and `saves\data.sav`'s `Data00` change.
- **Freeze countdown timer's clock has not been watched in the main game.** The patches are the
  Mercenaries' (watched there) inside a block the main game runs the same way, and the room scripts
  that start its countdowns were read offline (see "The Mercenaries"), but the hospital and the Dead
  Factory are far into the game and there is no save near either to test from. The missile's patch
  was watched, but only in the attract demo with the radar's bits set by a test build, not in R50A
  itself. The panel's "(missile in M:SS)" has not been seen on the status screen; the test build
  logged the numbers it is made from.
- **The save file manager in the attract demo** (seen 2026-10-08, headless, not looked into). During
  the demo's typewriter save screen the panel came up and listed card 0 as a single "Warehouse,
  0:01:10, 0 saves, Hard" save, while the file the game had read at start (`recon` logged it) held
  two Dead Factory saves. The listing is read from the in-memory image, so the image had changed.
  The harness's own old `bu00.sav` holds exactly that one save. A "copy this save?" confirmation
  (slot 1 to 2) was also open without a click: only two Returns were sent, and the panel has no
  keyboard navigation. Nothing was copied and no card file was written (the run was killed). Find
  out what the demo does to the card, and whether a confirmed change in the demo would write that
  image over the player's file, before trusting the save manager while a demo runs.
- **Which end of a Mercenaries run gets the half-pay results screen** (screen 2, see "The money").
- **A voiced in-game scene past the opening** has not been run through the skip: the headless test
  can only reach the intro narration and the opening scene. Both voice paths are the same player
  (0x427220, state `0xA59E51`, flag 0x20), and the narration - voiced, and the case that used to
  stall - skips cleanly, so this is untried rather than doubtful. A test-only warp (a door record of
  our own in `[0xA5C8B8]`, byte `0xA587A0` = 1; the gameplay loop then runs the room change
  0x42C250 - record: x, y, z, dir as shorts, stage +8, room +9, cut +0xA) reached the warehouse
  office (stage 0, room 0, cut 1, x -25706, z -21191, dir 0xC98) but not Dario's scene, which wants
  the door from the warehouse floor.
- **The pad** is read through XInput (`xinput1_4/1_3/9_1_0`), which found a pad on the test
  machine, but no button press could be sent to it headless: Start/A/B are untested by hand.
- **The inventory editor calls the Mercenaries' block Jill's.** All three mercenaries use the shared
  block (index 0), and `inventory.cpp` names a block by its index.
- **The ammo decrement.** Infinite ammo is a hold on the equipped slot rather than a patch, because
  the firing code reaches the count through a cached pointer rather than the slot index, and the
  decrement was not found. The hold behaves the same from the player's side but refills the weapon
  rather than never spending the round.
- **A second character's inventory and box.** `inventory_index()` is read, but the panel only edits
  whichever block is in play.
- **A live icon redraw.** See "The inventory": the game's screen shows an edit the next time it
  opens. Redrawing in place means calling its icon upload (0x4DCCE0), which the rules forbid.
- **Item flags.** Bits 0..1 = 3 mark a weapon as infinite (0x4DEA2A, 0x4DEC36: no decrement). Byte +2 of a slot is carried along on every move and cleared when the editor puts a
  different item in a slot; what its bits mean (0x10 at 0x4D9E28, 0x20 at 0x4DA8D6) is not known.

## Dead ends worth not repeating
- **Swapping the green + red and green + blue mix labels.** On 2026-10-07 the user saw a green + red
  picture on a mix the panel called green + blue (0x25), so `items.h` swapped the two names, then
  swapped them back when a green + red herb they took from the box was 0x26. A mix is always its
  recipe's item: the combine's last state (0x4DB760, case 0 at 0x4DB845 for type 1) writes the
  recipe's `entry+3` into the slot and uploads that id's icon (0x4DCC90), and the icon file draws 0x25
  green + blue (see "The Rofs archives"). The picture and the panel disagreeing came from elsewhere -
  most likely the status screen's icons, which only redraw as it opens, lagging a Take or Store made
  from the panel while it was up.
- **Freezing the play time by holding the total.** The first Freeze play time put 0xA61F58 back
  whenever it moved on - and it never moved: the game only adds to it as it saves, and writes the card
  in that same frame. So the panel showed the frozen value while every save got that value plus all
  the time since the session started or the last save, and loading it brought that back (the user's
  `bu00.sav` of 22:32 on 2026-10-07: frozen at 29:37, saved at 41:29 after eleven minutes of play).
  It also re-froze at a loaded save's total whenever the game had started afresh, and its editor
  left the base alone, so a value set from the panel still had the time since the last save added
  to it at the next one. See "The play time".
- **God mode as a hold alone.** Topping HP back up from the tick is between frames; the damage
  routine sets the death flag inside the frame a big hit lands, so anything over HP + 10 (a barrel
  at point blank) killed straight through it. The hit has to be taken out where it is applied.
- **A clock warp by a factor inflates play time.** Running `timeGetTime` 20x moved the scene on, but
  whenever the machine could not keep up the clock ran ahead of the frames, and the play-time clock
  counts vblanks. Moving it 1 ms per read ties it to the frames instead.
- **Out-running the audio does not work.** A voiced scene waits for its line to finish playing,
  which happens in real time whatever the frame rate; ending the line (state 3) is the way.
- **Forcing the frame wait does not fast-forward.** Writing the vsync count `0xA629F0` up to the
  divider from the `PeekMessageA` hook only ends a wait that has already run its subsystems once, and
  the pacing is the logic thread's spin on `timeGetTime` anyway. Warping only the *main* thread's
  `timeGetTime` did nothing either: it is the logic thread that asks (a stack sample of every thread
  - `SuspendThread` + scan for return addresses into the image - is what showed both).
- **RE1's unpack gate does not fit RE3.** See above: prologue counting is a compiler artefact, not a
  property of unpacked code. Entropy is the signal that carries.
- **The item box was looked for outside the inventory block** (the save block, `0xA6207C`) on the
  assumption that the block's 0x12B bytes were all slots and bookkeeping. They are not: +0x28..+0x127
  is the box. The box screen's own list (0x4E5751) is what settled it.
- **`.idata` is not the import table here.** Section 3 (0xA64000) is 86% zeros at run time and the
  383 non-zero dwords in it are leftovers, not pointers; the mod read them as an import table and
  found nothing, and `hook_import` silently did nothing until `find_iat()` existed.
- **`find_iat()` has to run before the pump hook.** It used to be called only from `game::recon()`,
  which the mod thread runs *after* `dispatch::hook_message_pump()` — so the first launch logged
  "the game's PeekMessageA slot was not found - no main-thread tick" while recon, seconds later,
  reported the slot was right there.
- **Plain `wine` cannot run this game at all**, with or without the mod: the Enigma wrapper calls
  `SteamAPI_Init` and exits within a second when there is no Steam client. Everything has to go
  through Proton with Steam running (see "Testing without playing").

## Tooling notes
- x86 encodes small displacements in one byte: write patterns from the raw bytes (`objdump -d` on the
  fixed dump), never from mnemonics.
- `tools/fix_dump.py` rebuilds a real import directory in the free tail of `.idata` from the mod's
  import map, so Ghidra names all ~170 API calls; it also finds the CRT startup (the function that
  calls both `GetCommandLineA` and `GetStartupInfoA`) and points the entry point at it.
- Each `tools/ghidra/run.sh` invocation reopens the project (~40 s). Batch questions into one run
  with the `x:`/`d:` prefixes, and use `ghidra_proj/re3.asm` for anything a grep can answer.
