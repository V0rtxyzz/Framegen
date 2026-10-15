#include "framegen/FrameGen.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <system_error>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#endif

namespace framegen {

// ============================================================================
// Planner (platform independent)
// ============================================================================
float FrameBlendPlanner::multiplierForMode(int mode) noexcept {
    return (mode == FrameBlendConsts::kModeX15) ? 1.5f : 2.0f;
}

float FrameBlendPlanner::weightForStrengthPct(int strengthPct) noexcept {
    return static_cast<float>(std::clamp(
               strengthPct, FrameBlendConsts::kMinStrengthPct, FrameBlendConsts::kMaxStrengthPct)) /
           100.0f;
}

FrameBlendPlan FrameBlendPlanner::planFrame(int mode, int64_t realFrameNs, bool havePrevious) noexcept {
    FrameBlendPlan plan{};
    const uint64_t index = mFrameCounter++;

    if (!havePrevious || realFrameNs <= 0 ||
        realFrameNs > FrameBlendConsts::kMaxRealFrameNsForGeneration) {
        return plan;
    }

    const bool everyFrame = (mode != FrameBlendConsts::kModeX15);
    if (everyFrame || (index & 1u) == 0u) {
        plan.generate = true;
        plan.presentsThisFrame = 2;
    }
    return plan;
}

Settings& settings() noexcept {
    static Settings sSettings;
    return sSettings;
}

namespace {
std::atomic<uint64_t> gGeneratedFrames{0};
std::atomic<bool>     gFailed{false};
std::atomic<bool>     gHistoryResetRequested{false};
} // namespace


// ============================================================================
// Logger
// ============================================================================
namespace FrameGenLog {
namespace {
std::mutex  gMutex;
std::string gPrimary;
std::string gSecondary;
int64_t     gStartNs = 0;
size_t      gBytes = 0;
constexpr size_t kMaxBytes = 512 * 1024;

int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void appendLine(const std::string& path, const char* line, size_t len) noexcept {
    if (path.empty()) return;
    if (FILE* f = std::fopen(path.c_str(), "a")) {
        std::fwrite(line, 1, len, f);
        std::fclose(f);
    }
}

void truncateFile(const std::string& path) noexcept {
    if (path.empty()) return;
    if (FILE* f = std::fopen(path.c_str(), "w")) std::fclose(f);
}
} // namespace

void init(const std::filesystem::path& configDir) noexcept {
    std::lock_guard<std::mutex> lock(gMutex);
    std::error_code ec;
    gPrimary.clear();
    gSecondary.clear();
    if (!configDir.empty()) {
        std::filesystem::create_directories(configDir, ec);
        gPrimary = (configDir / "framegen.log").string();
    }
#if defined(__ANDROID__)
    // Best effort copy somewhere reachable from a file manager (needs storage permission).
    const std::filesystem::path pub = "/storage/emulated/0/Download/FrameGen";
    std::filesystem::create_directories(pub, ec);
    gSecondary = (pub / "framegen.log").string();
#endif
    truncateFile(gPrimary);
    truncateFile(gSecondary);
    gBytes = 0;
    gStartNs = nowNs();
}

std::string primaryPath() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gPrimary;
}

void write(const char* fmt, ...) noexcept {
    if (!settings().debugLog.load(std::memory_order_relaxed)) return;

    char msg[480];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "FrameGen", "%s", msg);
#endif

    std::lock_guard<std::mutex> lock(gMutex);
    if (gBytes >= kMaxBytes) return;
    char line[560];
    const double secs = static_cast<double>(nowNs() - gStartNs) / 1e9;
    const int n = std::snprintf(line, sizeof(line), "[%9.3f] %s\n", secs, msg);
    if (n <= 0) return;
    const size_t len = std::min<size_t>(static_cast<size_t>(n), sizeof(line) - 1);
    appendLine(gPrimary, line, len);
    appendLine(gSecondary, line, len);
    gBytes += len;
}
} // namespace FrameGenLog

bool FrameBlendGl::wantsVsync() noexcept {
    return settings().enabled.load(std::memory_order_relaxed) &&
           !gFailed.load(std::memory_order_relaxed);
}

uint64_t FrameBlendGl::generatedFrames() noexcept {
    return gGeneratedFrames.load(std::memory_order_relaxed);
}

void FrameBlendGl::resetHistory() noexcept {
    gHistoryResetRequested.store(true, std::memory_order_relaxed);
}

#if defined(__ANDROID__)

// ============================================================================
// GL engine (render thread only)
// ============================================================================
namespace {

constexpr const char* kVertexShader =
    "#version 300 es\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    vUv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

constexpr const char* kFragmentShader =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uCur;\n"
    "uniform sampler2D uPrev;\n"
    "uniform float uWeight;\n"
    "uniform vec3 uTint;\n"
    "in vec2 vUv;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 c = texture(uCur, vUv).rgb;\n"
    "    if (uWeight > 0.0) {\n"
    "        c = mix(c, texture(uPrev, vUv).rgb, uWeight);\n"
    "    }\n"
    "    fragColor = vec4(c * uTint, 1.0);\n"
    "}\n";

constexpr int kVerifyFrames = 4;

int64_t monotonicNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Engine {
    EGLContext        ctx{EGL_NO_CONTEXT};
    GLuint            program{0};
    GLuint            vao{0};
    GLuint            tex[2]{0, 0};
    GLint             uCur{-1};
    GLint             uPrev{-1};
    GLint             uWeight{-1};
    GLint             uTint{-1};
    int               width{0};
    int               height{0};
    int               curIdx{0};
    bool              havePrev{false};
    int               verifyFramesLeft{kVerifyFrames};
    int64_t           lastRealSwapNs{0};
    FrameBlendPlanner planner{};

    // Diagnostics (logged every 2 s)
    bool              infoLogged{false};
    bool              firstGenLogged{false};
    int64_t           windowStartNs{0};
    uint64_t          statReal{0};
    uint64_t          statGenerated{0};
    uint64_t          statSlowSkips{0};
    int64_t           statHookNs{0};
};

constexpr int64_t kStatsIntervalNs = 2'000'000'000LL;

Engine gEngine;

struct SavedState {
    GLint     program{0};
    GLint     vao{0};
    GLint     activeTex{GL_TEXTURE0};
    GLint     tex[2]{0, 0};
    GLint     sampler[2]{0, 0};
    GLint     drawFbo{0};
    GLint     readFbo{0};
    GLint     readBuffer{GL_BACK};
    GLint     viewport[4]{0, 0, 0, 0};
    GLboolean colorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean scissor{GL_FALSE};
    GLboolean blend{GL_FALSE};
    GLboolean depth{GL_FALSE};
    GLboolean cull{GL_FALSE};
    GLboolean stencil{GL_FALSE};
};

void setCap(GLenum cap, GLboolean on) noexcept {
    if (on) {
        glEnable(cap);
    } else {
        glDisable(cap);
    }
}

void saveState(SavedState& s) noexcept {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTex);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.readFbo);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.blend   = glIsEnabled(GL_BLEND);
    s.depth   = glIsEnabled(GL_DEPTH_TEST);
    s.cull    = glIsEnabled(GL_CULL_FACE);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);

    for (int unit = 0; unit < 2; ++unit) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex[unit]);
        glGetIntegerv(GL_SAMPLER_BINDING, &s.sampler[unit]);
    }
    glActiveTexture(static_cast<GLenum>(s.activeTex));

    // Read buffer of the default framebuffer (restored explicitly below).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glGetIntegerv(GL_READ_BUFFER, &s.readBuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(s.readFbo));
}

void restoreState(const SavedState& s) noexcept {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(static_cast<GLenum>(s.readBuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(s.drawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(s.readFbo));

    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    setCap(GL_SCISSOR_TEST, s.scissor);
    setCap(GL_BLEND, s.blend);
    setCap(GL_DEPTH_TEST, s.depth);
    setCap(GL_CULL_FACE, s.cull);
    setCap(GL_STENCIL_TEST, s.stencil);

    for (int unit = 0; unit < 2; ++unit) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s.tex[unit]));
        glBindSampler(static_cast<GLuint>(unit), static_cast<GLuint>(s.sampler[unit]));
    }
    glActiveTexture(static_cast<GLenum>(s.activeTex));

    glBindVertexArray(static_cast<GLuint>(s.vao));
    glUseProgram(static_cast<GLuint>(s.program));
}

GLuint compileShader(GLenum type, const char* src) noexcept {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) return 0;
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool buildProgram(Engine& e) noexcept {
    const GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return false;
    }

    const GLuint prog = glCreateProgram();
    if (prog == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        glDeleteProgram(prog);
        return false;
    }

    e.program = prog;
    e.uCur    = glGetUniformLocation(prog, "uCur");
    e.uPrev   = glGetUniformLocation(prog, "uPrev");
    e.uWeight = glGetUniformLocation(prog, "uWeight");
    e.uTint   = glGetUniformLocation(prog, "uTint");
    glGenVertexArrays(1, &e.vao);
    return e.vao != 0 && e.uCur >= 0 && e.uPrev >= 0 && e.uWeight >= 0 && e.uTint >= 0;
}

void deleteTextures(Engine& e, bool contextAlive) noexcept {
    if (contextAlive && (e.tex[0] != 0 || e.tex[1] != 0)) {
        glDeleteTextures(2, e.tex);
    }
    e.tex[0] = 0;
    e.tex[1] = 0;
    e.width = 0;
    e.height = 0;
    e.havePrev = false;
    e.curIdx = 0;
}

void releaseAll(Engine& e, bool contextAlive) noexcept {
    deleteTextures(e, contextAlive);
    if (contextAlive) {
        if (e.program != 0) glDeleteProgram(e.program);
        if (e.vao != 0) glDeleteVertexArrays(1, &e.vao);
    }
    e.program = 0;
    e.vao = 0;
    e.uCur = e.uPrev = e.uWeight = e.uTint = -1;
}

bool createTextures(Engine& e, int w, int h) noexcept {
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(2, e.tex);
    if (e.tex[0] == 0 || e.tex[1] == 0) return false;

    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, e.tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    e.width = w;
    e.height = h;
    e.havePrev = false;
    e.curIdx = 0;
    return true;
}

bool ensureResources(Engine& e, int w, int h) noexcept {
    if (e.program == 0 && !buildProgram(e)) {
        return false;
    }
    if (e.tex[0] == 0 || e.width != w || e.height != h) {
        deleteTextures(e, true);
        if (!createTextures(e, w, h)) return false;
    }
    return true;
}

void drawFullscreen(const Engine& e, GLuint prevTex, GLuint curTex, float weight, bool tint) noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, e.width, e.height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Every pixel is overwritten: tell tile-based GPUs not to load the old color.
    const GLenum colorAttachment = GL_COLOR;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &colorAttachment);

    glUseProgram(e.program);
    glUniform1i(e.uCur, 0);
    glUniform1i(e.uPrev, 1);
    glUniform1f(e.uWeight, weight);
    if (tint) {
        glUniform3f(e.uTint, 1.0f, 0.35f, 0.35f); // debug: generated frames look red
    } else {
        glUniform3f(e.uTint, 1.0f, 1.0f, 1.0f);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, curTex);
    glActiveTexture(GL_TEXTURE1);
    glBindSampler(1, 0);
    glBindTexture(GL_TEXTURE_2D, prevTex);

    glBindVertexArray(e.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void failModule(Engine& e, const char* reason) noexcept {
    gFailed.store(true, std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_WARN, "FrameGen", "Frame Blend disabled: %s", reason);
    FrameGenLog::write("ERROR: Frame Blend disabled: %s", reason);
    releaseAll(e, true);
}

} // namespace

int FrameBlendGl::onBeforeSwap(
    void* eglDisplay, void* eglSurface, const FrameBlendPresentOps& ops) noexcept {
    auto& e = gEngine;
    auto& cfg = settings();
    const EGLContext currentCtx = eglGetCurrentContext();

    // --- Module disabled: free GL objects once, on the thread that owns them ---
    if (!cfg.enabled.load(std::memory_order_relaxed)) {
        if (e.program != 0 || e.tex[0] != 0 || e.vao != 0) {
            releaseAll(e, currentCtx != EGL_NO_CONTEXT && currentCtx == e.ctx);
            FrameGenLog::write("Frame Gen turned off: GL resources released");
        }
        e.infoLogged = false;
        e.firstGenLogged = false;
        e.windowStartNs = 0;
        gFailed.store(false, std::memory_order_relaxed);
        e.havePrev = false;
        e.lastRealSwapNs = 0;
        e.verifyFramesLeft = kVerifyFrames;
        e.planner.reset();
        return 0;
    }

    if (gFailed.load(std::memory_order_relaxed) || ops.present == nullptr ||
        currentCtx == EGL_NO_CONTEXT) {
        return 0;
    }

    // --- New GL context (app resume / surface recreation): old names are dead ---
    if (currentCtx != e.ctx) {
        releaseAll(e, false);
        e.ctx = currentCtx;
        e.lastRealSwapNs = 0;
        e.verifyFramesLeft = kVerifyFrames;
        e.infoLogged = false;
        e.firstGenLogged = false;
    }
    if (gHistoryResetRequested.exchange(false, std::memory_order_relaxed)) {
        e.havePrev = false;
        e.planner.reset();
    }

    EGLint w = 0;
    EGLint h = 0;
    if (!eglQuerySurface(static_cast<EGLDisplay>(eglDisplay), static_cast<EGLSurface>(eglSurface),
                         EGL_WIDTH, &w) ||
        !eglQuerySurface(static_cast<EGLDisplay>(eglDisplay), static_cast<EGLSurface>(eglSurface),
                         EGL_HEIGHT, &h) ||
        w <= 0 || h <= 0) {
        return 0;
    }

    const int64_t nowNs = monotonicNowNs();
    const int64_t realFrameNs = (e.lastRealSwapNs != 0) ? (nowNs - e.lastRealSwapNs) : 0;
    e.lastRealSwapNs = nowNs;

    SavedState saved;
    saveState(saved);

    if (e.verifyFramesLeft > 0) {
        while (glGetError() != GL_NO_ERROR) {
            // drop errors left behind by the game so verification stays accurate
        }
    }

    if (!ensureResources(e, w, h)) {
        failModule(e, "could not create GL resources (needs OpenGL ES 3.0)");
        restoreState(saved);
        return 0;
    }

    if (!e.infoLogged) {
        e.infoLogged = true;
        const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const auto* version  = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        FrameGenLog::write("GL ready: %dx%d renderer=%s version=%s", static_cast<int>(w), static_cast<int>(h),
                           renderer ? renderer : "?", version ? version : "?");
    }

    // 1. Capture the finished frame from the default framebuffer.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, e.tex[e.curIdx]);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

    const GLuint curTex  = e.tex[e.curIdx];
    const GLuint prevTex = e.tex[e.curIdx ^ 1];

    // 2. Generated frame: blend(previous, current) -> present, then redraw the real frame.
    const FrameBlendPlan plan = e.planner.planFrame(
        cfg.mode.load(std::memory_order_relaxed), realFrameNs, e.havePrev);

    int presents = 1;
    if (plan.generate) {
        const float weight = FrameBlendPlanner::weightForStrengthPct(
            cfg.strengthPct.load(std::memory_order_relaxed));

        drawFullscreen(e, prevTex, curTex, weight, cfg.debugTint.load(std::memory_order_relaxed));
        if (ops.beforePresent != nullptr) ops.beforePresent();
        const bool presented = ops.present(eglDisplay, eglSurface);

        // The new back buffer is undefined: redraw the real frame for the caller's swap.
        drawFullscreen(e, curTex, curTex, 0.0f, false);

        if (presented) {
            presents = 2;
            gGeneratedFrames.fetch_add(1, std::memory_order_relaxed);
            if (!e.firstGenLogged) {
                e.firstGenLogged = true;
                FrameGenLog::write("first generated frame presented (real frame interval %.2f ms, weight %.2f)",
                                   static_cast<double>(realFrameNs) / 1e6, static_cast<double>(weight));
            }
        } else {
            FrameGenLog::write("WARNING: generated frame present failed (eglSwapBuffers returned false)");
        }
    }

    e.curIdx ^= 1;
    e.havePrev = true;

    if (e.verifyFramesLeft > 0) {
        --e.verifyFramesLeft;
        if (glGetError() != GL_NO_ERROR) {
            failModule(e, "GL error while capturing/blending frames");
        }
    }

    restoreState(saved);

    // Statistics window (logged every 2 s)
    const int64_t endNs = monotonicNowNs();
    if (e.windowStartNs == 0) e.windowStartNs = nowNs;
    ++e.statReal;
    if (presents == 2) ++e.statGenerated;
    if (!plan.generate && realFrameNs > FrameBlendConsts::kMaxRealFrameNsForGeneration) ++e.statSlowSkips;
    e.statHookNs += endNs - nowNs;
    if (endNs - e.windowStartNs >= kStatsIntervalNs && e.statReal > 0) {
        const double secs = static_cast<double>(endNs - e.windowStartNs) / 1e9;
        FrameGenLog::write(
            "stats %.1fs: real=%llu (%.1f fps) generated=%llu shown=%.1f fps slowSkips=%llu "
            "hook=%.2f ms/frame mode=%s strength=%d%% tint=%d res=%dx%d",
            secs, static_cast<unsigned long long>(e.statReal), static_cast<double>(e.statReal) / secs,
            static_cast<unsigned long long>(e.statGenerated),
            static_cast<double>(e.statReal + e.statGenerated) / secs,
            static_cast<unsigned long long>(e.statSlowSkips),
            static_cast<double>(e.statHookNs) / static_cast<double>(e.statReal) / 1e6,
            cfg.mode.load(std::memory_order_relaxed) == FrameBlendConsts::kModeX15 ? "1.5x" : "2.0x",
            cfg.strengthPct.load(std::memory_order_relaxed), cfg.debugTint.load(std::memory_order_relaxed) ? 1 : 0,
            static_cast<int>(w), static_cast<int>(h));
        e.windowStartNs = endNs;
        e.statReal = e.statGenerated = e.statSlowSkips = 0;
        e.statHookNs = 0;
    }
    return presents;
}

#else // Host build (unit tests): no GL, module is always inert.

int FrameBlendGl::onBeforeSwap(void*, void*, const FrameBlendPresentOps&) noexcept {
    return 0;
}

#endif

} // namespace framegen
