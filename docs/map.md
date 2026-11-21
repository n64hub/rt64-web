# Where things are

A map of the parts of RT64 that decide where a draw lands and what a frame drawn between the game's own shows, for
whoever reads a frame that looks wrong. It names functions, not lines: a search that prints each hit's whole function
finds them. The fork's options (compile-time switches, off on a desktop build unless a game turns them on) are listed
in N64Recomp-browser's `docs/forks.md`, and each one's comment is beside its code.

## A game frame, and the frames drawn between

- `WorkloadQueue::threadLoop()` (`src/hle/rt64_workload_queue.cpp`) gathers the workloads of a game frame
  (`endedGameFrame()`, why it ended), pairs it with the one before (`GameFrame::match()`), and renders each frame
  drawn from it, the game's own and each one between, by `threadRenderFrame()` with its weight. Each lands in
  `interpolatedTargets`, which the present queue (`src/hle/rt64_present_queue.cpp`) shows in turn.
- `GameFrame::match()` (`src/hle/rt64_game_frame.cpp`) pairs each workload with one of the frame before
  (`alignDrawnWorkloads()`), each scene's view-projections (`matchScene()`), each call's world transforms
  (`matchTransform()`, by the call hashes of `buildCallHashMap()`) and its tiles, and each rect
  (`matchRectScrolls()`). What it found is `GameFrameMap` (`src/hle/rt64_game_frame.h`): per workload its pair
  (`prevWorkloadIndex`), whether the camera cut (`cameraCut`, set in `matchScene()` where too few of a scene's
  transforms agree on one step), each transform's pair and flags, and `rectMoves`, each rect's edges in the frame
  before less this one's.
- `GameFrame::matchPreviousFrame()` uploads what the frames between move: transforms on the GPU, rects by
  `rectMoves` in the framebuffer renderer.

## Where a draw lands

- `FramebufferRenderer::addFramebuffer()` (`src/render/rt64_framebuffer_renderer.cpp`) places every call of a
  framebuffer pair: its scissor (`convertFixedRect()` of the projection's `scissorRect`, with the widescreen origins
  and `aspectRatioScale`), its viewport (`convertViewportRect()`), the screen scale and offset its vertices are drawn
  with, and for a rect its rect moved by the frame's `rectMoves`.
- `ProjectionProcessor::process()` (`src/render/rt64_projection_processor.cpp`) widens a projection for widescreen
  (`G_EX_ASPECT_ADJUST`, or `G_EX_ASPECT_AUTO` with a viewport across the scissor's width).
- `src/shaders/RasterVS.hlsl` turns a vertex's screen position into the target's, and draws a rect's quad as it is.

## Tile copies and render-to-RAM

- A tile copy is a framebuffer drawn earlier that a call samples as a texture: `State::loadDrawState()`
  (`src/hle/rt64_state.cpp`) asks `FramebufferManager::checkRegionsTMEM()` (and `checkRowTileCopy()`,
  `checkLeadingTileCopy()`, `src/hle/rt64_framebuffer_manager.cpp`) whether the TMEM a tile loads came from one, and
  sets the call tile's `tileCopyUsed`; `createTileCopySetup()` makes the copy.
- Render-to-RAM (`renderToRAM` in `src/common/rt64_emulator_configuration.h`, and `renderToRAMLate`) writes a drawn
  framebuffer back to the game's RAM: `State::fullSync()`'s `renderAndSynchronize`, and
  `FramebufferManager::checkRAM()` for a framebuffer the game changed by the CPU.

## Logs and dumps

Set in the environment of the program that runs RT64:

- `RT64_FRAME_LOG`: each game frame, why it ended and what each workload drew, each present and wait, on stderr
  (`WorkloadQueue::logGameFrame()`; a build with `RT64_GAME_FRAME_WORKLOADS` on).
- `RT64_FRAME_DUMP=<dir>`: every frame presented, where the 3D's triangles lay across the view, and what drew each
  frame and how it was paired (`src/hle/rt64_frame_dump.h`, which says what each file holds, and
  `rt64_frame_dump.cpp`). `RT64_FRAME_DUMP_SECONDS=<from>-<to>` keeps those seconds after the first present,
  `RT64_FRAME_DUMP_RATE=<n>` how many frames a second.
