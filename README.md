# Tigryx

Tigryx is a cross-platform game and app engine that is tuned to work on
modern and retro platforms. The aim is to run on every popular platform,
but for now we are rolling out DOS, Windows 95+ and Windows (modern)
engines. Tigryx uses a fast JIT compiler and a highly optimised set of
APIs to allow games to harness modern accelerated frameworks or run on
classic framebuffers.

Find out more at [tigryx.com](https://tigryx.com), and see the
[documentation](https://tigryx.com/#/start/overview) for the language,
API and platform guides.

This repository holds the source code for the Tigryx engine, interpreter,
VM and preprocessor — released under the GNU GPL v3 (see COPYING), with
the linking/runtime exception in EXCEPTION.md. If you want to ship
closed-source executables built with Tigryx, take a look at COMMERCIAL.md.

This is Beta software so will contain bugs, omissions and errors. Please
report any issues to tigryx.com/issues.

## What you get

| Binary        | What it does                                |
|---------------|---------------------------------------------|
| `tigryx`      | Dispatcher (`tigryx run`, `tigryx pp`)      |
| `tigryx-ri`   | Interpreter / bytecode compiler             |
| `tigryx-pp`   | Standalone preprocessor                     |
| `tigryxw` etc.| GUI-subsystem twins (Windows only; win95 also builds `tigryx-riw`/`tigryx-ppw`) |

## Platforms

| Platform directory                    | Target                          | Build system |
|---------------------------------------|---------------------------------|--------------|
| `platforms/windows-opengl-mingw-x64`  | Windows Vista+, x64             | CMake        |
| `platforms/windows-opengl-mingw-x86`  | Windows 2000/XP+, x86           | CMake        |
| `platforms/win95-opengl-mingw-x86`    | Windows 95/98/ME/XP, x86        | Makefile     |
| `platforms/win95-relic3d-mingw-x86`   | Windows 95/98/ME/XP, software 3D| Makefile     |
| `platforms/dos-relic3d-djgpp-x86`     | DOS (DJGPP), x86                | Makefile     |
| `platforms/dos-relic3d-djgpp-i386`    | DOS (DJGPP), i386               | Makefile     |
| `platforms/dos-frelic3d-djgpp-i386`   | DOS (DJGPP), i386, fixed-point variant | Makefile     |

## What you'll need

### Compilers

- **Windows x64** — MSYS2 (https://www.msys2.org/) with
  `mingw-w64-x86_64-gcc` and `cmake`.
- **Windows x86** — MSYS2 with `mingw-w64-i686-gcc` and `cmake`; use the
  MINGW32 shell.
- **Win95/98/ME** — the original MinGW (not MinGW-w64) plus MSYS 1.0
  `make.exe`. The makefile targets `_WIN32_WINNT=0x0400`.
- **DOS (DJGPP)** — a DJGPP cross-compiler or native DJGPP setup
  (`i586-pc-msdosdjgpp-gcc`), plus GNU make.
- PowerShell for the `build.ps1` scripts.

### Drop-in dependencies

A few third-party bits aren't part of the source release — grab them and
drop them where the build expects:

| Dependency                               | Where it goes                                    | Used by |
|------------------------------------------|--------------------------------------------------|---------|
| miniz (`miniz.c`, `miniz.h`)             | `thirdparty/miniz/`                              | `vm.c` (@unzip) |
| `stb_image.h`                            | `include/`                                       | `vm.c` |
| `stb_truetype.h`                         | `include/`                                       | font rendering |
| Allegro 4 for DJGPP (headers)            | `src/backends/allegro-dos/include/`              | DOS backend |
| Allegro 4 for DJGPP (`liballeg.a`)       | `src/backends/allegro-dos/lib/djgpp/`            | DOS backend |
| lwIP 2.2.x source tree                 | `thirdparty/lwip/`                               | optional DOS HTTP (`-DosNet`) |
| dr_libs (`dr_flac.h`, `dr_mp3.h`, `dr_wav.h`) | `thirdparty/audio/`                     | `@music` decoder (`musdec_impl.h`) |
| `stb_vorbis.c`                           | `thirdparty/audio/`                              | `@music` OGG decoder (`musdec_impl.h`) |

### Fonts

Tigryx renders TrueType fonts at runtime. Put any `.ttf` files you want
programs to see next to the built binaries — or in `fonts/` and the win95
makefile will copy them across for you. On DOS a `.fnt` cache file with
the same name wins over the `.ttf` and loads faster, so if you have
pre-baked caches drop those in alongside.

## Building

Each platform has a `build.ps1`:

```powershell
.\platforms\windows-opengl-mingw-x64\build.ps1
.\platforms\windows-opengl-mingw-x86\build.ps1 -WindowsVersion 5.1
.\platforms\win95-opengl-mingw-x86\build.ps1
.\platforms\dos-relic3d-djgpp-x86\build.ps1
.\platforms\dos-relic3d-djgpp-x86\build.ps1 -DosNet   # with lwIP HTTP support
```

Handy flags: `-Config Release|Debug`, `-Clean`, and toolchain overrides
(`-MinGWPath`, `-DJGPPPath`). The DOS build also takes `-DosNet` to build
the in-tree lwIP port (`src/dosnet/`) against a `thirdparty/lwip/` drop-in,
giving the DOS backend real `@http_*` support.

Prefer raw CMake? That works too:

```
cmake -S . -B build -DTIGRYX_BACKEND=opengl-win32 -DTIGRYX_JIT=x64-win
cmake --build build
```

Binaries land in `bin/<platform-id>/`.

| Option             | Default | Effect                                |
|--------------------|---------|---------------------------------------|
| `TIGRYX_BACKEND`   | `auto`  | `opengl-win32` or `allegro-dos`       |
| `TIGRYX_JIT`       | `auto`  | `x64-win`, `x86-win`, `x86-dos`, `null` |
| `TIGRYX_DOSNET`    | `OFF`   | lwIP TCP/IP for DOS `@http_*`         |
| `TIGRYX_PROFILE`   | `OFF`   | `-pg` gprof profiling                 |
| `TIGRYX_PRODUCTION`| `OFF`   | Production splash screen              |

Running the DOS binaries: they're DJGPP programs, so on a machine without
a DPMI host (bare FreeDOS, for example) you'll want CWSDPMI from the DJGPP
distribution.

## Licence

GPLv3 — see COPYING. Linking/runtime exception — see EXCEPTION.md.
Commercial licence — see COMMERCIAL.md.
