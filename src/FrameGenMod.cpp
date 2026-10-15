#include "framegen/EglHooks.hpp"
#include "framegen/FrameGen.hpp"

#include "pl/Mod.hpp"
#include "pl/ModMenu.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace framegen {
namespace {

constexpr const char* kModuleId = "framegen.frame_blend";

std::filesystem::path gConfigDir;
bool                  gRegistered = false;

int parseInt(std::string_view v, int fallback, int lo, int hi) noexcept {
    int out = fallback;
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc{}) return fallback;
    return std::clamp(out, lo, hi);
}

bool parseBool(std::string_view v) noexcept {
    return v == "true" || v == "1" || v == "on";
}

void loadConfig() noexcept {
    try {
        std::ifstream in(gConfigDir / "config.json");
        if (!in) return;
        const auto j = nlohmann::json::parse(in, nullptr, true, true);
        auto& s = settings();
        if (j.contains("enabled") && j["enabled"].is_boolean()) {
            s.enabled.store(j["enabled"].get<bool>());
        }
        if (j.contains("mode") && j["mode"].is_number_integer()) {
            s.mode.store(std::clamp(j["mode"].get<int>(), 0, 1));
        }
        if (j.contains("debug_tint") && j["debug_tint"].is_boolean()) {
            s.debugTint.store(j["debug_tint"].get<bool>());
        }
        if (j.contains("debug_log") && j["debug_log"].is_boolean()) {
            s.debugLog.store(j["debug_log"].get<bool>());
        }
        if (j.contains("strength_pct") && j["strength_pct"].is_number_integer()) {
            s.strengthPct.store(std::clamp(j["strength_pct"].get<int>(),
                                           FrameBlendConsts::kMinStrengthPct,
                                           FrameBlendConsts::kMaxStrengthPct));
        }
    } catch (...) {
        // Corrupt config: keep defaults.
    }
}

void saveConfig() noexcept {
    try {
        if (gConfigDir.empty()) return;
        std::filesystem::create_directories(gConfigDir);
        const auto& s = settings();
        nlohmann::json j = {
            {"enabled", s.enabled.load()},
            {"mode", s.mode.load()},
            {"strength_pct", s.strengthPct.load()},
            {"debug_tint", s.debugTint.load()},
            {"debug_log", s.debugLog.load()},
        };
        std::ofstream out(gConfigDir / "config.json", std::ios::trunc);
        out << j.dump(2) << "\n";
    } catch (...) {
    }
}

void onToggle(std::string_view moduleId, bool enabled) noexcept {
    if (moduleId != kModuleId) return;
    settings().enabled.store(enabled, std::memory_order_relaxed);
    FrameBlendGl::resetHistory();
    saveConfig();
}

void onConfigChanged(std::string_view moduleId, std::string_view key, std::string_view value) noexcept {
    if (moduleId != kModuleId) return;
    auto& s = settings();
    if (key == "mode") {
        s.mode.store(parseInt(value, FrameBlendConsts::kModeX20, 0, 1), std::memory_order_relaxed);
        FrameBlendGl::resetHistory();
    } else if (key == "strength_pct") {
        s.strengthPct.store(parseInt(value, 50, FrameBlendConsts::kMinStrengthPct,
                                     FrameBlendConsts::kMaxStrengthPct),
                            std::memory_order_relaxed);
    } else if (key == "debug_tint") {
        s.debugTint.store(parseBool(value), std::memory_order_relaxed);
        FrameGenLog::write("debug tint %s", parseBool(value) ? "ON (generated frames look red)" : "OFF");
    } else if (key == "debug_log") {
        const bool on = parseBool(value);
        if (on) s.debugLog.store(true, std::memory_order_relaxed);
        FrameGenLog::write("debug log %s", on ? "ON" : "OFF");
        if (!on) s.debugLog.store(false, std::memory_order_relaxed);
    }
    saveConfig();
}

bool registerModule(const std::string& ownerModId) noexcept {
    if (gRegistered) return true;
    using pl::modmenu::ConfigType;
    using pl::modmenu::ModuleBuilder;
    const auto& s = settings();

    const bool ok =
        ModuleBuilder(kModuleId, "Frame Gen (1.5x / 2.0x Frame Blend)")
            .modId(ownerModId)
            .description("Presents blended frames between rendered frames (like Mine Frame Gen). "
                         "Forces VSync on while active: use it on 90/120 Hz displays only. "
                         "Adds ~1 display frame of latency and can ghost on fast motion.")
            .defaultEnabled(s.enabled.load())
            .onToggle(&onToggle)
            .onConfigChanged(&onConfigChanged)
            .config("mode", "Generation Multiplier", ConfigType::Radio,
                    std::to_string(s.mode.load()),
                    "1.5x (1 generated frame every 2 rendered),2.0x (1 generated frame per rendered)")
            .config("strength_pct", "Blend Strength (% of previous frame)", ConfigType::SliderInt,
                    std::to_string(s.strengthPct.load()),
                    std::to_string(FrameBlendConsts::kMinStrengthPct),
                    std::to_string(FrameBlendConsts::kMaxStrengthPct))
            .config("debug_tint", "Debug: tint generated frames red", ConfigType::Toggle,
                    s.debugTint.load() ? "true" : "false")
            .config("debug_log", "Write debug log (framegen.log)", ConfigType::Toggle,
                    s.debugLog.load() ? "true" : "false")
            .registerModule();
    gRegistered = ok;
    return ok;
}

void unregisterModule() noexcept {
    if (!gRegistered) return;
    pl::modmenu::unregisterModule(kModuleId);
    gRegistered = false;
}

} // namespace

class FrameGenMod {
public:
    FrameGenMod() : mSelf(*pl::mod::NativeMod::current()) {}

    static FrameGenMod& getInstance() {
        static FrameGenMod sInstance;
        return sInstance;
    }

    [[nodiscard]] const pl::mod::NativeMod& getSelf() const noexcept { return mSelf; }

    bool load() {
        gConfigDir = getSelf().getConfigDir();
        loadConfig();
        FrameGenLog::init(gConfigDir);
        FrameGenLog::write("FrameGen 1.1.0 loaded. config dir: %s", gConfigDir.string().c_str());
        FrameGenLog::write("settings: enabled=%d mode=%s strength=%d%% tint=%d",
                           settings().enabled.load() ? 1 : 0,
                           settings().mode.load() == 0 ? "1.5x" : "2.0x", settings().strengthPct.load(),
                           settings().debugTint.load() ? 1 : 0);
        getSelf().getLogger().info("[FrameGen] Loaded (enabled={}, mode={}, strength={}%)",
                                   settings().enabled.load(), settings().mode.load(),
                                   settings().strengthPct.load());
        return true;
    }

    bool enable() {
        registerModule(getSelf().getId());
        installEglHooks();
        getSelf().getLogger().info("[FrameGen] EGL hooks installed.");
        return true;
    }

    bool disable() {
        saveConfig();
        uninstallEglHooks();
        unregisterModule();
        getSelf().getLogger().info("[FrameGen] Hooks removed, configuration saved.");
        return true;
    }

    bool unload() {
        disable();
        return true;
    }

private:
    pl::mod::NativeMod& mSelf;
};

} // namespace framegen

PL_REGISTER_MOD(framegen::FrameGenMod, framegen::FrameGenMod::getInstance())
