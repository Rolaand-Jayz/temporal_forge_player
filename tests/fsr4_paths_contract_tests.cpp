// fsr4_paths_contract_tests.cpp
//
// Hermetic contract test for host-independent FSR4 asset resolution
// (src/util/Fsr4Paths). Validates precedence using only environment
// overrides and temporary directories — no machine-specific paths.
#include "util/Fsr4Paths.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#define CHECK(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAILED: %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

namespace {

class TempEnv {
public:
    TempEnv() {
        root_ = std::filesystem::temp_directory_path() /
                ("tforge-fsr4paths-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }
    ~TempEnv() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    const std::filesystem::path &root() const { return root_; }

    // Environment mutations are intentionally not restored: each test uses
    // its own TempEnv and the process exits when main() returns.
    void setEnv(const char *var, const std::string &value) {
        ::setenv(var, value.c_str(), 1);
    }
    void unsetEnv(const char *var) { ::unsetenv(var); }

private:
    std::filesystem::path root_;
};

int testWeightBlobPrecedence() {
    TempEnv env;
    const auto reRoot = env.root() / "re-tree";
    const auto xdg = env.root() / "xdg-data";
    std::filesystem::create_directories(
        reRoot / "extracted" / "v410_initializers");
    std::filesystem::create_directories(
        xdg / "temporal-forge-player" / "fsr4" / "extracted" /
        "v410_initializers");
    { std::ofstream a(reRoot / "extracted" / "v410_initializers" / "v410_quality.bin"); }
    { std::ofstream b(xdg / "temporal-forge-player" / "fsr4" / "extracted" /
                            "v410_initializers" / "v410_quality.bin"); }

    env.setEnv("TFORGE_FSR4_RE_ROOT", reRoot.string());
    env.setEnv("XDG_DATA_HOME", xdg.string());
    auto r = tforge::fsr4paths::resolveWeightBlob("v410_quality.bin");
    CHECK(r.found());
    CHECK(r.path == reRoot / "extracted" / "v410_initializers" /
                         "v410_quality.bin"); // env tier wins over XDG

    ::unsetenv("TFORGE_FSR4_RE_ROOT");
    r = tforge::fsr4paths::resolveWeightBlob("v410_quality.bin");
    CHECK(r.found());
    CHECK(r.path == xdg / "temporal-forge-player" / "fsr4" / "extracted" /
                         "v410_initializers" / "v410_quality.bin");

    // Absence must report every searched location.
    r = tforge::fsr4paths::resolveWeightBlob("v410_nonexistent.bin");
    CHECK(!r.found());
    CHECK(r.searched.size() == 1);
    return 0;
}

int testWeightBlobHomeFallbackAndAbsence() {
    TempEnv env;
    const auto home = env.root() / "home";
    std::filesystem::create_directories(home);
    env.unsetEnv("TFORGE_FSR4_RE_ROOT");
    env.unsetEnv("XDG_DATA_HOME");
    env.setEnv("HOME", home.string());
    auto r = tforge::fsr4paths::resolveWeightBlob("v410_quality.bin");
    CHECK(!r.found());
    CHECK(r.searched.size() == 1);
    CHECK(r.searched[0] == home / ".local" / "share" / "temporal-forge-player" /
                                "fsr4" / "extracted" / "v410_initializers" /
                                "v410_quality.bin");

    std::filesystem::create_directories(r.searched[0].parent_path());
    { std::ofstream out(r.searched[0]); }
    auto found = tforge::fsr4paths::resolveWeightBlob("v410_quality.bin");
    CHECK(found.found());
    CHECK(found.path == r.searched[0]);
    return 0;
}

int testNativePackResolution() {
    TempEnv env;
    env.unsetEnv("TFORGE_FSR4_RE_ROOT");
    // Use a fabricated pack name so the exe-relative tier (which may find a
    // real in-tree resources/ copy when running from a build dir) cannot
    // satisfy the lookup; the CWD tier must.
    const std::string pack = "contract_test_pack";
    const auto cwd = env.root() / "cwd";
    std::filesystem::create_directories(cwd / "resources" / "fsr4" / "native_i8" / pack);
    std::filesystem::current_path(cwd);

    auto r = tforge::fsr4paths::resolveNativePackDir(pack);
    CHECK(r.found());
    CHECK(r.path.filename() == pack);
    CHECK(r.path.parent_path().filename() == "native_i8");

    auto missing = tforge::fsr4paths::resolveNativePackDir("no_such_pack");
    CHECK(!missing.found());
    CHECK(missing.searched.size() >= 2); // exe-relative + cwd tiers logged
    bool cwdTierListed = false;
    for (const auto &p : missing.searched)
        cwdTierListed = cwdTierListed ||
            p == std::filesystem::path(".") / "resources" / "fsr4" /
                     "native_i8" / "no_such_pack";
    CHECK(cwdTierListed);
    return 0;
}

} // namespace

int main() {
    if (testWeightBlobPrecedence())
        return 1;
    if (testWeightBlobHomeFallbackAndAbsence())
        return 1;
    if (testNativePackResolution())
        return 1;
    std::printf("fsr4_paths_contract_tests: all checks passed\n");
    return 0;
}
