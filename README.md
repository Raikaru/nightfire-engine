# nightfire-engine

Reimplementation of the console *007: Nightfire* engine (PS2 USA, SLUS-20579) that loads the
original game data. Original code is used as a specification via the symbolised `ACTION.ELF`
(see `../nightfire-ps2` for the IDA/Ghidra databases and pseudocode dumps); behaviour is checked
against the game running in PCSX2 (`docs/oracle.md`).

## Game data

Extract from your own disc into one directory:

```
7z e "007 - Nightfire (USA).iso" ACTION.ELF FILES.BIN -o<gamedir>
```

## Build

```
cmake -S . -B build -G Ninja && cmake --build build
```

SDL3 is fetched and built statically. Needs a C++20 compiler and OpenGL 3.3.

## Tools

- `build/nfdump <gamedir> files|maps|validate`: list FILES.BIN, list level maps, or parse and decode
  every map chunk file, texture, PS2 mesh and collision mesh on the disc (exit status 0 = everything decoded).
- `build/nfview <gamedir> [level.bin] [--coll]`: fly-through level viewer. Click to capture the mouse,
  WASD / Space / C to move, Shift for speed, K for the collision wireframe, Esc to release/quit.
  `--shot out.bmp [--eye x,y,z] [--look yaw,pitch]` renders one frame headless.
- `tools/oracle/`: PCSX2 PINE client, virtual pad, per-frame player tracer.

Formats: `docs/formats.md`.

## Status

- Loads every level `.bin`: textures (4/8bpp CLUT), PS2 VIF meshes, collision meshes (BVH + quantised
  triangles), static instances with cross-file model resolution. `nfdump validate` decodes all 2,230
  map chunk files with no failures.
- Not yet: sky rendering, alpha blending modes / GS state from `DIRECT` blocks, animated texture
  frames, skinned characters, collision queries, gameplay.
