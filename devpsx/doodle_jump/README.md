# Doodle Jump MCP Demo

Small native PSX 2D test game designed for the emulator MCP "vector vision"
path. The game intentionally draws only flat colored quads so an LLM can infer
state from primitive lists without seeing a raster image.

## Controls

- D-pad left/right: steer horizontally.
- Cross: restart after a fall.
- Start: soft restart at any time.

## Vector Contract

- Player: cyan quad, 12x16 pixels.
- Platforms: green quads, usually 42x8 pixels.
- Bottom danger line: red quad near the bottom of the visible camera.
- Background stars: dark blue quads, decorative and small.

The useful policy loop for an LLM is:

1. Read a scene vector snapshot with HUD/2D primitives enabled.
2. Locate the cyan player quad and green platform quads.
3. Prefer the nearest reachable green platform below/near the player.
4. Hold `left` or `right` for a few frames to align horizontal center.
5. Release or switch direction when centered enough.

This intentionally stresses continuous pad control, unlike a Flappy-style tap
game. If latency is high, use longer hold windows, for example 6-12 frames per
action.

## Build

```bat
make
```

The output is `doodle_jump.ps-exe`.
