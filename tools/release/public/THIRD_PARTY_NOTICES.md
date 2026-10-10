# Third-party notices

Bloodborne VR is free software. Its own code is **GPL-2.0-or-later** (see `LICENSE`), except
where a file says otherwise (for example `gpu/xr_bridge/` is MIT). Third-party components keep
their own licences, listed here. This is a good-faith summary, not legal advice.

## Licence of binary releases

Source files that say "GPL-2.0-or-later" may be combined with code under **Apache-2.0**
(OpenSSL 3, the OpenXR loader, abseil, libc++) only under **GPLv3** terms: Apache-2.0 is not
compatible with GPL-2.0-only. The release binaries link those libraries, and the Windows
FFmpeg build of MSYS2 is itself GPLv3 (`--enable-gpl --enable-version3`). Therefore **the
binary release zips are distributed under the GNU GPL version 3** (`COPYING.GPL-3.0`), chosen
through the "or any later version" option of the source files. The source repository stays
"GPL-2.0-or-later". Nobody gets fewer rights: every recipient may rebuild from this source.

## Project code and vendored sources

| Component | Where | Licence |
|---|---|---|
| shadPS4 (emulator core, HLE libraries, shader recompiler, video core) | `gpu/shadps4/` | GPL-2.0-or-later (plus a few MIT, Zlib, CC0, BSL-1.0, BSD-3-Clause files, marked in each header) |
| bbport (native PS4 loader / host) and the original Windows port | `src/`, `gpu/shim/`, `scripts/` | GPL-2.0-or-later; credits: deadinside28 (bbport), Supermedo (Windows port), shadPS4 team |
| OpenXR bridge and VR sandbox | `gpu/xr_bridge/` | MIT (bbport contributors) |
| Dear ImGui | `gpu/third_party/imgui` (submodule) | MIT |
| magic_enum | `gpu/third_party/magic_enum` (submodule) | MIT |
| miniz | `gpu/third_party/miniz` (submodule) | MIT |
| xbyak | `gpu/third_party/xbyak` (submodule) | BSD-3-Clause (`COPYRIGHT`) |
| LibAtrac9 | `third_party/LibAtrac9` (submodule) | MIT |
| FSR-Vulkan (patched, see `gpu/patches/fsr-vulkan/`) | `gpu/third_party/fsr-vulkan` (submodule) | MIT |
| half | `gpu/third_party/half` | MIT |
| cpp-httplib | `gpu/third_party/httplib` | MIT |
| sirit, SPIRV-Headers | `gpu/third_party/sirit` | BSD-3-Clause; Khronos MIT-style |
| wepoll | `gpu/third_party/wepoll` | BSD-2-Clause |
| DejaVu Sans font | `gpu/third_party/fonts` | Bitstream Vera / DejaVu free licence (`LICENSE-DejaVu.txt`) |
| AMD GCN headers | `gpu/third_party/gcn` | MIT-style (AMD) |
| nlohmann/json 3.12.0 | `packaging/windows/licenses/` | MIT |
| Community patches (frame rate, effects) in `patches/Bloodborne.xml` | `patches/` | Authors named in each entry (illusion, Kyo and others), from the public shadPS4 patch collections; the VR hook entries are this project's |

## Runtime libraries shipped in the Windows release zip (`bin/`)

Copied unmodified from the MSYS2 CLANG64 environment; their licence texts are in `licenses/`.

| Library | Licence |
|---|---|
| SDL3 | zlib |
| OpenXR loader (`libopenxr_loader.dll`) | Apache-2.0 (Khronos) |
| OpenSSL 3 (`libssl`, `libcrypto`) | Apache-2.0 |
| protobuf, abseil | BSD-3-Clause, Apache-2.0 |
| LLVM libc++, libunwind | Apache-2.0 with LLVM exception |
| libwinpthread | MIT / BSD-style (MinGW-w64) |
| zlib, xxhash, Zydis | zlib, BSD-2-Clause, MIT |
| FFmpeg (libav*, libsw*) | MSYS2 build: GPL-3.0-or-later; use a slim LGPL build with `make_release.py --ffmpeg-dir` to avoid the codec DLLs |
| Embeddable Python 3.12 (`python/`) | Python Software Foundation License (`python/LICENSE.txt`) |

The Vulkan loader (`vulkan-1.dll`) is not shipped: it comes with your graphics driver.

## Not included, on purpose

* **No game files.** No eboot, executables, models, textures, audio, parameters or extracted
  assets. The first-run helper builds the VR menu / hit-shape data from the user's own dump on
  their PC.
* **NVIDIA DLSS** (`nvngx_dlss.dll`) and **AMD FSR 4 models**: their redistribution terms are not
  cleared, so neither is in the repository or the release. `tools/fsr4cap/` builds FSR 4.1.1
  assets from the user's own AMD DLLs; the result is not to be redistributed.
* **Online / co-op services.** Out of scope for this alpha.

## Trademarks

PlayStation, PS4 and Bloodborne are trademarks of their owners. This project is not
affiliated with, endorsed or sponsored by Sony Interactive Entertainment or FromSoftware.
