// jitter_gpu_contract_tests.cpp — disabled live-GPU contract probe.
//
// This test exercises the real decoded-YUV -> model-color upload path, rather
// than a source-text check or the FSR neural dispatch proof. It uploads one
// deterministic 32x16 YUV420 frame twice: once without synthetic jitter and
// once with a fixed +0.5 source-pixel X offset. The output is RGB10/A2 model
// color, so the test measures the bright stripe's luminance centroid and
// verifies that sampling at p + jitter moves that stripe toward smaller X.
//
// The test is intentionally disabled from normal CTest. Run it manually on a
// Vulkan device with the project's generated shader assets available. Return
// 77 when the required GPU/device capabilities or shader-backed uploader are
// unavailable, matching the repository's other live GPU diagnostics.

#include "render/GpuImageUploader.hpp"
#include "render/VulkanContext.hpp"

#include <vulkan/vulkan.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace temporal_forge;

namespace {

constexpr uint32_t kWidth = 32;
constexpr uint32_t kHeight = 16;
constexpr float kFixedJitterX = 0.5f;

struct Measurement {
    double centroidX = 0.0;
    double totalLuma = 0.0;
    uint32_t nonzeroPixels = 0;
};

DecodedVideoFrame makeDeterministicFrame() {
    DecodedVideoFrame frame{};
    frame.width = static_cast<int>(kWidth);
    frame.height = static_cast<int>(kHeight);
    frame.avFormat = AV_PIX_FMT_YUV420P;
    frame.colorRange = AVCOL_RANGE_JPEG;
    frame.colorSpace = AVCOL_SPC_BT709;
    frame.colorTransfer = AVCOL_TRC_BT709;
    frame.colorPrimaries = AVCOL_PRI_BT709;
    frame.chromaLocation = AVCHROMA_LOC_CENTER;
    frame.bitDepth = 8;
    frame.planes = 3;

    const uint32_t chromaWidth = kWidth / 2u;
    const uint32_t chromaHeight = kHeight / 2u;
    frame.plane[0].resize(static_cast<size_t>(kWidth) * kHeight, 0u);
    frame.plane[1].assign(static_cast<size_t>(chromaWidth) * chromaHeight,
                          128u);
    frame.plane[2].assign(static_cast<size_t>(chromaWidth) * chromaHeight,
                          128u);

    // A wide, centered white stripe makes a half-pixel displacement robust
    // against RGB10 quantization while keeping both borders far away from the
    // clamp-to-edge behavior under test.
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 12; x < 20; ++x)
            frame.plane[0][static_cast<size_t>(y) * kWidth + x] = 255u;
    }

    frame.linesize[0] = static_cast<int>(kWidth);
    frame.linesize[1] = static_cast<int>(chromaWidth);
    frame.linesize[2] = static_cast<int>(chromaWidth);
    return frame;
}

bool requiredFormatSupport(VkPhysicalDevice physical) {
    constexpr VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (VkFormat format : {VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                            VK_FORMAT_R8_UNORM}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
        if ((properties.optimalTilingFeatures & required) != required) {
            std::fprintf(stderr,
                         "SKIP: required optimal image format support missing "
                         "for VkFormat %d (features=0x%x)\n",
                         static_cast<int>(format),
                         properties.optimalTilingFeatures);
            return false;
        }
    }
    return true;
}

Measurement measureModelColor(const std::vector<uint8_t> &bytes,
                              uint32_t width, uint32_t height) {
    const size_t expected = static_cast<size_t>(width) * height * sizeof(uint32_t);
    if (bytes.size() != expected)
        return {};

    double weightedX = 0.0;
    Measurement result{};
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t offset =
                (static_cast<size_t>(y) * width + x) * sizeof(uint32_t);
            uint32_t packed = 0;
            packed |= static_cast<uint32_t>(bytes[offset + 0]);
            packed |= static_cast<uint32_t>(bytes[offset + 1]) << 8u;
            packed |= static_cast<uint32_t>(bytes[offset + 2]) << 16u;
            packed |= static_cast<uint32_t>(bytes[offset + 3]) << 24u;

            const double red = static_cast<double>(packed & 0x3ffu) / 1023.0;
            const double green =
                static_cast<double>((packed >> 10u) & 0x3ffu) / 1023.0;
            const double blue =
                static_cast<double>((packed >> 20u) & 0x3ffu) / 1023.0;
            const double luma = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
            if (!std::isfinite(luma))
                return {};
            result.totalLuma += luma;
            weightedX += static_cast<double>(x) * luma;
            if (luma > 1.0e-4)
                ++result.nonzeroPixels;
        }
    }
    if (result.totalLuma > std::numeric_limits<double>::epsilon())
        result.centroidX = weightedX / result.totalLuma;
    return result;
}

void isolateUploaderEnvironment() {
    // The production uploader intentionally exposes campaign overrides via
    // environment variables. Clear those overrides here so this contract
    // always compares the same unsharpened, metadata-driven YUV conversion.
    for (const char *name : {
             "TFORGE_FSR4_INPUT_SHARPEN_STRENGTH",
             "TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709",
             "TFORGE_FSR4_CHROMA_PHASE",
             "TFORGE_FSR4_INPUT_TRANSFER",
             "TFORGE_FSR4_EXPERIMENTAL_REC709_INPUT_EOTF",
             "TFORGE_FSR4_CHROMA_FILTER",
             "TFORGE_FSR4_PRE_CAS"}) {
        unsetenv(name);
    }
}

bool uploadAndRead(GpuImageUploader &uploader, const DecodedVideoFrame &frame,
                   float jitterX, float jitterY, bool enabled,
                   Measurement &measurement) {
    uploader.setSharpness(0.0f);
    uploader.setCompareEnabled(false);
    uploader.setInputJitter(jitterX, jitterY, enabled);
    if (!uploader.uploadColor(frame))
        return false;

    std::vector<uint8_t> bytes;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!uploader.readbackModelColor(bytes, width, height) ||
        width != kWidth || height != kHeight)
        return false;
    measurement = measureModelColor(bytes, width, height);
    return measurement.totalLuma > 0.0 && measurement.nonzeroPixels > 0;
}

} // namespace

int main() {
    isolateUploaderEnvironment();
    VulkanContext context;
    if (!context.init(false)) {
        std::fprintf(stderr, "SKIP: no usable Vulkan device for jitter probe\n");
        return 77;
    }
    if (!requiredFormatSupport(context.physical())) {
        context.shutdown();
        return 77;
    }

    GpuImageUploader uploader;
    if (!uploader.init(context.physical(), context.device(), context.queue(),
                       context.queueFamily())) {
        std::fprintf(stderr, "SKIP: GpuImageUploader initialization unavailable\n");
        context.shutdown();
        return 77;
    }

    if (!uploader.allocate(kWidth, kHeight, kWidth, kHeight)) {
        std::fprintf(stderr, "SKIP: GpuImageUploader image allocation unavailable\n");
        uploader.destroy();
        context.shutdown();
        return 77;
    }

    const DecodedVideoFrame frame = makeDeterministicFrame();
    Measurement noJitter{};
    Measurement fixedJitter{};
    const bool noJitterOk = uploadAndRead(uploader, frame, 0.0f, 0.0f, false,
                                          noJitter);
    const bool fixedJitterOk = uploadAndRead(
        uploader, frame, kFixedJitterX, 0.0f, true, fixedJitter);

    uploader.destroy();
    context.shutdown();

    if (!noJitterOk || !fixedJitterOk) {
        std::fprintf(stderr, "FAIL: upload/readback of deterministic YUV420 frame\n");
        return 1;
    }

    const double deltaX = fixedJitter.centroidX - noJitter.centroidX;
    std::printf("jitter_gpu_contract_tests: device-backed model-color probe\n");
    std::printf("  no jitter:    centroid_x=%.6f total_luma=%.6f nonzero=%u\n",
                noJitter.centroidX, noJitter.totalLuma,
                noJitter.nonzeroPixels);
    std::printf("  +%.3fpx X:    centroid_x=%.6f total_luma=%.6f nonzero=%u\n",
                kFixedJitterX, fixedJitter.centroidX, fixedJitter.totalLuma,
                fixedJitter.nonzeroPixels);
    std::printf("  directional delta (jitter - no-jitter): %.6f pixels\n", deltaX);

    // The production shader samples at p + jitter. A positive X offset must
    // therefore make a finite bright feature appear at a smaller destination
    // X. Require a measurable, correctly signed movement rather than merely
    // accepting any byte-level difference.
    if (!(deltaX < -0.10)) {
        std::fprintf(stderr,
                     "FAIL: fixed positive jitter did not move the feature "
                     "toward smaller X (delta=%.6f)\n",
                     deltaX);
        return 1;
    }

    std::printf("jitter_gpu_contract_tests: OK (positive jitter moves model "
                "color left as p + jitter)\n");
    return 0;
}
