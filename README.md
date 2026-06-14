# Black Hole Renderer — WebAssembly

Real-time Schwarzschild black hole ray tracer written in C++, compiled to WebAssembly, embedded as a single self-contained HTML file. No server required — open by double-clicking.

## Demos

| File | Description |
|------|-------------|
| `index.html` | Black hole + object launcher |
| `oda.html` | Checkered room + 3 reflective spheres |

## Physics (index.html)

**Light bending** — null geodesic integrated step by step:
```
a = -(3/2)·h²·r̂/r⁴     h = conserved angular momentum, rs = 1 unit
```
Equivalent to the Schwarzschild orbit equation `d²u/dφ² + u = (3/2)·rs·u²`. The photon sphere (`r = 1.5·rs`) and critical impact parameter `b = (3√3/2)·rs` emerge naturally.

**Event horizon** (`r < rs`) — light cannot escape → absolute black.

**Accretion disk** — inner edge at ISCO (`r = 3·rs`):
- Shakura–Sunyaev brightness profile `~r⁻²·⁵`
- Doppler beaming δ³ (approaching side bright/bluish)
- Gravitational redshift `√(1 − rs/r)`
- Kepler orbital velocity `v = √(rs/2r)`

**Far rays** (`b > 9·rs`) — analytic first-order deflection `α = 2·rs/b` for speed. Close rays fully integrated (320 steps).

**Stars** — procedural, lensed into arcs by the bent ray directions.

**Launched objects** (click to spawn) — massive-particle geodesic ODE via velocity-Verlet:
```
a = -(1/(2r²) + (3/2)·h²/r⁴)·r̂
```
Objects spaghettify from tidal forces, Doppler-shift toward blue/red, fade and vanish at the horizon. Tidal drag ensures grazing orbits spiral inward rather than escaping.

## Controls

| Action | Effect |
|--------|--------|
| Drag | Look around |
| Click edge | Launch object → stable orbit |
| Click center | Launch object → plunge |
| Space | Toggle animation |
| 1 / 2 / 3 | Set render quality |
| Arrow keys | Look around |

Quality auto-adjusts to maintain ~55 FPS.

## Build

**Requirements:** [`zig`](https://ziglang.org) ≥ 0.12, `python3`

```sh
./build.sh
```

Compiles both C++ sources to `wasm32-freestanding` with `-O2`, then uses Python to base64-embed each `.wasm` into the corresponding HTML file. Output is fully self-contained.

```
render_wasm.cpp → render.wasm → index.html
oda_wasm.cpp    → oda.wasm    → oda.html
```

## Zero dependencies

No SDL, no stdlib, no libm, no Emscripten. Custom `memset`/`memcpy`, polynomial `sin`/`cos`, integer hash for stars. The entire engine fits in a single `.cpp` file per demo.
