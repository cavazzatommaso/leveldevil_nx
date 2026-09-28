# Level Devil — Nintendo Switch port (Stencyl / OpenFL / Lime wrapper)

Based on [ChanseyIsTheBest/heartstar_nx](https://github.com/ChanseyIsTheBest/heartstar_nx) (MIT) — all the loader work is theirs.
It loads the original ARM64 Android build of *Level Devil* and recreates the Android
layer underneath it: bionic libc, SDL (liblime's SDL 2.0.12 Dynamic API table is
filled with switch-sdl2), OpenGL ES, audio, input and the Java/JNI side Lime expects.
It contains **no game code and no game assets**.

Tested and working on hardware with Level Devil 1.9.0 (`com.unept.leveldevil`, arm64-v8a).
Other versions may need changes.

## Install

From the xapk: `config.arm64_v8a.apk` → `lib/arm64-v8a/*.so`, base apk → `assets/`.

```
sdmc:/switch/leveldevil_nx
├── leveldevil.nro
├── liblime.so
├── libApplicationMain.so
└── assets/          (the apk's assets/ folder: assets/data/game.mbs, ...)
```

Get `leveldevil.nro` from the [releases](../../releases) or build it. Launch via title override (hold R while starting an installed game).

## Controls (config.txt, created on first run)

The game's own key map (`config/game-config.json`, inside libApplicationMain.so):
arrows, Up/W/Space, Action1 = Z, Action2 = X, R, Enter, Esc.

| Input | Key |
|---|---|
| Stick, D-pad | Arrows |
| A | Up (jump, same as stick up) |
| B | Space |
| X / Y | Z (Action1) / X (Action2) |
| R | R (restart) |
| + / − | Enter / Esc |
| ZL + ZR | On-screen cursor |
| Touchscreen | Touch (handheld) |

## Build

Needs devkitA64 and `switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau switch-libpng switch-zlib`.

```
make
```

`icon.jpg` is optional and not committed.
