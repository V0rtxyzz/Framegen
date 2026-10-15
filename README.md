# Frame Gen (FrameGen.levipack)

Standalone native mod for **Minecraft Bedrock on LeviLaunchroid** (arm64-v8a) with a single feature:
experimental **1.5x / 2.0x frame blending** (frame generation), inspired by *Mine Frame Gen*.

## What it does
1. Copies each finished frame into a texture right before `eglSwapBuffers`.
2. On generation frames, a full-screen shader presents `mix(current, previous, strength)` first,
   then the real frame (2.0x = one generated frame per real frame, 1.5x = one every two).
3. Forces EGL swap interval 1 (VSync) while enabled, so each queued frame gets its own vsync.

## Mod Menu (one module: "Frame Gen")
| Option | Meaning |
|---|---|
| Toggle | Off by default |
| Generation Multiplier | 1.5x or 2.0x |
| Blend Strength | 10-90 % weight of the previous frame |
| Debug: tint generated frames red | Visual proof: generated frames look red, so you see a flicker/red tint while moving |
| Write debug log | Writes `framegen.log` and logcat lines (tag `FrameGen`) |

Settings persist in `config/config.json` inside the mod folder.

## How to check that it is really blending
1. Turn on **Debug: tint generated frames red**. If generation works you see red-tinted frames interleaved with normal ones.
2. Read the log (`framegen.log`, or `logcat -s FrameGen`). Lines to look for:
   - `hooks: eglSwapInterval installed=1, eglSwapBuffers installed=1` (hooks are active)
   - `GL ready: WxH renderer=... version=...` (GLES 3 resources created)
   - `first generated frame presented ...` (the extra eglSwapBuffers worked)
   - `stats 2.0s: real=.. (.. fps) generated=.. shown=.. fps ... hook=.. ms/frame` every 2 s.
     With 2.0x, `generated` ~= `real` and `shown` ~= 2 x `real`. If `shown` stays near the display refresh
     rate while `real` is half of it, the frames are really reaching the screen.
   - `ERROR: ...` / `WARNING: ...` (module disabled itself or a present failed)
3. The log path is printed in the first line (`config dir: ...`). A copy is also written, when storage
   permission allows it, to `/storage/emulated/0/Download/FrameGen/framegen.log`.

## Limits (be aware)
- It is frame **blending**, not motion-vector generation: about 1 display frame of extra latency and some ghosting on fast turns.
- Needs a **90/120 Hz display**. On 60 Hz it only lowers the real FPS.
- Real frames slower than 20 FPS are never blended.
- Needs OpenGL ES 3.0. If setup or a GL call fails it disables itself and logs `Frame Blend disabled: ...` (logcat tag `FrameGen`); toggle off/on to retry.
- Costs one full-screen copy per frame plus 1-2 full-screen passes on generated frames.

## Build
Push to GitHub: the workflow builds `FrameGen.levipack` (artifact `FrameGen-levipack`).
The SDK ref defaults to preloader-android `main`; override with `-DFRAMEGEN_PRELOADER_REF=<tag|branch|commit>`.
