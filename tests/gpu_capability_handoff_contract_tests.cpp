// gpu_capability_handoff_contract_tests.cpp — protects the live playback
// capability boundary. The standalone probe already sees the selected GPU,
// but PlaybackEngine must receive the same Vulkan instance and pass the
// resulting capability flags into Fsr4DispatchHarness; otherwise diagnostic
// FP16 fallback dispatch is silently disabled while INT8 remains active.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

static std::string readSource(const char *relative) {
#ifdef TFORGE_SOURCE_ROOT
    const auto path = std::filesystem::path(TFORGE_SOURCE_ROOT) / relative;
#else
    const auto path = std::filesystem::path(relative);
#endif
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

int main() {
    const auto header = readSource("src/core/PlaybackEngine.hpp");
    const auto playback = readSource("src/core/PlaybackEngine.cpp");
    const auto main = readSource("src/main.cpp");

    // The instance is required by GpuCapabilityProbe to resolve cooperative-
    // matrix properties for the selected physical device.
    CHECK(header.find("VkInstance instance") != std::string::npos);
    CHECK(playback.find("GpuCapabilityProbe::probe(physical, instance)") !=
          std::string::npos);
    CHECK(main.find("vk.instance()") != std::string::npos);
    // Do not regress the production profile: this plumbing discovers support;
    // it must not replace the RDNA3 INT8 default with an invented FP8 path.
    CHECK(playback.find("vkCap_.profile = Fsr4Profile::Int8Dot4") ==
          std::string::npos);
    CHECK(playback.find("vkCap_ = GpuCapabilityProbe::probe") !=
          std::string::npos);

    return g_failures == 0 ? 0 : 1;
}
