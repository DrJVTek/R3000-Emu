# UE5 Camera Modes for PSX3D

## Goal

Provide two practical camera workflows in UE5 for live development and VR:

- `Free Roam`: lock the reconstructed mesh in world space so the UE5 camera can move freely around it.
- `VR Recenterable`: keep the mesh world-locked, but allow aligning it to the active player/HMD view with an explicit offset.

This is implemented with a split:

- `UR3000Gpu3DComponent` keeps the rendering API/properties
- `FR3000Gpu3DTrackingController` owns the tracking / recenter logic

Files:

- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Public/R3000Gpu3DComponent.h`
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000Gpu3DComponent.cpp`
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000Gpu3DTrackingController.h`
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000Gpu3DTrackingController.cpp`

## Tracking Modes

`EGpu3DTrackingMode` now has three modes:

- `LegacyFollowOwner`
  - Existing behavior.
  - The procedural mesh follows the component/owner transform.

- `WorldLocked`
  - Display name: `Free Roam / World Locked`
  - The procedural mesh is detached once and stays fixed in world space.
  - Best mode for live debugging with a free UE5 camera or pawn.

- `VrRecenterable`
  - Display name: `VR Recenterable`
  - The procedural mesh is also world-locked.
  - The mesh can be repositioned from the active player/HMD view.
  - Intended for VR alignment and quick re-centering during live sessions.

## VR Properties

New properties on `UR3000Gpu3DComponent`:

- `VrViewOffset`
  - Local offset applied from the active player/HMD view when recentering.
  - Use this to move the scene forward/back/up/down for comfort and scale alignment.

- `bAutoRecenterVrOnModeEnter`
  - When enabled, entering `VrRecenterable` automatically recenters once.

- `bTrackVrViewEveryTick`
  - Recenter from the active player/HMD view every tick.
  - Experimental.
  - This can be useful for tests, but it removes natural room-scale parallax.

## Blueprint Function

New callable function:

- `RecenterToPlayerView()`

Behavior:

- Gets the active player view via `APlayerController::GetPlayerViewPoint()`
- Applies `VrViewOffset` in view-local space
- Repositions the world-locked mesh to that target location

This is the main runtime hook for "force the VR camera position" without rewriting the whole pawn/camera stack.

Internally, this delegates to the dedicated tracking controller instead of keeping the logic embedded inside the mesh reconstruction component.

## Recommended Usage

### Live UE5 debug / presentation

- Set `TrackingMode = Free Roam / World Locked`
- Use a normal UE5 free camera or pawn
- Move around the reconstructed geometry manually

### VR testing

- Set `TrackingMode = VR Recenterable`
- Start with `bAutoRecenterVrOnModeEnter = true`
- Tune `VrViewOffset`
- Call `RecenterToPlayerView()` whenever the world needs to be re-aligned to the headset/player view

## Current Limits

- This does not replace the UE5 VR pawn/camera system.
- It repositions the reconstructed mesh relative to the active view.
- `bTrackVrViewEveryTick` is intentionally optional because continuous recentering is usually wrong for room-scale VR.

## Why this split

This keeps responsibilities clear:

- UE5 camera/pawn/HMD system handles player movement and view tracking
- `UR3000Gpu3DComponent` handles how PSX reconstructed geometry is anchored in the UE world

That separation is the safest way to support:

- normal editor/free-camera debugging
- VR alignment
- future per-game VR patching
