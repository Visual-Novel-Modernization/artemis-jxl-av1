# artemis-jxl-av1

Runtime hooks that add **JPEG XL** and **AV1** decoding support to
**Artemis Engine** (ies-net / Daiju Hanaoka) visual novels, so game assets can
be recompressed to roughly half the size.

The engine understands neither format, so both are supplied at runtime:
images are transcoded back to PNG just before the engine hands them to libpng,
and video is decoded by a process-local AV1 decoder MFT that the engine's
existing Media Foundation pipeline picks up on its own.

> **Measured on _Oniichan Continue! Yuuri to Secret Love_ (PANMIMI SOFT)**
>
> | | Before | After | Saved |
> |---|---|---|---|
> | `root.pfs` (images + audio) | 1,462.4 MB | **728.1 MB** | 734.4 MB |
> | `video\op.mp4` (1080p60) | 183.3 MB | **82.6 MB** | 100.7 MB |
> | **Total** | **1,650.8 MB** | **820.4 MB** | **830.4 MB (50.3%)** |
>
> Images: 6,180 PNGs → 5,226 JXL, **0 failures**, down to 19.3% of the
> original size.
> Video: AV1 decode sustained **600 frames / 9.937 s = 60.4 fps**, real-time.

---

## How it works

### Images: detouring the engine's PNG reader

The engine statically links libpng. Disassembly locates its own high-level PNG
reader (in the reference binary: `sub_60BE50`, RVA `0x20BE50`) with this shape:

```c
sub_60BE50(this, image_target, smartptr_out, stream, ctx)
// stream vtable: +20 = read, +24 = seek, +32 = size
```

A 5-byte `jmp` is written over the function entry. The detour then:

1. Rewinds the stream and peeks at the first bytes
2. **If JXL** — decodes with libjxl, re-encodes to an **in-memory PNG**, and
   swaps the `stream` argument on the stack for a shim stream that serves
   those PNG bytes
3. **Otherwise** — passes straight through

It then tail-jumps into a trampoline (the original 5 bytes followed by a `jmp`
back to entry+5).

**Why transcode back to PNG instead of producing pixels directly?** Because
this way colour handling, palettes, alpha and row layout are all inherited from
the engine's own libpng path. Nothing about the target pixel format has to be
guessed, and the result is identical to the original PNG. Measured cost:
0.3–1.2 ms for small images, roughly 50 ms for a full-screen 1080p background.

**Filenames are left unchanged** (still `.png`); only the contents differ. The
engine indexes resources by name, so no scripts or indices need touching — the
hook detects JXL by magic bytes, not by extension.

### Hooking without knowing the calling convention

`sub_60BE50` contains **no `ret` instruction at all** — it has `__try`-protected
smart pointers, so MSVC merged the cleanup into a shared tail reached by `jmp`,
and the real `epilogue + ret N` lives somewhere else entirely. That makes the
calling convention impossible to recover statically: guess `__thiscall` when
it is really `__cdecl` and the stack is corrupted on return.

So `src/thunk.S` does something more robust — it only **swaps the stream
argument on the stack** and tail-`jmp`s through a trampoline:

```asm
_PngLoaderThunk:
    pushal
    pushl   0x2C(%esp)          ; a4 = [esp+0xC] on entry; +32 after pushal
    call    _JxlMaybeSwapStream
    addl    $4, %esp
    testl   %eax, %eax
    je      1f
    movl    %eax, 0x2C(%esp)    ; replace the stream argument
1:  popal
    jmp     *_g_trampEntry      ; tail-jump, never call
```

Nothing is ever `call`ed, so the original function's own `ret N` returns
directly to *its* caller. **The calling convention never has to be known.**

### Video: a process-local AV1 decoder MFT

The engine plays MP4 through a standard Media Foundation topology (source node
plus an EVR renderer node), and the decoder is inserted by MF's **topology
resolver**. Decoders registered with `MFTRegisterLocal` live in the process and
**take priority over registry MFTs**, so the resolver picks ours automatically.

That means no engine playback code needs hooking at all — just implement an
`IMFTransform` (`src/av1_mft.cpp`, backed by dav1d) and register it:

- Input: `MFMediaType_Video` / `MFVideoFormat_AV1`
- Output: `MFMediaType_Video` / `MFVideoFormat_NV12`

The payoff: **no dependency on the Microsoft Store "AV1 Video Extension"**.
On the test machine that extension *is* installed, and the log still shows our
MFT being the one selected.

---

## Requirements

**To build** (MSYS2, mingw32 / i686 environment):

```bash
pacman -S mingw-w64-i686-gcc \
          mingw-w64-i686-libjxl \
          mingw-w64-i686-libpng \
          mingw-w64-i686-dav1d
```

libjxl, libpng and dav1d are linked **statically** (including libstdc++), so
the resulting DLL has zero runtime DLL dependencies.

> The game is a 32-bit PE, so an i686 toolchain is mandatory — the mingw32
> environment already targets i686, which is exactly what this needs.

**To recompress assets** — only needed if you follow the recipe further down.
None of these are bundled; fetch them yourself.

| Tool | Purpose | Source |
|---|---|---|
| `pfs-rs` | unpack / repack Artemis `.pfs` | <https://github.com/sakarie9/pfs-rs/releases> |
| `cjxl` | JPEG XL encoding | <https://github.com/libjxl/libjxl/releases> (`jxl-x64-windows-static.zip`) |
| `HandBrakeCLI` | AV1 encoding | <https://handbrake.fr/downloads2.php> |

Building and using the hooks themselves needs none of them — only MSYS2.

---

## Building

```powershell
# Build the DLL and the launcher
.\build.ps1 all

# Only run the libjxl static-link smoke test
.\build.ps1 test

# If MSYS2 is not in a standard location
.\build.ps1 all -Msys '<MSYS2 root>'
```

Outputs land in `build\`: `artemis_jxl.dll` and `launcher.exe`.

---

## Using it

**Start the game through `launcher.exe`.** It creates the process suspended,
injects the DLL with a remote `LoadLibraryW`, then resumes the main thread — so
the hooks are in place before any game code runs, and DLL search order and
KnownDLL issues never come up.

Put `launcher.exe` and `artemis_jxl.dll` next to the game executable and run
`launcher.exe`. Nothing else about the installation changes.

### How the launcher picks the game

`launcher.exe` resolves its target in two steps:

1. **A path on the command line wins.** The first argument that does not start
   with `-` is taken as the game executable:

   ```
   launcher.exe "<path to the game exe>"
   ```

2. **Otherwise it scans its own directory** for `*.exe` and picks the first
   entry that is not `launcher.exe`. That enumeration comes back alphabetical,
   so a directory holding one game executable resolves correctly every time.

The rest of the command line:

| Argument | Effect |
|---|---|
| `<game exe>` | what to launch — the first argument not starting with `-` |
| `--dll <path>` | inject this DLL instead of `artemis_jxl.dll` beside the launcher |
| `-- <args...>` | everything after `--` is handed to the game |
| any other `-flag` | appended to the game's command line |

**Directories holding more than one `.exe` need the explicit form.** A patched
build, the game's own launcher and a second-language executable all appear in
that list, and step 2 takes whichever sorts first.

---

## Recompressing the assets

The hooks are the interesting part. Producing the JXL and AV1 files is
mechanical, **nothing in the hook depends on how they were made**, and this
repository deliberately ships no scripts for it — the external tools do all the
work. Below is the recipe that produced the numbers above.

**1. Unpack** with [pfs-rs](https://github.com/sakarie9/pfs-rs):

```
pfs-rs extract root.pfs extracted\
```

**2. Convert every PNG in place** with `cjxl` (ships with libjxl):

```
cjxl input.png output.jxl -e 7 -q 90
```

`-e 7` is the effort level, `-q 90` the quality. Together they land at 19.3% of
the original size with no visible loss on 2D art.

Two rules matter here:

- **Keep the `.png` filename.** Write the JXL bytes back to the same path. The
  engine indexes resources by name and the hook detects JXL by magic bytes, so
  neither scripts nor indices need editing.
- **Keep the original PNG whenever the JXL comes out larger.** Icons and
  buttons usually do; without that check the archive can grow.

**3. Repack:**

```
pfs-rs create -o root.pfs extracted\
```

**4. Encode the movie** with `HandBrakeCLI`:

```
HandBrakeCLI -i op.mp4 -o op_av1.mp4 -e svt_av1 -q 32 --encoder-preset 5 -E copy:aac -f av_mp4
```

`-q 32` with `--encoder-preset 5` took the 1080p60 opening from 183.3 MB down to
82.6 MB. `-E copy:aac` passes the audio through rather than encoding it a second
time.

**5. Ship it.** Put the new `root.pfs` and the AV1 `op.mp4` into a copy of the
game directory, alongside `launcher.exe` and `artemis_jxl.dll`. Clear out
`savedata` so your own test saves and read-tracking do not ship with it, and
include a note telling players to start the game through `launcher.exe` rather
than double-clicking the game executable.

---

## Retargeting another game

**The function addresses in this repository are hard-coded for one specific
binary.** A different game — or a different build of the same engine — needs
them located again:

| Constant | File | Meaning |
|---|---|---|
| `RVA_PNG_LOADER` | `src/hook.cpp` | the engine's high-level PNG reader |
| `EXPECTED_PROLOGUE` | `src/hook.cpp` | first 5 bytes of that function, used as a safety check |

How to find it (IDA + Hex-Rays):

1. Search for the PNG signature constant `89 50 4E 47 0D 0A 1A 0A`
   (usually a single occurrence in `.rdata`)
2. Follow cross-references to whatever consumes it
3. Go up one level and find the function that calls **both** `png_sig_cmp`
   and `png_read_info` — that is the engine's PNG reader
4. Confirm its first 5 bytes are `55 8B EC 6A FF`
   (`push ebp; mov ebp, esp; push -1`) with no relative operands among them;
   if not, adjust `PROLOGUE_LEN` accordingly
5. Put the RVA into `RVA_PNG_LOADER`

`EXPECTED_PROLOGUE` is a runtime safety check: if the prologue does not match,
the hook is abandoned and a line is written to the log rather than corrupting
the game.

---

## Known limitations

- **Images and video only; audio (OGG) is untouched.** Recompressing it to Opus
  would require additionally hooking the engine's `artemis::CVorbis` decode
  path.
- **Only MP4 played through Media Foundation is handled.** The engine's
  built-in `artemis::CTheora` path (`.ogv`, typically a few MB of effect
  animations) is out of scope.
- **Conversion is lossy.** The recipe above uses `cjxl -e 7 -q 90`; for
  pixel-exact output use `cjxl -q 100` (lossless) instead.
- **Small images often do not compress.** Icons and buttons tend to get
  *larger* as JXL — keep the original PNG in that case so the archive never
  grows.

---

## Credits

- [pfs-rs](https://github.com/sakarie9/pfs-rs) — Artemis `.pfs` unpack/repack
- [libjxl](https://github.com/libjxl/libjxl) — JPEG XL reference implementation
- [dav1d](https://code.videolan.org/videolan/dav1d) — AV1 decoder
- [HandBrake](https://handbrake.fr/) — video encoding front-end

License terms for the statically linked components are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## License

MIT. See [LICENSE](LICENSE). Copyright (c) 2026 artemis-jxl-av1 contributors.
