#pragma once

// In-process redirect of other mods' configuration files to the co-op view prepared by
// Compat::PrepareOverlays (common/Compat/ConfigOverlay.h). Nothing outside the RFAB Online folder
// is written; only this process sees the co-op values.

#include <filesystem>
#include <string>

namespace stubs
{
/**
 * Reads the configuration the game would read under `acGamePath`/Data, prepares the co-op copies
 * and installs the CreateFile redirect. Call once, after the game directory is known and the
 * working directory points at it, and BEFORE the game executable (and with it any SKSE / .NET
 * plugin) is loaded. Never throws; failures are reported and EngineFixes is then kept out.
 */
void InitCoopConfigOverlay(const std::filesystem::path& acGamePath) noexcept;

/** Human-readable report of what the co-op view changes (empty before InitCoopConfigOverlay). */
const std::string& GetCoopConfigReport() noexcept;

/**
 * May EngineFixes.dll load? True only once InitCoopConfigOverlay found what EngineFixes will read
 * safe for Skyrim Together (see Compat::IsEngineFixesSafe). Read from the DLL load hook: no I/O.
 */
bool IsEngineFixesAllowed() noexcept;
} // namespace stubs
