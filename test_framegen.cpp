// Host unit tests for the Frame Gen planner and settings (no GL needed).
#include "framegen/FrameGen.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>
#include <iostream>

int main() {
    using namespace framegen;
    using namespace framegen::FrameBlendConsts;

    // Off by default; no VSync forcing until enabled.
    assert(!settings().enabled.load());
    assert(!FrameBlendGl::wantsVsync());
    settings().enabled.store(true);
    assert(FrameBlendGl::wantsVsync());
    settings().enabled.store(false);

    // Helpers
    assert(std::fabs(FrameBlendPlanner::multiplierForMode(kModeX15) - 1.5f) < 1e-6f);
    assert(std::fabs(FrameBlendPlanner::multiplierForMode(kModeX20) - 2.0f) < 1e-6f);
    assert(std::fabs(FrameBlendPlanner::weightForStrengthPct(50) - 0.5f) < 1e-6f);
    assert(std::fabs(FrameBlendPlanner::weightForStrengthPct(0) - 0.1f) < 1e-6f);
    assert(std::fabs(FrameBlendPlanner::weightForStrengthPct(500) - 0.9f) < 1e-6f);

    // 2.0x: every real frame generates one (the first has no history)
    FrameBlendPlanner planner;
    assert(!planner.planFrame(kModeX20, 22'000'000LL, false).generate);
    for (int i = 0; i < 10; ++i) {
        const auto plan = planner.planFrame(kModeX20, 22'000'000LL, true);
        assert(plan.generate && plan.presentsThisFrame == 2);
    }

    // 1.5x: 3 presents every 2 real frames
    planner.reset();
    int generated = 0;
    for (int i = 0; i < 100; ++i) {
        generated += planner.planFrame(kModeX15, 16'000'000LL, true).generate ? 1 : 0;
    }
    assert(generated == 50);

    // Too-slow real frames (< 20 FPS) and invalid intervals are never blended
    assert(!planner.planFrame(kModeX20, 80'000'000LL, true).generate);
    assert(!planner.planFrame(kModeX20, 0, true).generate);

    // Logger: writes timestamped lines, truncates on init, honours the debugLog switch, caps size
    {
        const auto dir = std::filesystem::temp_directory_path() / "framegen_test_log";
        std::filesystem::remove_all(dir);
        FrameGenLog::init(dir);
        FrameGenLog::write("hello %d", 42);
        settings().debugLog.store(false);
        FrameGenLog::write("hidden");
        settings().debugLog.store(true);
        std::ifstream in(FrameGenLog::primaryPath());
        std::stringstream ss; ss << in.rdbuf();
        const std::string text = ss.str();
        assert(text.find("hello 42") != std::string::npos);
        assert(text.find("hidden") == std::string::npos);
        assert(!text.empty() && text[0] == '[');
        FrameGenLog::init(dir);                       // new session truncates the file
        std::ifstream in2(FrameGenLog::primaryPath());
        std::stringstream ss2; ss2 << in2.rdbuf();
        assert(ss2.str().empty());
        for (int i = 0; i < 20000; ++i) FrameGenLog::write("filler line %d to exercise the size cap ........", i);
        assert(std::filesystem::file_size(FrameGenLog::primaryPath()) <= 520 * 1024);
        std::filesystem::remove_all(dir);
    }

    // Host build: the GL engine is inert
    assert(FrameBlendGl::onBeforeSwap(nullptr, nullptr, FrameBlendPresentOps{}) == 0);

    std::cout << "framegen tests OK\n";
    return 0;
}
