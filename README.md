# Level Devil — Nintendo Switch port (Defold build)

Loads the ARM64 Android build of *Level Devil* **1.4.1** (`com.unept.leveldevil`,
version code 192 — the Defold release with all the newer levels) and plays the
part of Android around it: a NativeActivity lifecycle, ALooper/AInputQueue, the
NDK asset manager, OpenSL ES audio over audout, EGL/GLES via switch-mesa, and a
fake JNI for the Java side. It contains **no game code and no game assets**.

Tested and working on hardware: menus, levels, sound, controls and saves.

> The older Stencyl build of the game (1.9.0, fewer levels) is supported by
> release [v1.0.0](../../releases/tag/v1.0.0) of this repository.

The loader base (bionic shims, so_util, JNI runtime, window, cursor) comes from
ChanseyIsTheBest's Stencyl ports ([poorbunny_nx](https://github.com/ChanseyIsTheBest/poorbunny_nx),
[heartstar_nx](https://github.com/ChanseyIsTheBest/heartstar_nx), MIT). The
Defold/NativeActivity layer is new.

## Install

Get `leveldevil.nro` from the [releases](../../releases), then take the game
files from your own copy of the game. From an `.apks`/`.xapk` split set:

- `split_config.arm64_v8a.apk` → `lib/arm64-v8a/libLevelDevil.so`
- `base.apk` → the `assets/` folder (`game.arcd`, `game.arci`, `game.dmanifest`, `game.projectc`, ...)

```
sdmc:/switch/leveldevil_nx
├── leveldevil.nro
├── libLevelDevil.so
└── assets/
```

Launch via title override (hold R while starting an installed game). Saves go
to `save/` next to the `.nro`.

## Controls (config.txt, written on first run)

| Input | Key |
|---|---|
| Stick, D-pad | Arrows |
| A | Up (jump) |
| B | Space |
| + / − | Enter / Esc |
| ZL + ZR | On-screen cursor (stick moves, A taps) |
| Touchscreen | Touch |

Everything is remappable in `config.txt`. `log_level = 1` writes
`leveldevil.log` next to the `.nro`; `frame_stats = 1` adds an fps line every
5 seconds.

## Notes

- Gameplay runs at 60 fps. Loading a level pauses for about a second: LuaJIT
  runs as an interpreter, because homebrew heap memory can't be made
  executable, so its JIT is refused and it falls back cleanly.
- Ads, IAP, Firebase and Play Games live on the Java side. The port answers
  the way a phone without Play services would: sign-in fails, the ads SDK
  reports ready but never loads an ad, the store has no products.

## Build

devkitA64 with `switch-dev switch-mesa switch-libdrm_nouveau switch-libpng switch-zlib`, or:

```
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make
```
