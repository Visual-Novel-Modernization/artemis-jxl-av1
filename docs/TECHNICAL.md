# Technical notes

How the hook points were found, why the design ended up like this, and what
went wrong on the way. Most of this is about the mistakes rather than the code,
which is why it is worth reading before retargeting another Artemis title.

---

## 1. Finding something to hook

The engine statically links libpng, so the first job was locating its PNG
reader. Searching for the 8-byte PNG signature leads to a function that looks
promising and is not: it is the engine's own tEXt/IEND chunk walker, the thing
that reads the metadata sitting next to every `.png` as a same-named `.csv`.
That detour cost an afternoon.

The productive route was libpng itself. The version string
(`pplication built with libpng-`) pins down the library, and from there:

- `sub_4EFC60` is `png_read_info` — its chunk loop
  (IHDR/IDAT/IEND/PLTE/bKGD/cHRM/gAMA…) is unmistakable
- `sub_4EBFD0` is `png_sig_cmp`

Whichever function calls *both* of those is the engine's PNG loader:
**`sub_60BE50`**, RVA `0x20BE50`, 994 bytes. It reads a PNG out of a stream
object and writes it into an image target object:

```c
char __thiscall sub_60BE50(_BYTE *this, int a2, int a3, int a4, int a5)
{
  (*(vtable+24))(a4, 0, 0);            // stream->seek(0, 0)
  (*(vtable+20))(a4, buf, 16);         // stream->read(buf, 16)
  if (sub_4EBFD0(4)) goto fail;        // png_sig_cmp
  if (v32 != 'IHDR') goto fail;
  png  = sub_4EFC00();                 // png_create_read_struct
  info = sub_4EC490(png);              // png_create_info_struct
  sub_4EFC60(png, info);               // png_read_info
  sub_4EE370(...);                     // transforms, per color_type
  (*(a2+4))(w, h, fmt, 0);             // target: prepare w x h as format fmt
  rows = malloc(4*h);
  for (i...) rows[i] = (*(a2+48))(a2, i);   // target: row pointer for row i
  sub_4F04A0(png, rows);               // png_read_image
  sub_4F0570(png);                     // png_read_end
  (*(a2+64))(a2);                      // target: commit
}
```

The thing that makes all of this work is the stream object `a4`'s vtable:

| Offset | Method |
|---|---|
| `+0x14` | `read(self, buf, n) -> bytes read` |
| `+0x18` | `seek(self, offset, whence)` |
| `+0x20` | `size(self) -> total length` |

Swap that object for one of ours and we own everything the PNG reader sees.

---

## 2. The function with no `ret`

Scanning `sub_60BE50` for a `ret` returns nothing. Not one, in the whole
function.

It has `__try`-protected smart pointers, and MSVC merged the cleanup into a
shared tail reached by `jmp`, so the real `epilogue + ret N` lives somewhere
else — possibly attributed to a neighbouring function entirely. That means the
calling convention cannot be recovered statically, and guessing between
`__thiscall` and `__cdecl` is how you end up with a stack that is quietly
wrecked on return.

The way out was to stop trying to *call* the original and just *jump* to it.
Its entry is unusually cooperative:

```
55          push ebp
8B EC       mov  ebp, esp
6A FF       push -1
```

Five bytes, no relative operands, a perfect instruction boundary. So those five
bytes move into a trampoline, a 5-byte `jmp` goes over the entry, and the stub
that lands there does one thing — replace the stream argument on the stack:

```asm
_PngLoaderThunk:
    pushal
    pushl   0x2C(%esp)          ; a4 = [esp+0xC] on entry; +32 after pushal
    call    _JxlMaybeSwapStream
    addl    $4, %esp
    testl   %eax, %eax
    je      1f
    movl    %eax, 0x2C(%esp)
1:  popal
    jmp     *_g_trampEntry
```

Nothing is ever `call`ed, so the original's own `ret N` hands the result
straight back to *its* caller. Return value, stack cleanup and SEH all stay
untouched, and the calling convention never has to be known.

The trampoline has to live in executable memory. It was a
`static unsigned char g_tramp[32]` in `.data` at first, and jumping there took
a DEP violation. The fault offset was exactly `g_tramp`'s address, which is the
fastest way to recognise it; `VirtualAlloc` with `PAGE_EXECUTE_READWRITE` fixes
it.

---

## 3. Getting JXL into the engine

Detection is by magic bytes — `FF 0A` for a bare codestream,
`00 00 00 0C 4A 58 4C 20` for a container. Extensions are not consulted, which
is why the shipped archives can keep `.png` entry names while holding JXL
payloads.

On a hit we decode with libjxl and then **re-encode to an in-memory PNG**, which
gets handed to the original function through a shim stream. That extra encode
looks wasteful, and it is deliberate.

The alternative — decode straight to the pixels the engine wants — means
reverse-engineering the target object's expected format, row stride, palette
expansion and alpha handling. Going through PNG means the engine's own libpng
path does all of that, exactly as before, and nothing about the output format
has to be guessed. The cost is one deflate pass with compression level 1 and
filtering off, which measures at 0.3–1.2 ms for small images and around 50 ms
for a full-screen 1080p background. Buffers are reused through a `__thread`
slot so nothing leaks.

Skipping the re-encode would save that 50 ms and put 1,210 paletted images at
risk of silent colour or alpha corruption. We took the 50 ms.

---

## 4. Video, without touching the player

The engine plays MP4 through a normal Media Foundation topology: source node
plus an EVR renderer node, built in `sub_5EAF60`. Crucially, **it never creates
a decoder node** — MF's topology resolver inserts one.

That is the whole opportunity. `MFTRegisterLocal` registers a decoder in the
process, and local MFTs outrank registry MFTs, so the resolver picks ours
without a single byte of engine playback code being patched. We implement
`IMFTransform` on top of dav1d (`src/av1_mft.cpp`) and register it for
`MFVideoFormat_AV1` in, `MFVideoFormat_NV12` out.

Worth confirming on the test machine: the Microsoft Store AV1 extension *is*
installed there, and the log still shows our MFT being instantiated. So the
fallback genuinely works, rather than being masked by a system decoder.

Registration happens on a thread spawned from `DllMain`, because `MFStartup`
and `MFTRegisterLocal` both want COM and `LoadLibrary` must not run under the
loader lock. The launcher starts the process suspended, so that thread has
plenty of time before anything plays.

---

## 5. The same problem on two engines

We have now solved this twice, for two engines whose media APIs sit at opposite
ends of the spectrum. The comparison is the most reusable part of these notes.

**AdvHD (WillPlus / RioShiina)** plays video through **DirectShow**, which
offers no way to add a decoder without taking part in graph construction. So
that project hooks the engine's graph building (`sub_4CC910`) and intercepts
`AddSourceFilter` to substitute LAVSplitter. Substituting the splitter changes
what the engine's own code sees: on ASF sources its `FindPin(L"Output")` fails,
so it falls back to `RenderFile` and renders both streams — and once LAVSplitter
makes that `FindPin` succeed, the engine takes the single-pin path and leaves
audio unconnected, which plays a silent movie. `Hook_AddSourceFilter` walks the
splitter's output pins and pre-renders any pin named `"Audio"` before returning.
Its textures were the easy half: they hang off
`D3DXCreateTextureFromFileInMemoryEx`, a public D3DX9 API.

**Artemis (this project)** plays video through **Media Foundation**, which is a
far friendlier host — the topology resolver inserts decoders itself, and
`MFTRegisterLocal` is an official way to offer one, so no engine playback code
is patched at all. Its textures are the hard half, because nothing public sits
in that path: the engine calls its own PNG loader directly, so the hook lands
on an internal function and needs a per-binary RVA.

Both engines look assets up by name, so both projects keep the `.png` entry
name and put JXL bytes inside, and both detect the format by magic bytes.

The practical consequence is what decodes the video. AdvHD gets ffmpeg by way
of LAV, which covers AV1, Opus and a great deal more; Artemis gets an MFT we
wrote against dav1d, which covers AV1 alone. Where several codecs are in play,
bringing a decoder suite wins, and pulling in LAV cost that project nothing at
build time. Writing our own here was tractable because AV1 was the only target
— and it keeps this side to a single DLL, since libjxl, libpng and dav1d all
link statically rather than being resolved at runtime alongside a decoder
suite.

---

## 6. Things that cost real time

**A compile define that is not in any of the docs.** Static-linking libjxl
fails with `undefined reference to '_imp__JxlDecoderVersion'` until
`JXL_STATIC_DEFINE` is defined; without it the headers expand to `dllimport`.
Chasing `libjxl_cms.a` afterwards is a dead end, since it was built against hwy
as a DLL import and will not link against `libhwy.a`.

**`-m32` against a mingw32 toolchain.** The environment already targets i686,
and the flag makes GCC fail with no output at all.

**Three AV1 MFT bugs, all presenting as "audio but no video":**

1. The output sample was over-released. `MFCreateSample` returns a refcount of
   1; writing the pointer into `MFT_OUTPUT_DATA_BUFFER.pSample` hands ownership
   to the caller, and releasing it again destroys the sample the renderer is
   about to read.
2. `dav1d_send_data` returning `EAGAIN` means the decoder did *not* take the
   data. Discarding it, as the first version did, means the decoder never
   assembles a complete unit and emits nothing at all.
3. `SetSampleTime` without `SetSampleDuration` also produces no picture. The
   duration comes from `MF_MT_FRAME_RATE`: `10^7 * den / num`, in 100 ns units.

The related lifetime trap: with `dav1d_data_wrap(..., free_cb = NULL)` you own
the buffer until the frame comes out. `dav1d_data_create` makes dav1d own it
and removes the problem for one memcpy.

---

## 7. Numbers

Images, `cjxl -e 7 -q 90` over 6,180 PNGs: 5,027 converted, 199 already done,
954 kept as PNG, 0 failures. **888.2 MB → 171.2 MB (19.3%)**. The 954 are
mostly icons and buttons, where the JXL container's few dozen bytes of overhead
does not pay for itself — keeping the original there is the difference between
a smaller archive and a larger one.

Video, H.264 1080p60 15 Mbps going to SVT-AV1:

| CRF | preset | Result | Time |
|---|---|---|---|
| 28 | 6 | 183.3 → 130.0 MB | 1.6 min |
| 32 | 5 | 183.3 → 82.6 MB | 2.1 min |

Audio passes through with `copy:aac`, so it is not encoded twice.

At runtime, image loads cost 0.3–1.2 ms for small assets and about 50 ms for a
full-screen background. AV1 playback held 60.4 fps over 600 frames
(600 frames in 9.937 s), which is real-time.

Archive: 1,533,456,521 → 763,457,983 bytes, **734.4 MB saved, 50.2%**.

One caveat on the repack: pfs-rs round-trips are **not** byte-identical. The
data offsets in the index change because it lays members out in a different
order. File lists, file counts and total size match, and the game runs — the
engine looks members up through the index and does not care about physical
order.

---

## 8. Limits

Audio (522.8 MB of OGG) is untouched. The engine decodes it with its own
built-in `artemis::CVorbis`, so that is a separate hook, the way the AdvHD
project handles Opus.

`.ogv` is untouched too. Those go through the engine's built-in
`artemis::CTheora`, they are small (2.2 MB total), and they carry alpha
semantics that AV1 does not reproduce without extra work.

The addresses are hard-coded for one binary. `RVA_PNG_LOADER` and
`EXPECTED_PROLOGUE` in `src/hook.cpp` have to be re-derived per title; the
README says how. The prologue check makes a mismatch fail loudly instead of
corrupting the game, which is what makes hard-coded addresses defensible here.
