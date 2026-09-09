// Fsr4Paths.hpp
//
// Runtime resolution of FSR4 assets (native INT8 packs and generic RE weight
// blobs). All lookups are host-independent: no build-time source-root path is
// baked into the binary. Precedence:
//
//   Native pack root : (1) <exe_dir>/../resources/fsr4   (in-tree build dir)
//                     (2) <cwd>/resources/fsr4
//   Weight blob      : (1) $TFORGE_FSR4_RE_ROOT/extracted/v410_initializers/
//                     (2) $XDG_DATA_HOME/temporal-forge-player/fsr4/extracted/
//                         v410_initializers/  (or $HOME/.local/share/...)
//
// The struct returns every searched location so callers can log actionable
// absence diagnostics instead of a bare "not found".
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tforge::fsr4paths {

struct Resolution {
    std::filesystem::path path;
    std::vector<std::filesystem::path> searched;

    [[nodiscard]] bool found() const { return !path.empty(); }
};

// Directory containing the running executable. Falls back to the current
// working directory when /proc/self/exe is unavailable.
std::filesystem::path executableDir();

// Candidate roots (in precedence order) that contain native_i8/<pack>.
std::vector<std::filesystem::path> nativePackRoots();

// Resolve resources/fsr4/native_i8/<packName>.
Resolution resolveNativePackDir(const std::string &packName);

// Resolve the generic RE weight blob <blobName> (e.g. v410_quality.bin).
// Tier 1 is the documented TFORGE_FSR4_RE_ROOT override; tier 2 is the XDG
// data location.
Resolution resolveWeightBlob(const std::string &blobName);

} // namespace tforge::fsr4paths
