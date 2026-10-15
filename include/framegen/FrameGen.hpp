#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace framegen {

// ============================================================================
// Frame Gen (Frame Blend): experimental 1.5x / 2.0x frame generation
//
// Inspired by "Mine Frame Gen" (Fabric). It is NOT motion-vector frame
// generation: the generated frame is a weighted blend of the previous and the
// current rendered frame (mix(current, previous, strength)). It is presented
// right before the real frame, so it costs roughly one display-frame of extra
// latency and can show ghosting on fast motion.
//
// How it works on LeviLaunchroid (Android GL ES):
//   1. Before eglSwapBuffers the finished back buffer is copied to a texture.
//   2. On "generation" frames a full-screen shader blends previous + current
//      into the back buffer and presents it (extra eglSwapBuffers).
//   3. The real frame is redrawn from its texture and presented normally.
// While active it forces EGL swap interval 1 (VSync) so each queued frame is
// shown on its own vsync. It only helps on displays whose refresh rate is at
// least (real FPS x multiplier), e.g. 90/120 Hz screens.
// ============================================================================

namespace FrameBlendConsts {
// Real frames slower than this (< 20 FPS) are never blended: frames that far
// apart would only look like ghosting.
inline constexpr int64_t kMaxRealFrameNsForGeneration = 50'000'000LL;
inline constexpr int     kModeX15 = 0; // 1 generated frame every 2 rendered frames
inline constexpr int     kModeX20 = 1; // 1 generated frame per rendered frame
inline constexpr int     kMinStrengthPct = 10;
inline constexpr int     kMaxStrengthPct = 90;
} // namespace FrameBlendConsts


// Runtime settings (written by the Mod Menu / config file, read on the render thread).
struct Settings {
    std::atomic<bool> enabled{false};
    std::atomic<int>  mode{FrameBlendConsts::kModeX20};   // 0 = 1.5x, 1 = 2.0x
    std::atomic<int>  strengthPct{50};                    // 10..90 (weight of the previous frame)
    std::atomic<bool> debugTint{false};                   // tint generated frames red (visual proof)
    std::atomic<bool> debugLog{true};                     // write framegen.log + logcat statistics
};
Settings& settings() noexcept;

// Small file + logcat logger (thread-safe, size-capped, truncated on every game start).
// Files: <mod config dir>/framegen.log and, best effort, /storage/emulated/0/Download/FrameGen/framegen.log
// logcat tag: FrameGen
namespace FrameGenLog {
void init(const std::filesystem::path& configDir) noexcept;
void write(const char* fmt, ...) noexcept __attribute__((format(printf, 1, 2)));
std::string primaryPath();
} // namespace FrameGenLog

struct FrameBlendPlan {
    bool generate{false};      // present a blended frame before the real frame
    int  presentsThisFrame{1}; // 1 (real only) or 2 (generated + real)
};

// Pure scheduling logic (no GL, fully host-testable).
class FrameBlendPlanner {
public:
    // 0 -> 1.5, 1 (or anything else) -> 2.0
    static float multiplierForMode(int mode) noexcept;

    // Blend weight of the PREVIOUS frame in the generated frame (0.1 .. 0.9).
    static float weightForStrengthPct(int strengthPct) noexcept;

    // Decide what to do for the real frame that just finished rendering.
    //  - realFrameNs: time since the previous real frame was handed to the swap
    //  - havePrevious: a previous frame texture exists
    FrameBlendPlan planFrame(int mode, int64_t realFrameNs, bool havePrevious) noexcept;

    void reset() noexcept { mFrameCounter = 0; }

private:
    uint64_t mFrameCounter{0};
};

// Callbacks supplied by NativeHooks so the GL engine can present frames
// without knowing about the hook plumbing. EGL handles are passed as void*.
struct FrameBlendPresentOps {
    bool (*present)(void* eglDisplay, void* eglSurface){nullptr}; // original eglSwapBuffers
    void (*beforePresent)(){nullptr};                             // e.g. TBDR depth/stencil discard
};

class FrameBlendGl {
public:
    // Called on the render thread right before the real eglSwapBuffers, with the
    // finished frame in the default framebuffer.
    // Returns: 0 = module inactive (nothing done),
    //          1 = real frame captured only,
    //          2 = a generated frame was already presented and the real frame was
    //              redrawn; the caller must now present the real frame normally.
    static int onBeforeSwap(void* eglDisplay, void* eglSurface, const FrameBlendPresentOps& ops) noexcept;

    // True while the module is enabled and GL initialisation has not failed.
    // The EGL hooks use it to force swap interval 1.
    static bool wantsVsync() noexcept;

    // Generated frames presented since load.
    static uint64_t generatedFrames() noexcept;

    // Drop any cached "previous frame" (call when toggling the module).
    static void resetHistory() noexcept;
};

} // namespace framegen
