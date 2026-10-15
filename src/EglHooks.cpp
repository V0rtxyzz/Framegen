#include "framegen/EglHooks.hpp"
#include "framegen/FrameGen.hpp"

#if defined(__ANDROID__)

#include "pl/memory/Hook.hpp"

#include <EGL/egl.h>
#include <dlfcn.h>

#include <atomic>

namespace framegen {
namespace {

using EglSwapIntervalFn = EGLBoolean (*)(EGLDisplay, EGLint);
using EglSwapBuffersFn  = EGLBoolean (*)(EGLDisplay, EGLSurface);

EglSwapIntervalFn          orig_eglSwapInterval = nullptr;
EglSwapBuffersFn           orig_eglSwapBuffers  = nullptr;
pl::memory::HookHandle     h_eglSwapInterval{};
pl::memory::HookHandle     h_eglSwapBuffers{};

EGLDisplay        gLastDisplay{EGL_NO_DISPLAY};
int               gAppliedSwapInterval{-1};
std::atomic<int>  gEngineSwapInterval{-1}; // last interval the game itself asked for

EGLBoolean hook_eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    gEngineSwapInterval.store(static_cast<int>(interval), std::memory_order_relaxed);
    if (FrameBlendGl::wantsVsync()) {
        // Every queued frame (generated + real) must own a vsync.
        interval = 1;
    }
    return orig_eglSwapInterval ? orig_eglSwapInterval(dpy, interval) : EGL_TRUE;
}

// Presents the current back buffer through the ORIGINAL eglSwapBuffers.
bool presentViaOriginalSwap(void* dpy, void* surface) {
    if (orig_eglSwapBuffers == nullptr) return false;
    return orig_eglSwapBuffers(static_cast<EGLDisplay>(dpy), static_cast<EGLSurface>(surface)) == EGL_TRUE;
}

EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    static bool sFirstCall = true;
    if (sFirstCall) {
        sFirstCall = false;
        FrameGenLog::write("eglSwapBuffers hook is running (frame blend %s)",
                           settings().enabled.load() ? "ON" : "OFF");
    }

    // 1. Swap interval: 1 (VSync) while Frame Gen is active, otherwise whatever the game asked for.
    if (orig_eglSwapInterval != nullptr && dpy != EGL_NO_DISPLAY) {
        const int wantInterval = FrameBlendGl::wantsVsync()
                                     ? 1
                                     : gEngineSwapInterval.load(std::memory_order_relaxed);
        if (wantInterval >= 0 && (dpy != gLastDisplay || wantInterval != gAppliedSwapInterval)) {
            gLastDisplay = dpy;
            gAppliedSwapInterval = wantInterval;
            orig_eglSwapInterval(dpy, wantInterval);
            FrameGenLog::write("swap interval set to %d (game asked for %d)", wantInterval,
                               gEngineSwapInterval.load(std::memory_order_relaxed));
        }
    }

    // 2. Frame Gen: may present a generated frame first and redraw the real one.
    FrameBlendPresentOps ops{};
    ops.present = &presentViaOriginalSwap;
    (void)FrameBlendGl::onBeforeSwap(dpy, surface, ops);

    // 3. Present the real frame.
    return orig_eglSwapBuffers ? orig_eglSwapBuffers(dpy, surface) : EGL_FALSE;
}

template <typename FnPtr>
void installSymHook(void* lib, const char* name, FnPtr detour, FnPtr& origOut,
                    pl::memory::HookHandle& handleOut) noexcept {
    if (handleOut.installed() || lib == nullptr) return;
    void* target = dlsym(lib, name);
    if (!target) return;
    pl::memory::FuncPtr orig = nullptr;
    handleOut = pl::memory::HookHandle(
        target, reinterpret_cast<pl::memory::FuncPtr>(detour), &orig, pl::memory::HookPriority::Normal);
    if (handleOut.installed() && orig != nullptr) {
        origOut = reinterpret_cast<FnPtr>(orig);
    }
}

} // namespace

void installEglHooks() noexcept {
    void* eglLib = dlopen("libEGL.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!eglLib) eglLib = dlopen("libEGL.so", RTLD_LAZY);
    if (!eglLib) return;
    installSymHook(eglLib, "eglSwapInterval", hook_eglSwapInterval, orig_eglSwapInterval, h_eglSwapInterval);
    installSymHook(eglLib, "eglSwapBuffers", hook_eglSwapBuffers, orig_eglSwapBuffers, h_eglSwapBuffers);
    FrameGenLog::write("hooks: eglSwapInterval installed=%d, eglSwapBuffers installed=%d",
                       h_eglSwapInterval.installed() ? 1 : 0, h_eglSwapBuffers.installed() ? 1 : 0);
}

void uninstallEglHooks() noexcept {
    h_eglSwapBuffers.reset();
    h_eglSwapInterval.reset();
    FrameBlendGl::resetHistory();
    gLastDisplay = EGL_NO_DISPLAY;
    gAppliedSwapInterval = -1;
}

} // namespace framegen

#else // Host build: nothing to hook.

namespace framegen {
void installEglHooks() noexcept {}
void uninstallEglHooks() noexcept {}
} // namespace framegen

#endif
