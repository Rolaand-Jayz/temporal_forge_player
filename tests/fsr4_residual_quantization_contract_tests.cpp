// fsr4_residual_quantization_contract_tests.cpp — guard the recovered FP8
// tensor boundary after residual and skip addition.
//
// Upstream: the recovered FNB_CT2D_ADD implementation performs CopySat after
// adding its residual/skip values. Downstream: residual_add.comp feeds the
// next convolution or decoder stage. This source contract prevents a future
// refactor from silently leaving the sum as an unquantized FP16 carrier.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

static std::string readSource(const char *relative) {
#ifdef TFORGE_SOURCE_ROOT
  const std::filesystem::path path =
      std::filesystem::path(TFORGE_SOURCE_ROOT) / relative;
#else
  const std::filesystem::path path = relative;
#endif
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

int main() {
  const std::string shader = readSource("shaders/fsr4/residual_add.comp");
  CHECK(!shader.empty());

  // The normal residual path must quantize the post-add value at the same
  // finite E4M3 boundary used by convolution outputs. The bypass path is a
  // copy of already-quantized partial channels and is intentionally separate.
  const std::string normalPath =
      "scratchData[dst] = float16_t(disableQuantization ? sum";
  CHECK(shader.find(normalPath) != std::string::npos);
  CHECK(shader.find(": quantizeFp8(sum)") != std::string::npos);
  CHECK(shader.find("disableQuantization ? sum") != std::string::npos);
  CHECK(shader.find("mode & 0x40000000u") != std::string::npos);

  // Quantization must remain disabled for the high-bit partial-channel copy;
  // that path must preserve the source channel rather than quantize twice.
  const auto bypass = shader.find("if ((mode & 0x80000000u) != 0u)");
  CHECK(bypass != std::string::npos);
  CHECK(bypass != std::string::npos &&
        shader.find("scratchData[uint(slot0.x) + pixel * cout + channel] =",
                    bypass) != std::string::npos);

  const std::string harness = readSource("src/render/Fsr4DispatchHarness.cpp");
  CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_DISABLE_RESIDUAL_QUANTIZATION") !=
        std::string::npos);

  if (g_failures == 0) {
    std::printf("fsr4_residual_quantization_contract_tests: OK\n");
    return 0;
  }
  std::fprintf(stderr, "fsr4_residual_quantization_contract_tests: %d FAILURES\n",
               g_failures);
  return 1;
}
