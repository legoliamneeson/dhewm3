# Native HDR / PsychoV35 for Windows

This fork renders Doom 3 into an RGBA16F OpenGL framebuffer, maps the scene
with PsychoV35 before screen-space GUI rendering, then presents linear scRGB
through an engine-owned D3D11/DXGI flip swapchain. No ReShade or RenoDX addon
is required. The GLSL tonemapper comes from the user's RenoDX Doom 3 mod.

## Installing the Windows x64 package

1. Enable **Use HDR** in Windows for the display on which the game will run.
2. Back up existing files with the same names before copying the package into
   the original Doom 3 installation folder (the folder containing `base/`).
3. Copy **all** package files, including the x64 `base.dll`, `d3xp.dll`,
   `SDL2.dll`, `OpenAL32.dll`, and `libcurl-4.dll`. Mixing these with the old
   x86 files will not work. Doom 3 BFG Edition data is not supported.
4. Run `Launch Native HDR.cmd`. Use `Launch RoE Native HDR.cmd` for
   Resurrection of Evil, if its game data is installed.
5. Open **F10 → Video** to set display peak, scene white, HUD/menu white,
   and PsychoV35 controls. The default peak is 1000 nits and the default
   scene/UI white is 203 nits. Set the peak for your display.

This executable explicitly loads Windows system OpenGL, bypassing a
game-folder `opengl32.dll`, including an old 32-bit ReShade loader. Existing
addon files can stay in the folder; this executable does not load them.
Other injectors that attach by external means have not been validated.

While HDR gameplay uses raw relative mouse input, the hidden Windows cursor
is parked in a one-pixel rectangle at the bottom-right corner. This moves the
Windows cursor's SDR-clamped box away from the crosshair, matching the RenoDX
addon. SDL owns the rectangle and releases it for menus/F10, focus loss and
shutdown; the rectangle follows window resizing. Cursor-warp fallback is left
alone so it cannot break mouse-look. This fix uses the bundled SDL2 (2.24+).


The launchers select `r_hdr 1`, shader gamma, and no MSAA. The engine still
needs the original game's patched 1.3.1 data. The executable and shaders are
self-contained; the source checkout is not needed while playing.

## Controls

| Console cvar | Default | Meaning |
| --- | --- | --- |
| `r_hdr` | 1 | Native HDR; use `vid_restart` after changing |
| `r_hdrPeakNits` | 1000 | Target peak in the chosen output gamut |
| `r_hdrGameNits` | 203 | Scene reference white |
| `r_hdrUINits` | 203 | Screen-space HUD/menu reference white |
| `r_hdrExposure` | 1 | Scene exposure multiplier |
| `r_hdrContrast` | 1 | PsychoV35 contrast |
| `r_hdrSaturation` | 1 | Scene saturation |
| `r_hdrCompression` | 0 | PsychoV35 compression; 0 selects automatic |
| `r_hdrConeResponse` | 1 | PsychoV35 cone response |
| `r_hdrPurity` | 1 | PsychoV35 purity scale |
| `r_hdrGamutCompression` | 1 | Target gamut projection strength |
| `r_hdrGamut` | 1 | Target gamut: 0 BT.709, 1 BT.2020 |
| `r_hdrDebug` | 0 | Read back and log the next rendered HDR scene/output frame |

All HDR look controls are archived except the diagnostic switch. General
`r_gamma` and `r_brightness` still affect the original rendering; keep both
at 1 for the neutral baseline used during validation.

The output is FP16 **scRGB**, with 1.0 representing 80 nits. Windows manages
conversion to the display signal. BT.709/scRGB channel values can exceed
the configured peak when representing saturated BT.2020 colors; the peak
constraint applies in the selected target gamut.

## Compatibility and fallback

- Requires Windows 10/11, OpenGL 3.3+, D3D11, and driver support for
  `WGL_NV_DX_interop2` on the rendering GPU. Validated on an RTX 4080 SUPER.
- MSAA is not implemented for this first HDR path. Set `r_multiSamples 0`.
  Unsupported configurations log a warning and use the existing SDR path.
- Shader gamma must be enabled (`r_gammaInShader 1`).
- Windows HDR must be enabled on the containing output when HDR initializes.
  Enable it and run `vid_restart` if the engine falls back to SDR.
- Use a window or borderless desktop fullscreen. Exclusive fullscreen,
  hybrid/multi-GPU configurations, and arbitrary third-party mods have not
  been comprehensively tested.
- In-world GUIs remain part of scene lighting. Screen-space HUD/menus use UI
  white; PDA model views after the GUI boundary are treated as UI.
- Existing screenshots are SDR-format captures and clip HDR headroom. They
  are useful for composition checks, not peak-luminance measurements.
- For SDR, enter `r_hdr 0` followed by `vid_restart`, then launch the executable
  directly instead of using an HDR launcher.

The log is normally `Documents/My Games/dhewm3/dhewm3log.txt`. Look for
`Native HDR ACTIVE` and the pre-UI PsychoV35 message. For a range check,
set `r_hdrDebug 1` during gameplay. Diagnostic readbacks are intentionally slow.

## Implementation and verification

- `neo/sys/win32/native_hdr.cpp`: FP16/depth-stencil resources, GLSL passes,
  WGL/D3D synchronization, output, resize, shutdown, cvars and diagnostics.
- `neo/renderer/hdr_tonemap.h`: embedded GLSL from the RenoDX Doom 3 mod,
  including PsychoV35 and the mod's surrounding highlight/color treatment.
- `draw_arb2.cpp`: remove only known scene-color upper clamps; mask clamps stay.
- `Image_load.cpp`: preserve FP16 through framebuffer color copies, with
  vanilla RGB-copy alpha semantics. Depth copies read the scene attachment.
- Backend/view hooks locate the main-scene/GUI boundary and present HDR.
- F10 video options use the existing engine cvar UI.

Build the `dhewm3`, `base`, and `d3xp` targets in x64 RelWithDebInfo. The
project's Windows dependency repository is still needed when building.
No shader compiler or external shader files are needed at runtime.

Manual checks after changing the renderer: main menu, F10, gameplay with
emissive lights and stencil shadows, screen-space HUD, PDA, heat haze,
damage/double-vision effects, level-loading fades, resize/`vid_restart`,
SDR fallback, and Resurrection of Evil. Compare the same scenes/settings
with the original RenoDX mod; retain the game's intentional fades.

PsychoV35/GLSL attribution and the RenoDX MIT license are supplied in the
package. The engine remains under its existing GPL license and additional
terms. See `COPYING.txt` and the project README.
