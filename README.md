# iPBP

Convert PSP UMD images between **ISO** and PSN-style **EBOOT.PBP** (`NPUMDIMG`), as a C/C++ library and a command-line tool, and PlayStation discs to and from PSOne EBOOTs (see [PSOne EBOOTs](#psone-eboots)).

iPBP replaces LMAN's *ISO~PBP Converter* (2015), which is no longer available. That program was a Windows GUI built on two native tools, **sign_np 1.0.4** (Hykem, mod by LMAN) for ISO→PBP and **npdpc_mod** (tpu, mod by LMAN) for PBP→ISO. iPBP reimplements both from their binaries, and the formats are cross-checked against the originals:

| Direction | Check |
|---|---|
| sign_np PBP → iPBP ISO | bit-identical to the source ISO |
| iPBP PBP → npdpc ISO | bit-identical (compressed and uncompressed) |
| iPBP PBP layout | same PBP offsets, DATA.PSP, NPUMDIMG header fields and table format as sign_np |
| ECDSA signatures | iPBP verifies sign_np's signatures, and its own signatures verify against sign_np's key |

Compared with the original:

* **Cancellable.** Any conversion can be stopped from another thread or with Ctrl+C. It stops within milliseconds and the partial output file is deleted.
* **Multi-threaded** LZRC compression, block encryption and decryption.
* Optional **max compression** (`--max`, `IPBP_COMPRESS_MAX`): the same LZRC format, chosen by its actual coding cost (optimal parsing) instead of greedy matching, about 1–2% smaller on compressible data.
* **Portable.** C++17 with no dependencies. Builds on Windows, Linux, macOS, Android and iOS, and paths are UTF-8 (Unicode file names work on Windows too).
* **Embeddable.** Supports streams, so a host such as an emulator can convert from its own virtual file system.

## Command line

```
ipbp game.iso                       # -> game.PBP   (direction is auto-detected)
ipbp EBOOT.PBP                      # -> ULUS-12345.ISO (named after the disc id, like the GUI)
ipbp game.iso out/                  # -> out/EBOOT.PBP
ipbp iso2pbp -s boot.png game.iso   # with a boot screen (480x272 PNG)
ipbp pbp2iso EBOOT.PBP game.iso
ipbp info EBOOT.PBP                 # title, content id, version key, block layout
ipbp check game.iso                 # good dump / overdump / underdump
```

| Option | |
|---|---|
| `-n, --no-compress` | store blocks uncompressed (default: LZRC, kept when a block shrinks to ≤ 90%) |
| `-m, --max` | smallest LZRC: optimal parsing instead of greedy, about 5–10× slower. Still plain LZRC that real PSPs decode, but not byte-identical to sign_np |
| `-c, --content-id ID` | `XXYYYY-AAAABBBBB_CC-DDDDDDDDDDDDDDDD` (default `UP0000-<DISC_ID>_00-0000000000000000`) |
| `-k, --version-key HEX` | 32 hex digits (default: the NPDRM fixed key, NP flags `0x01000003`) |
| `-s, --startdat FILE` | boot screen PNG |
| `--seed N` | reproducible output (fixed header key and signature nonces) |
| `--no-verify` | PBP→ISO: skip per-block MAC checks |
| `--trim` | PBP→ISO: cut the image at the ISO9660 volume size |
| `-t, --threads N` | worker threads (default: CPU count) |
| `-f, --force` / `-q, --quiet` | overwrite output / no progress |

Exit codes: `0` ok, `1` conversion failed, `2` usage error, `3` bad dump (`check`), `130` cancelled.

## Library

### C++

```cpp
#include <ipbp/ipbp.hpp>

ipbp::Callbacks cb;
cb.progress = [](uint64_t done, uint64_t total) { /* update UI */ };
cb.log      = [](const std::string& line)        { /* optional */ };
// keep a copy of cb.cancel anywhere; cb.cancel.cancel() from any thread aborts the job

ipbp::Result r = ipbp::isoToPbp("game.iso", "EBOOT.PBP", {}, cb);
if (r.status == ipbp::Status::Cancelled) { /* output already removed */ }
else if (!r) std::puts(r.message.c_str());

ipbp::PbpToIsoOptions o;  o.threads = 4;
r = ipbp::pbpToIso("EBOOT.PBP", "game.iso", o, cb);
```

The stream overloads (`isoToPbp(InputStream&, OutputStream&, ...)`) let you plug in your own I/O. The path overloads write to `<output>.part` and rename it when the conversion succeeds, so a failed or cancelled run never leaves a half-written file behind.

### C

```c
#include <ipbp/ipbp.h>

ipbp_job* job = ipbp_job_create();
ipbp_job_set_progress(job, on_progress, ctx);
/* from another thread: ipbp_job_cancel(job); */
int rc = ipbp_iso_to_pbp(job, "game.iso", "EBOOT.PBP", NULL);   /* NULL = default options */
if (rc != IPBP_OK) printf("%s\n", ipbp_job_error(job));
ipbp_job_destroy(job);
```

`ipbp_iso_to_pbp_reader()` reads the ISO through an `ipbp_reader` and `ipbp_pbp_to_iso_writer()` writes it through an `ipbp_writer` (since 1.2.0), so a host can keep the ISO in memory, e.g. to repack an EBOOT as CSO or CHD without a temporary file. `ipbp_iso_to_pbp_io()` (since 1.3.0) also writes the PBP itself through an `ipbp_writer`, e.g. to build it in memory and store it in an archive.

## PSOne EBOOTs

Since 1.4.0 iPBP also converts PlayStation discs to and from PSOne EBOOT.PBPs (`PSISOIMG0000`, or
`PSTITLEIMG000000` for up to five discs), in popstation's layout. That layout is what PS1 emulators
(DuckStation, Beetle PSX, PCSX-ReARMed), Adrenaline and PSP firmware read.

- **In:** a raw image of 2352-byte frames with every track back to back (a single `.bin`), and its
  track list. Parsing cue sheets, CloneCD, CHD and similar formats is left to the caller (ISOPackager uses
  CHDmodern for that). The disc ID comes from `SYSTEM.CNF`. Blocks are deflated in parallel, at
  level 9 by default.
- **Out:** `readPsxInfo` returns each disc's ID, length and tracks, and `pbpToPsx` writes a disc back
  as the same raw image.
- **Real PSP:** it also needs DATA.PSP and STARTDAT. These are taken from a PSOne Classic EBOOT given as
  `basePbpPath`. iPBP doesn't ship one, since it's Sony's code. Emulators don't need it.
- **zlib:** this part needs it. CMake uses `IPBP_ZLIB_TARGET` when that names a target, and
  `find_package(ZLIB)` otherwise. Without zlib, these functions return `Unsupported`.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release      # self-contained, no game data needed
```

CMake options: `IPBP_BUILD_CLI` (ON), `IPBP_BUILD_TESTS` (ON), `IPBP_SHARED` (OFF, builds a DLL / .so with the C API exported). The library file is named `iPBP` on every platform (`iPBP.dll`, `iPBP.so`, `iPBP.dylib`, `iPBP.lib`, `iPBP.a`). To embed iPBP in another CMake project, use `add_subdirectory(iPBP)` and `target_link_libraries(app PRIVATE ipbp)`.

### Platforms

CI builds all of these on every push:

| Platform | Architectures | Shared library |
|---|---|---|
| Windows (MSVC, CMake or the solution) | x86, x64, ARM64 | `iPBP.dll` (+ `iPBP.lib`) |
| Linux | x64 | `iPBP.so` |
| macOS | arm64, x86_64 (universal) | `iPBP.dylib` |
| Android (NDK, API 21+) | arm64-v8a, armeabi-v7a, x86_64, x86 | `iPBP.so` |
| iOS (12.0+) | arm64 | `iPBP.dylib` |

```
# Android, one ABI per build tree
cmake -S . -B build-android-arm64 -DCMAKE_BUILD_TYPE=Release -DIPBP_SHARED=ON -DIPBP_BUILD_CLI=OFF -DIPBP_BUILD_TESTS=OFF \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21
# iOS (on a Mac with Xcode)
cmake -S . -B build-ios -DCMAKE_BUILD_TYPE=Release -DIPBP_SHARED=ON -DIPBP_BUILD_CLI=OFF -DIPBP_BUILD_TESTS=OFF \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
# macOS universal
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DIPBP_SHARED=ON "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
```

On iOS a `.dylib` can only be loaded if it is signed together with the app (or on a jailbroken / TrollStore device); link the static `iPBP.a` into the app when that isn't possible.

### Visual Studio

Open `msvc/iPBP.sln` (Visual Studio 2022 or newer with the "Desktop development with C++" workload; it uses whichever toolset is installed). Platforms x86, x64 and ARM64, configurations Debug and Release. Everything goes to `build/msvc/<Platform>/<Configuration>/`:

| Project | Output |
|---|---|
| `iPBP` | `iPBP.lib`, the static library |
| `iPBP_dll` | `dll/iPBP.dll` and its import library `dll/iPBP.lib`, exporting the C API |
| `ipbp_cli` | `ipbp.exe` |
| `ipbp_tests` | `ipbp_tests.exe` |

The CRT is linked statically (`/MT`), so `iPBP.dll` and `ipbp.exe` don't need the Visual C++ redistributable. From the command line: `msbuild msvc\iPBP.sln /p:Configuration=Release /p:Platform=x64`.

## Format notes

* **PBP**: `PARAM.SFO` (DISC_ID taken from the content id, CATEGORY set to `EG`), ICON0/ICON1/PIC0/PIC1/SND0 copied from `PSP_GAME`, then `DATA.PSP`, which holds the content id, NP flags and an ECDSA signature, plus an optional `STARTDAT` boot image.
* **DATA.PSAR**: a 0x100-byte `NPUMDIMG` header (ECDSA-signed, partly AMCTRL-encrypted), followed by a table of 0x20-byte entries (MAC, offset, size and flags, scrambled), followed by 32 KiB blocks (16 sectors each). Each block is LZRC-compressed if that saves at least 10%, then encrypted with AMCTRL (BBCipher, then BBMac type 3).
* The version key is either the NPDRM fixed key derived from the content id, or one you supply. When reading, the key is recovered from the header MAC, so any NPUMDIMG PBP converts back, including official PSN releases. PS1 (`PSISOIMG`) EBOOTs are not supported.

## License

GPL-3.0, see [LICENSE](LICENSE).

## Credits

Hykem (sign_np), tpu (npdpc, LZRC/AMCTRL reverse engineering), LMAN (mods and the original ISO~PBP Converter GUI).
