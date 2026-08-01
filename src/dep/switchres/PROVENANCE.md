# switchres — vendored modeline calculator (calc-only)

Generates CRT-correct modelines from a `(width, height, refresh)` triple. We use it purely as a
**calculator**: the resulting modeline is sent to the MiSTer over UDP (`CmdSwitchres`). We never
switch a host display.

## Provenance

| | |
|---|---|
| Upstream project | https://github.com/antonioginer/switchres |
| Taken via        | the PCSX2 Groovy fork's vendored copy (`PCSX2/pcsx2/3rdparty/switchres/`), which had itself taken it via the RPCS3 fork, branch `groovy-nlc-v2`, commit `69ef26ac2237e671769ba866449b761967630230` |
| Imported on      | 2026-07-24 |
| Local delta      | none — straight file copy |

Taking it second-hand rather than from upstream is deliberate: the `SR_CALC_ONLY` patch (below) is
**not** an upstream option, and the PCSX2/RPCS3 copy already carries it in the form both forks
settled on. A future re-sync should diff against that copy first, and only then against upstream.

Build files (`CMakeLists.txt`, `switchres.vcxproj`) were **not** imported; this tree is built
directly from `projectfiles/visualstudio-2015/fbneo_vs2015.vcxproj`.

## Licensing

switchres is **GPL-2.0+** (see the per-file header blocks; upstream ships no separate licence file,
so the canonical text is included here as `LICENSE.GPL-2.0.txt`). FBNeo's own licence
(`src/license.txt`) is source-available but non-commercial. Combining the two in one binary is a
question for the project owner, who has signed off on vendoring; this note exists so the situation
is on the record rather than discovered later. FBNeo's licence already requires publishing source
changes, which is aligned with GPL's obligation.

## Build — calc-only is not optional

Two defines, both required:

- **`SR_CALC_ONLY`** — drops every real host-display backend (`display_linux` / `display_windows` /
  `display_sdl2` and their X11 / DRM / ADL / ATI `custom_video` dependencies) and keeps only the
  `dummy` backend. Without this, switchres pulls in display-switching machinery we must never link,
  let alone execute — it would try to reprogram the *host's* monitor.
- **`SR_WIN32_STATIC`** — makes the wrapper symbols non-dllexport for static linking.

Consequently we call `sr_init_disp("dummy", nullptr)`. Any change that makes a real backend
reachable is a bug.

The reduced source set is exactly what is in this directory: `switchres.cpp`, `monitor.cpp`,
`modeline.cpp`, `display.cpp`, `custom_video.cpp`, `log.cpp`, `edid.cpp`, `switchres_wrapper.cpp`.
The backend implementations (`display_windows.cpp`, `custom_video_adl.cpp`, `custom_video_ati*.cpp`,
`custom_video_drmkms.cpp`, `custom_video_pstrip.cpp`, `custom_video_xrandr.cpp`,
`resync_windows.cpp`, `drm_hook.cpp`, `grid.cpp`) are deliberately absent.

## Usage contract

`sr_init()` → log callbacks → `sr_set_monitor(preset)` → `sr_init_disp("dummy", nullptr)` →
optional `sr_load_ini(path)` → `sr_add_mode(w, h, hz, flags, &sr_mode)` per video-mode change →
`sr_deinit()`.

Order matters: `sr_init()` parses any stray `switchres.ini` in the working directory, so setting the
monitor **after** it means our choice wins. An unknown preset name falls back to `generic_15`
silently, so the caller allowlists and lowercases the name first — see
`src/burner/win32/groovy/groovy_switchres.cpp`.

**Return-value traps** (both verified against the source in this directory, not assumed):

- `sr_init_disp()` returns the **display index**, i.e. `0` for the first display, and `-1` on
  failure (`switchres_wrapper.cpp:85-104`). Success is therefore `>= 0`. Writing
  `if (sr_init_disp(...))` or comparing against `1` treats a perfectly good init as an error.
- `sr_add_mode()` returns `0` on failure; on success the caller should still check `sr_mode.width`
  before trusting the result.

The returned modeline is validated by the three-tier gate in
`src/burner/win32/groovy/groovy_modeline.h` before it is ever sent to the FPGA.
