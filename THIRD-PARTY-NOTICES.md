# Third-party components

The build output (`artemis_jxl.dll`) **statically links** the components below.
When distributing it in binary form, their copyright and license notices must
be preserved alongside it.

The source code in this repository (`src/`, `*.ps1`) contains none of their
code.

| Component | Used for | License |
|---|---|---|
| [libjxl](https://github.com/libjxl/libjxl) | JPEG XL decoding | BSD-3-Clause |
| [Highway](https://github.com/google/highway) (`libhwy`) | libjxl SIMD abstraction | Apache-2.0 |
| [Brotli](https://github.com/google/brotli) | libjxl entropy coding | MIT |
| [libpng](http://www.libpng.org/pub/png/libpng.html) | JXL → in-memory PNG transcoding | libpng-2.0 |
| [zlib](https://zlib.net/) | libpng dependency | zlib |
| [dav1d](https://code.videolan.org/videolan/dav1d) | AV1 decoding (MFT backend) | BSD-2-Clause |

Linked at runtime (operating system components, not distributed with this
repository):

| Component | Used for |
|---|---|
| Windows Media Foundation (`mfplat` / `mfuuid`) | video pipeline |
| `kernel32` / `ole32` / `user32` / `shell32` | Windows API |

## External programs used by the workflow

The recompression recipe in the README uses the following **standalone
programs**. They are **not** distributed with this repository — obtain them
separately, and their own license terms apply.

| Program | Purpose | Source |
|---|---|---|
| `pfs-rs` | unpack / repack Artemis `.pfs` | <https://github.com/sakarie9/pfs-rs> |
| `cjxl` | JPEG XL encoding | <https://github.com/libjxl/libjxl> |
| `HandBrakeCLI` | AV1 encoding | <https://handbrake.fr/> |

When shipping a binary release, include the LICENSE texts of libjxl, dav1d,
libpng and the other statically linked projects. The MSYS2
`mingw-w64-i686-*` packages usually carry them, and they can also be found
under each project's `share/licenses/` directory.

## About the game itself

This repository contains **no** game assets, no `root.pfs`, no game
executables, and no disassembly output. Users must hold their own legally
obtained copy of the game.

This project is not affiliated with the developers of Artemis Engine
(Daiju Hanaoka / Mikage, ies-net) or with any game publisher.
