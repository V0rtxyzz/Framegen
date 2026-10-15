#pragma once

namespace framegen {

// Installs hooks on libEGL's eglSwapInterval / eglSwapBuffers (Android only; no-op on host).
void installEglHooks() noexcept;

// Removes the hooks. Safe to call when nothing is installed.
void uninstallEglHooks() noexcept;

} // namespace framegen
