# IGI 2: Covert Strike - Direct3D 11 port

A Direct3D 11 renderer for IGI 2: Covert Strike 1.3. The game's Direct3D 8 device is implemented on Direct3D 11 inside
the game process. It is not a wrapper DLL dropped next to the game and not a translation layer like dgVoodoo or DXVK: the
game's Direct3D 8 calls are answered by a device written for this game, and the most expensive parts (terrain) are drawn
by our own GPU code instead of the game's CPU path.

It ships in the Neonix patch for IGI 2 (`plugins\igi2_unified.dll`). This repository holds only the renderer, all of it
written by Sachal; the rest of the Neonix patch is not part of it.

## What it does

- **Direct3D 8 device on Direct3D 11** (`dx11_device.c`, `dx11_d3d.c`, `dx11_res.c`): textures, vertex and index
  buffers, state blocks, render targets, Reset, gamma, borderless full screen.
- **Fixed-function pipeline in shaders** (`nx_ff_vs.hlsl`, `nx_ff_ps.hlsl`): the game's texture stages, fog and lighting.
- **Game shaders translated at run time**: the game's vs.1.0 vertex shaders (`dx11_vs.c`) and ps.1.x pixel shaders
  (`dx11_ps.c`) become Direct3D 11 shader code; results are cached on disk, so later starts compile nothing.
- **GPU terrain** (`dx11_terrain.c`, `nx_terrain.hlsl`): the height maps and material layers are uploaded once and drawn
  in chunks with level of detail on the GPU. The game's CPU terrain was more than half of its frame time.
- **Render thread** (`dx11_queue.c`): the game thread records, a second thread submits to Direct3D 11.
- **Scene cache** (`scene_cache.c`), **frame pacing** (`frame_pace.c`), **MSAA, anisotropic filtering, alpha to
  coverage** for foliage, **screenshots** (`screenshot.c`).
- **Fallback**: if Direct3D 11, the shader compiler or the device cannot start, the game's own Direct3D 8 is used.
- **Trace mode** (`d3d8_trace.c`): a pass-through Direct3D 8 proxy that counts every call and dumps shaders, used to map
  what the game does.

## How it hooks in

`renderer.c` patches one call in igi2.exe 1.3 (Direct3D_Open, 0x44A071). Before the game first uses its
`Direct3DCreate8` pointer (0x757C04), our `Nx_Direct3DCreate8` is put there. Nothing else in the game is replaced for
the device itself; the terrain path additionally hooks TerrainRender_Draw (0x609610). Addresses are for the 1.3 retail
executable.

## Settings

In `plugins\igi2_unified.ini`:

```ini
[Renderer]
Mode=dx11        ; dx11 = this renderer, dx8 = the game's own Direct3D 8
VSync=1          ; 1 = on, 0 = off, -1 = the game's own setting
Terrain=gpu      ; gpu = GPU terrain, cpu = the game's terrain
```

## Building

32-bit MinGW GCC with `-masm=intel`, as part of a DLL loaded into igi2.exe 1.3. `include/host.h` lists what the renderer
needs from that DLL: `Unified_Log` and the PostFX calls `Unified_PostFxPresentTest` / `Unified_PostFxDeviceLost`.
`dx11_terrain.c` also uses `Unified_TlSubstitute` (high-resolution terrain light) and `t_fog_exp2` (terrain fog curve).
Give them empty versions to build the renderer on its own.

The renderer's own shaders are precompiled into `dx11_shader_code.h`. After changing an `.hlsl` file:

```
python tools/build_renderer_shaders.py
```

(needs `fxc.exe` from the Windows 10 SDK, or set `FXC`).

## License

GPL-3.0. See `LICENSE`. If you ship a changed version, publish its source under the same license.
