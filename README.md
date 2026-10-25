# ES3 webOS probe

A small native app for LG webOS TVs that answers one question: **can this TV run the
[Animal Crossing PC port](https://github.com/flyngmt/ACGC-PC-Port)'s renderer?**

That renderer translates the GameCube's graphics to **OpenGL ES 3** (as in the
[Android port](https://github.com/zwaetschge/ACGC-Android-Port)) and draws everything through
one large "TEV" fragment shader. The webOS toolchain only ships ES 2 headers, and no webOS
homebrew app has been confirmed to use ES 3 yet, so this probe checks it on the TV itself
before any porting work starts.

It contains no game code or data: only the port's two shader files (MIT), compiled as the
game would compile them.

## What it checks

| Section | Checks |
|---|---|
| System | webOS version, SDL build, CPU cores, RAM, drawable and panel resolution, free storage (a full disc image is ~1.4 GB) |
| OpenGL ES context | Whether the TV gives an **ES 3.0** context (falls back to ES 2 to still show results), depth buffer, GPU vendor/renderer/version, GLSL version, uniform and texture-unit limits, `highp` in fragment shaders, extensions (ASTC, S3TC) |
| Shaders | A GLSL ES 3.00 feature test, then the **port's own vertex and TEV fragment shaders**, rewritten to `#version 300 es` exactly as the Android port does, with compile time; whether compiled programs can be saved and reloaded (a disk cache for shader variants) |
| Performance | GPU time per frame (draw + upscale) for a *typical* scene (2 TEV stages, textured, fog, alpha test, 3 layers) and a *worst case* (3 stages, 3 textures, 8 per-pixel lights, indirect texturing, 4 layers). Uses the **specialised shader variants** the port really draws with (built like `pc_gx_tev.c`), at internal resolutions of **1080p, 720p and 480p** scaled up to the screen; the generic uber shader at 1080p is kept as a reference |

Each line is marked **PASS**, **WARN** or **FAIL**; the top line gives the verdict. The game
runs at 30 fps (33.3 ms per frame); the typical scene passes at 60% of the 60 fps budget.
A FAIL at 1080p with a PASS at a lower resolution means the port should render internally at
that resolution and let the TV scale it.

## Using it

1. Install `dist/org.es3webosprobe.app_<version>_arm.ipk`
   (`ares-install -d <tv> dist/org.es3webosprobe.app_0.2.0_arm.ipk`, or webOS Dev Manager).
2. Open **ES3 Probe** on the TV. The checks and benchmark take about half a minute.
3. Read the results on screen, or fetch the text report: its path is shown at the bottom
   (`$HOME/es3-probe-report.txt`, the app's data folder).

**OK** runs everything again; **Back** quits.

## Building

The webOS toolchain is Linux x86_64 only (on a Mac, use a Linux VM or an x86_64 container).

```sh
# Toolchain: https://github.com/openlgtv/buildroot-nc4/releases (run its relocate-sdk.sh once)
export WEBOS_SDK=$HOME/arm-webos-linux-gnueabi_sdk-buildroot
npm install -g @webos-tools/cli      # ares-package
scripts/build-webos.sh               # -> dist/org.es3webosprobe.app_<version>_arm.ipk
```

Desktop build (needs SDL2 and an ES 3 driver such as Mesa):

```sh
cmake -B build -G Ninja && cmake --build build
./build/es3probe
# headless: save the results screen and exit
SDL_VIDEODRIVER=offscreen PROBE_SCREENSHOT=/tmp/probe.bmp ./build/es3probe
```

## How it works

- GL functions are loaded at run time with `SDL_GL_GetProcAddress` (`src/GlFunctions.*`), using
  the Khronos ES 3 headers for types only, so nothing is linked against the sysroot's ES 2
  library. A function the driver lacks is reported instead of crashing.
- The results screen uses GLSL ES 1.00 and an embedded public-domain 8x8 font
  (`src/TextRenderer.*`, `src/FontData.cpp`), so it works in an ES 2 fallback context too.
- The port's shaders are embedded at build time (`third_party/acgc-shaders/`,
  `cmake/EmbedTextFile.cmake`).
- SDL is webosbrew's [SDL-webOS](https://github.com/webosbrew/SDL-webOS) 2.30 build, bundled in
  the IPK (remote Back/Exit delivery, panel resolution).

## Licence

MIT (see `LICENSE`). Third-party parts are listed in `THIRD_PARTY_NOTICES.md`.
Animal Crossing is a trademark of Nintendo; this project is not affiliated with Nintendo.
