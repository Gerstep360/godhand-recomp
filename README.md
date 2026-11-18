# God Hand Recompiled

**God Hand (PS2, 2006) running on Windows as recompiled x64 code. The graphics and audio paths still rely on compatibility with the PS2's GS, VU1 and SPU2.**

A separate [native PC scene](native/README.md) uses converted room geometry, Gene's animation and stereo music. This executable builds with raylib and reads the prepared PC assets directly.

> [!WARNING]
> **Work in progress.** Tested from the start of the game into stage 3, past the Gorilla Mask and Mad Midget Five boss fights, played
> side by side with the original game on a real PlayStation 2 (the tester's own console and disc).
> The rest of stage 3, later stages, the final bosses and the ending have not been played yet. Expect bugs, and please report them
> (see [Reporting bugs](#reporting-bugs)).

![God Hand running on Windows, the title screen](assets/title-screen.png)

You need your own copy of the game (God Hand, USA version). **No game files are included here.**

---

## Progress (last updated October 20, 2026)

How far the game has been played on this port so far (each part compared side by side with a real PS2):

| Part of the game | Status |
|------------------|--------|
| Opening, title menu, Clover logo movie | ✅ Played |
| Stage 1 (all areas and the boss) | ✅ Played |
| Stage 2 (all areas, the sepia flashback, the boss) | ✅ Played |
| Stage 3 (the amusement park) | 🟡 Gorilla Mask and Mad Midget Five beaten |
| Stages 4 to 8 and their bosses | ⬜ Not played yet |
| Ending and credits | ⬜ Not played yet |
| Coliseum and the other extras | ⬜ Not played yet |

| Game feature | Status |
|--------------|--------|
| Fighting, combos and the Tension gauge | ✅ Works |
| God Reel (ability roulette) | ✅ Works |
| Dying and continuing | ✅ Works |
| Story cutscenes | ✅ Works |
| Saving and loading (memory card) | ✅ Works |
| Casino minigames | ✅ Works |
| Pirate ship cannon minigame (shows your buttons) | ✅ Works |
| Movies other than the Clover logo (ending, staff roll, trailers) | ⬜ Not seen yet |

---

## What works

- **The game itself:** fighting, bosses, dying and continuing, story cutscenes and movies, the casino, the God Reel,
  saving and loading. Tested through stage 2 and stage 3 past Gorilla Mask and the Mad Midget Five.
- **60 FPS at 3x resolution** (1536x1344, shown in the original 4:3) on a modern graphics card.
- **Windowed or full screen** (F11).
- **Keyboard, mouse and controllers**, all rebindable in game with **F1**.
- **Button prompts show your keys:** if "Action" is on the F key, the game shows "F" instead of the PS2 circle.
  With an Xbox-style controller it shows A, B, X, Y; a PlayStation controller keeps the PS2 symbols.
- **Minigames show the proper buttons too:** the crates in the pirate ship cannon minigame show the buttons you
  actually press. Prompts switch the moment you change between keyboard and controller.

![The pirate ship cannon minigame with a PlayStation controller, the keyboard and an Xbox-style controller](assets/minigame-buttons.jpg)

## Known issues

- **First start is slow:** the graphics are prepared for your graphics card the first time. The very first start can
  sit on a black screen for a couple of minutes. After that it starts in seconds.
- **Short pauses the first time an effect appears**, while its graphics are prepared. They do not come back once seen.
- **Freezes:** the ones found so far (disc reads, cutscene effects) are fixed. If you still see one, please report it.
- **Not played yet:** the rest of stage 3 and everything after it, the ending and its movies. They may crash or misbehave.

---

## Screenshots

All captured from this port running on PC.

| | |
|---|---|
| ![Title menu](assets/title-menu.jpg) | ![Warning screen](assets/warning-screen.jpg) |
| Title menu | Opening warning screen |
| ![Opening cutscene](assets/cutscene-faces.jpg) | ![Saloon fight](assets/saloon-fight.jpg) |
| Opening cutscene | Fighting inside the saloon |
| ![Story cutscene close-up at 3x](assets/cutscene-2k-1.jpg) | ![Story cutscene close-up at 3x](assets/cutscene-2k-2.jpg) |
| Cutscene at 3x resolution | Cutscene at 3x resolution |

![A grab with a button prompt at 3x](assets/fight-3x.jpg)

---

## How to set it up

### What you need

1. **Windows 10 or 11** and a reasonably modern graphics card.
2. **Visual Studio 2022 Build Tools** with "Desktop development with C++" (free from Microsoft).
3. **Python 3.10 or newer**.
4. **Your own God Hand (USA) disc image** (`.iso`, serial SLUS-21503). It must be this exact version: check it
   against [DISC_HASHES.md](DISC_HASHES.md). The port refuses any other disc.

### Build it

Open a terminal (PowerShell) and run these one at a time:

```powershell
git clone https://github.com/LasVola/godhand-recomp.git
cd godhand-recomp
copy "D:\path\to\God Hand (USA).iso" game\
python -I build.py generate
python -I tools\snr2\snr2.py extract --iso "game\God Hand (USA).iso"
python -I tools\snr2\gen_modules.py
python -I build.py modules
python -I build.py build
```

- The middle steps turn the code from your disc into C++ and can take a while.
- The last step downloads a few libraries the first time and then compiles everything (expect 10+ minutes).

### Play

Double-click **`Start-God-Hand.cmd`**. Remember the first start can take a couple of minutes (see
[Known issues](#known-issues)).

The first launch installs your disc files into `build\port\bin\game_data\disc`. Once installed, the game opens
those files directly. The ISO can be disconnected. To install the files ahead of time, run:

```powershell
.\build\port\bin\godhand.exe --prepare-data --iso "game\God Hand (USA).iso"
```

Use `--data <folder>` or `[Game] Data=` in `godhand.ini` to choose another installed location.
`--iso <file>` on its own still opens the ISO directly for compatibility testing.

The installed files preserve the original AFS resource archive and the logical locations requested by the recompiled
game. DirectX 11 is the default Windows GPU device for the GS-compatible renderer. The graphics front end keeps
the vertex state it needs from complete VU1 strips and fans and forwards the draw to the renderer. Five matrix
operations used by camera and model code run as x64 SIMD routines. The rest of the animation system still uses
the original formats and code.

SPU2 registers and DMA timing still drive the audio. The host processes both channels of each decoded PCM block
together and runs the reverb delay line, dry and wet mixing, and gain control in software. On x64 it uses SIMD
to interpolate voice samples before the Windows audio backend plays them.

For stereo device compensation, add `StereoOffsetMs=0` to `[Audio]` in `build\port\bin\godhand.ini`.
A positive value delays the left channel to match a late right channel. A negative value delays the right channel.
The range is −100 to 100 ms. The setting is applied to the final PCM stream at startup.

Your saves are in `build\port\bin\memcards\`. Your control settings are in `build\port\bin\controls.ini`.

---

## Controls

Press **F1** in game (or **Back + Start** on a controller) to change any key or button. Changes save right away.

![The in-game Controls screen](assets/controls-screen.png)

| Action | Keyboard | Controller |
|--------|----------|------------|
| Move / step back | W A S D | Left stick |
| Duck / back flip / sidestep | (not set, choose one with F1) | Right stick |
| Attack / OK | K | A |
| Action / prompt | L | B |
| Combo | J | X |
| Attack / cancel | I | Y |
| Turn around / God Reel | Q / E | LB / RB |
| Taunt / God Hand | 1 / 3 | LT / RT |
| Pause menu / Select | Enter / Backspace | Start / Back |
| Menus | Arrow keys | D-pad |

| Key | What it does |
|-----|--------------|
| F1 | Controls screen |
| F11 | Full screen on / off |
| F12 | Developer debugger |

To quit, close the window.

---

## Troubleshooting

Set `GODHAND_NATIVE_TRANSFORMS=0` to use the recompiled matrix operations when comparing behavior or diagnosing a regression.
Set `PS2X_GS_PACKED_GEOMETRY=0` to compare DirectX 11 graphics with the general GIF decoder.
Set `PS2X_PCSX2_DEVICE=dx12` under `[Runtime]` to use DirectX 12 with `Renderer=pcsx2`.
This needs the x64 DXC libraries `dxcompiler.dll` and `dxil.dll` beside the executable or on the system.
The bridge falls back to DirectX 11 if the libraries are missing or the DirectX 12 device fails to initialize.

| Problem | What to do |
|---------|------------|
| "Wrong disc" message | Your image is a different version. Compare it with [DISC_HASHES.md](DISC_HASHES.md). |
| Black screen on the very first start | Wait: the graphics are being prepared. Later starts are fast. |
| 3D scenes or cutscenes run slowly | See [Faster 3D](#faster-3d-for-developers) below. |
| The game crashes or freezes | Please report it with the log file (next section). |

## Reporting bugs

Open an issue and attach the log of your last session: `build\port\bin\logs\play_last.log`
(the session before that is kept as `logs\play_<time>.log`). Say where in the game it happened.

---

## For developers

### Faster 3D (for developers)

Some 3D effects run on the PS2's VU1 coprocessor. For full speed they are compiled ahead of time:

1. Play the affected 3D scenes, then close the game. Missing programs are saved automatically to
   `build\port\bin\logs\vu1_missing\`, including when the build has no VU AOT code at all.
2. Run `python -I build.py vu1`. It collects the captures, runs the existing VU compiler and rebuilds the game.
   To import earlier captures as well: `python -I build.py vu1 <capture-folder> [<another-folder> ...]`.

The local corpus stays in `build\vu_corpus\`, so later captures add to previous ones. Only changed generated sources
are rebuilt; the EE code and SNR2 modules are not regenerated. An incomplete corpus cannot replace an existing set of
AOT programs. Keep the corpus when cleaning the build directory. Generated game code remains local and is not committed.
Programs from scenes not captured yet still use the interpreter until you repeat these steps.

On integrated graphics with shared RAM, start with `InternalResolution=1` in `build\port\bin\godhand.ini`.
3x renders nine times as many internal samples as the native 1x default. Lower resolution does not cure a missing VU AOT build.
Audio gaps during sustained slowdowns should be investigated alongside game speed; enlarging the audio buffer alone
cannot supply the missing samples.

Frame presentation uses shared GPU textures (`PS2X_GS_DIRECT_PRESENT=1`) to avoid downloading every frame to RAM and
uploading it again. Use `0` for the compatibility path. The renderer also keeps a general hardware shader ready while
new specialized effect shaders compile, instead of falling back to tile/compute rasterization for a whole batch.
`PS2X_GS_HW_FALLBACK=0` restores the previous fallback for comparisons.

`PS2X_GS_HW_PRELOAD=2` prepares previously seen shader states before gameplay, with an animated progress screen using the
original US font loaded from the player's disc. The window keeps processing input while waiting for the GS worker.
This can lengthen startup but avoids loading those programs for the first time during an effect or cinematic.
Use `0` for on-demand loading or `1` for background preload. New effects can still require driver compilation on their
first appearance; neither the VU compiler nor the game's timing or instructions are changed by these settings.

Open the game's original **OPTIONS → CONTROLS** page to remap keyboard, mouse and controller inputs.
The page is rendered from the original IDD resource by the game's own renderer; there is no separate F2 menu.
**Tab / R1** switches between bindings and video settings. Resolution, shader preparation and the performance preset
are saved in `godhand.ini` for the next start; fullscreen applies immediately. **Esc / B** saves bindings and returns
through the original menu transition. The startup preparation dialog reuses the original memory-card window and font.

### How it works

- The main program (`SLUS_215.03`) is recompiled to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp).
  Function boundaries come from the [god-hand-decomp](https://github.com/LucasPicoli/god-hand-decomp) symbol list.
- God Hand loads 81 code overlays (rooms, events, memory card) while you play. `tools/snr2` relocates and recompiles
  each one, and `src/godhand` installs the right one when the game loads it.
- The PS2 hardware (graphics, sound, I/O processor) is reimplemented in `runtime/`. Every game-specific fix is
  commented where it is made.
- What changed and when: [CHANGELOG.md](CHANGELOG.md).

---

## Credits and legal

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors: the recompiler and runtime.
- [LightVelox](https://github.com/LightVelox): the [sotc-port runtime fork](https://github.com/LightVelox/PS2Recomp) and
  [Shadow of the Colossus port](https://github.com/LightVelox/sotc-vibe-pc).
- [thiagordsfx](https://github.com/thiagordsfx): the [Urban Reign port](https://github.com/thiagordsfx/urban-reign-recomp),
  whose runtime, VU1 compiler, sound and PCSX2 graphics bridge this port builds on.
- [PCSX2](https://github.com/PCSX2/pcsx2): graphics code in `thirdparty/`.
- [Ace Combat 5 static recompilation](https://github.com/sal063/Ace-Combat-5-Static-recompilation): the movie decoder
  (Apache-2.0).
- [LucasPicoli/god-hand-decomp](https://github.com/LucasPicoli/god-hand-decomp): function names and boundaries.
- This port: built with AI assistance (Claude Code by Anthropic, with OpenAI Codex), directed and play-tested by
  LasVola (October 2026).

The code here is GPL-3.0 ([LICENSE](LICENSE)), like PS2Recomp. Third-party licenses are in `docs/licenses/` and the
`thirdparty/` folders. **No game data is included.** God Hand is a trademark of Capcom. This project is not affiliated
with Capcom or Clover Studio.

**Let's keep God Hand alive.**
