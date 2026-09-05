// synthetic_jitter_contract_audit_tests.cpp — focused audit of the
// sampling/reporting contract for synthetic video jitter.
//
// This test is intentionally source-boundary based. The native passthrough
// branch publishes the converted decoded image without an FSR temporal
// dispatch, so it must not apply a synthetic color displacement that has no
// matching FSR jitter metadata. This test is written first and should fail
// until that boundary is made explicit.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static int g_failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
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
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

int main() {
    const std::string playback = readSource("src/core/PlaybackEngine.cpp");
    const size_t jitterSetter = playback.find("firstUploader->setInputJitter(");
    const size_t nativeBranch = playback.find("if (nativePassthrough)");

    // The audit must be meaningful even if a refactor removes one of the
    // boundaries: missing source means the contract is not verifiable.
    CHECK(jitterSetter != std::string::npos);
    CHECK(nativeBranch != std::string::npos);

    // Native passthrough has no FrameDispatchInput and therefore no FSR
    // jitterOffset equivalent. Any enabled sampler jitter must be gated off
    // before this call when nativePassthrough is selected.
    if (jitterSetter != std::string::npos) {
        const size_t callEnd = playback.find(");", jitterSetter);
        CHECK(callEnd != std::string::npos);
        if (callEnd != std::string::npos) {
            const std::string setter = playback.substr(
                jitterSetter, callEnd - jitterSetter + 2);
            CHECK(setter.find("!nativePassthrough") != std::string::npos);
        }
    }

    if (g_failures == 0) {
        std::printf("synthetic_jitter_contract_audit_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "synthetic_jitter_contract_audit_tests: %d FAILURES\n",
                 g_failures);
    return 1;
}
