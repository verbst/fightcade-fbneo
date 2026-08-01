# groovymister — vendored Groovy MiSTer UDP client

The maintained C++ client for the Groovy MiSTer protocol: video, audio and input streaming to a
DE10-Nano running the Groovy core, which scans our pixels out to an analog CRT.

Consumed from `src/burner/win32/groovy/`. We link the C++ class (`GroovyMister`) directly.

## Provenance

| | |
|---|---|
| Upstream project | `Groovy_MiSTer`, branch `proto/nlc-B`, directory `api/` |
| Imported from    | `/mnt/c/git/Groovy_MiSTer/api/` (tip `c5b4222`, "82") |
| Imported on      | 2026-07-24 |
| Local delta      | none — straight file copy |

**The upstream goal is zero delta.** Every patch the RPCS3 and PCSX2 forks carried has been absorbed
upstream, so a re-sync should stay a plain file copy. If a change turns out to be needed here,
request it upstream rather than editing this directory.

## What was and was not imported

Imported: `groovymister.cpp/.h`, `nlc_codec.cpp/.h`, `rio.h`, and the whole `lz4/` directory.

- `nlc_codec.*` is **required even if NLC is never enabled** — `groovymister.cpp` includes it
  unconditionally.
- `rio.h` supplies Windows Registered I/O declarations for toolchains whose SDK lacks them. It is
  guarded by `#if defined(_WIN32) && !defined(RIO_INVALID_CQ)`, so it is harmless everywhere.
- `lz4/` **is** imported, because FBNeo does not link LZ4 anywhere else in the tree. Accordingly we
  **do not define `GM_SYSTEM_LZ4`**, and `groovymister.cpp` takes its `#include "lz4/lz4.h"` branch,
  which matches this layout as-is.

Not imported: `groovymister_wrapper.cpp/.h` (the flat C `gmw_*` API and dlopen shims — unnecessary
when linking the C++ class statically), and `test*`.

## Build

```
sources : groovymister.cpp  nlc_codec.cpp  lz4/lz4.c  lz4/lz4hc.c  lz4/xxhash.c
defines : (none required — specifically NOT GM_SYSTEM_LZ4)
libs    : ws2_32   (already linked by fbneo_vs2015.vcxproj)
```

C++11 is sufficient. No exceptions, no RTTI. On Windows the send path uses RIO, whose entry points
are fetched at runtime via `WSAIoctl`; this is internal and the API is identical to the POSIX path.

## ⚠ Do not include `groovymister.h` from FBNeo translation units

`groovymister.h` includes `<winsock2.h>`. `src/burner/win32/main.cpp:23` includes the **Winsock 1.1**
`<winsock.h>`, and `burner_win32.h` pulls in `<windows.h>`. Under **MSVC** — the toolchain that
actually builds this project — `<winsock.h>` followed by `<winsock2.h>` in one translation unit is
the classic C2011 redefinition wall, because MSVC defines the shared socket types in both headers.

(mingw-w64 tolerates the combination, because it factors those types into separately guarded
`psdk_inc/*.h` headers. A clean cross-compile there proves nothing about MSVC — do not use it as
evidence.)

**The rule:** `#include "groovymister.h"` appears only in `src/burner/win32/groovy/groovy_output.cpp`
and `src/intf/input/win32/inp_mister.cpp`. Everything FBNeo-facing goes through
`groovy_output.h` / `groovy_input.h`, which are plain declarations plus POD types and pull in no
system socket headers at all. Keeping that boundary means no FBNeo file ever has to care about
include ordering, and it is also the right layering regardless.

## Notes for whoever touches the caller

- The class is **not thread-safe**. One thread owns `CmdInit` / `CmdSwitchres` / `CmdBlit` /
  `CmdAudio` / `WaitSync`. Our integration is single-threaded on the emulation thread.
- **`WaitSync()` is the only thing that drains the RIO send-completion queue on Windows.** A caller
  that stops calling it fills the 846-entry queue within a handful of frames, after which sends fail
  silently. This is why the FBNeo integration always calls it.
- `CmdSwitchres` is mandatory after **every** `CmdInit`.
- Never write pixels anywhere but `getPBufferBlit()` — those buffers are RIO-registered at
  `CmdInit`. A `memcpy` into them is fine; a pointer swap is not.

`lz4/LICENSE` is LZ4's own (BSD 2-Clause for the library). The Groovy client itself carries its
upstream licence in its file headers.
