// Fsr4Paths.cpp — see Fsr4Paths.hpp for the resolution contract.
#include "util/Fsr4Paths.hpp"

#include <unistd.h>

#include <cstdlib>
#include <limits.h>
#include <system_error>

namespace tforge::fsr4paths {
namespace {

std::filesystem::path parentOf(const std::filesystem::path &p) {
    return p.parent_path();
}

const char *nonEmptyEnv(const char *name) {
    const char *value = std::getenv(name);
    return (value && *value) ? value : nullptr;
}

} // namespace

std::filesystem::path executableDir() {
    char buffer[PATH_MAX];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0) {
        buffer[length] = '\0';
        std::error_code ec;
        const std::filesystem::path exe(buffer);
        if (std::filesystem::exists(exe, ec))
            return parentOf(exe);
    }
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path(".") : cwd;
}

std::vector<std::filesystem::path> nativePackRoots() {
    std::vector<std::filesystem::path> roots;
    // In-tree layout: the binary sits in build/ or build-fast/ next to
    // resources/, so <exe_dir>/../resources is the source tree's copy.
    roots.push_back(parentOf(executableDir()) / "resources" / "fsr4");
    roots.push_back(std::filesystem::path(".") / "resources" / "fsr4");
    return roots;
}

Resolution resolveNativePackDir(const std::string &packName) {
    Resolution result;
    for (const auto &root : nativePackRoots()) {
        const auto dir = root / "native_i8" / packName;
        result.searched.push_back(dir);
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec)) {
            result.path = dir;
            return result;
        }
    }
    return result;
}

Resolution resolveWeightBlob(const std::string &blobName) {
    Resolution result;
    if (const char *reRoot = nonEmptyEnv("TFORGE_FSR4_RE_ROOT")) {
        const auto candidate = std::filesystem::path(reRoot) / "extracted" /
                               "v410_initializers" / blobName;
        result.searched.push_back(candidate);
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            result.path = candidate;
            return result;
        }
    }
    std::filesystem::path dataHome;
    if (const char *xdg = nonEmptyEnv("XDG_DATA_HOME"))
        dataHome = std::filesystem::path(xdg);
    else if (const char *home = nonEmptyEnv("HOME"))
        dataHome = std::filesystem::path(home) / ".local" / "share";
    if (!dataHome.empty()) {
        const auto candidate = dataHome / "temporal-forge-player" / "fsr4" /
                               "extracted" / "v410_initializers" / blobName;
        result.searched.push_back(candidate);
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            result.path = candidate;
            return result;
        }
    }
    return result;
}

} // namespace tforge::fsr4paths
