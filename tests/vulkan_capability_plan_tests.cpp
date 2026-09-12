// vulkan_capability_plan_tests.cpp — hermetic contract test for the M-05
// device-request planner (pure functions in render/VulkanContext.hpp).
//
// No Vulkan calls, no GPU: the planner receives fabricated
// available-extension / available-feature sets and must produce the
// adjudicated request set:
//   - all-present          -> full baseline+FSR4-class request set
//   - no cooperative matrix-> baseline-only device, fsr4 flags false, no fatal
//   - no external memory   -> actionable fatal decision naming the extension
//   - no subgroup control  -> pipeline-without-required-size decision
//   - non-RADV AMD vendor  -> no +1000 selection bias
#include "render/VulkanContext.hpp"

#include <cstdio>
#include <set>
#include <string>

using namespace temporal_forge;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

bool contains(const std::vector<const char*>& v, const char* name) {
    for (const char* e : v)
        if (std::string(e) == name) return true;
    return false;
}

VulkanFeatureAvailability fullAvailability() {
    VulkanFeatureAvailability a;
    a.samplerAnisotropy = true;
    a.textureCompressionBC = true;
    a.shaderStorageImageWriteWithoutFormat = true;
    a.imageCubeArray = true;
    a.shaderStorageImageExtendedFormats = true;
    a.timelineSemaphore = true;
    a.shaderSubgroupExtendedTypes = true;
    a.shaderFloat16 = true;
    a.shaderInt8 = true;
    a.shaderIntegerDotProduct = true;
    a.storageBuffer8BitAccess = true;
    a.subgroupSizeControl = true;
    a.computeFullSubgroups = true;
    a.cooperativeMatrix = true;
    return a;
}

std::set<std::string> fullExtensions() {
    return {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
        VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
    };
}

VulkanSubgroupSizeBounds rdna3Bounds() {
    VulkanSubgroupSizeBounds b;
    b.known = true;
    b.minSize = 64;
    b.maxSize = 64;
    return b;
}

void testAllPresent() {
    const auto plan = planVulkanDeviceRequests(fullExtensions(), fullAvailability(),
                                                rdna3Bounds());
    check(!plan.fatal, "all-present: not fatal");
    check(plan.baselineExtensions.size() == 3, "all-present: 3 baseline extensions");
    check(plan.fsr4ClassExtensions.size() == 2,
          "all-present: coop-matrix + subgroup-size-control requested");
    check(contains(plan.fsr4ClassExtensions, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME),
          "all-present: subgroup-size-control extension inconsistency fixed");
    check(plan.enableSamplerAnisotropy && plan.enableTextureCompressionBC &&
              plan.enableImageCubeArray && plan.enableTimelineSemaphore &&
              plan.enableShaderSubgroupExtendedTypes,
          "all-present: baseline features enabled");
    check(plan.enableShaderFloat16 && plan.enableShaderInt8 &&
              plan.enableShaderIntegerDotProduct &&
              plan.enableStorageBuffer8BitAccess &&
              plan.enableSubgroupSizeControl &&
              plan.enableComputeFullSubgroups && plan.enableCooperativeMatrix,
          "all-present: FSR4-class features enabled");
    check(plan.fsr4ClassExtensionsPresent && plan.fsr4ClassFeaturesPresent,
          "all-present: fsr4 class flags true");
    check(plan.requireSubgroupSize64, "all-present: required subgroup size 64 requested");
    check(plan.warnings.empty(), "all-present: no warnings");
}

// N-3: missing uint8_t storage-buffer access must degrade FSR4 cleanly —
// fsr4-class features off, storage enable off, device still created with
// the full baseline set (not fatal, no blind requests).
void testMissing8BitStorage() {
    auto avail = fullAvailability();
    avail.storageBuffer8BitAccess = false;
    const auto plan =
        planVulkanDeviceRequests(fullExtensions(), avail, rdna3Bounds());
    check(!plan.fatal, "missing-8bit-storage: not fatal (baseline device)");
    check(!plan.fsr4ClassFeaturesPresent,
          "missing-8bit-storage: fsr4ClassFeatures false");
    // Extensions are still available (and still requested — e.g.
    // subgroup-size-control is used by the baseline-adjacent pipelines);
    // only the fsr4-class FEATURE summary degrades.
    check(plan.fsr4ClassExtensionsPresent,
          "missing-8bit-storage: fsr4ClassExtensions still present (extension-only flag)");
    check(!plan.enableStorageBuffer8BitAccess,
          "missing-8bit-storage: storage feature not requested");
    check(plan.enableSamplerAnisotropy && plan.enableShaderStorageImageWriteWithoutFormat,
          "missing-8bit-storage: baseline features still enabled");
    check(plan.enableShaderFloat16 && plan.enableShaderInt8,
          "missing-8bit-storage: available fsr4 arithmetic features still enabled");
    check(plan.enableSubgroupSizeControl && plan.enableComputeFullSubgroups &&
              plan.enableCooperativeMatrix,
          "missing-8bit-storage: other available fsr4 features still requested");
}

void testNoCoopMatrix() {
    auto exts = fullExtensions();
    exts.erase(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    const auto plan = planVulkanDeviceRequests(exts, fullAvailability(),
                                                rdna3Bounds());
    check(!plan.fatal, "no-coop-matrix: init still proceeds (baseline-only device)");
    check(!plan.fsr4ClassExtensionsPresent && !plan.fsr4ClassFeaturesPresent,
          "no-coop-matrix: fsr4 flags false");
    check(!plan.enableCooperativeMatrix,
          "no-coop-matrix: cooperativeMatrix feature not requested");
    check(plan.enableSamplerAnisotropy && plan.enableShaderStorageImageWriteWithoutFormat,
          "no-coop-matrix: baseline features still enabled");
}

void testNoExternalMemory() {
    auto exts = fullExtensions();
    exts.erase(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    const auto plan = planVulkanDeviceRequests(exts, fullAvailability(),
                                                rdna3Bounds());
    check(plan.fatal, "no-external-memory: fatal decision");
    check(plan.fatalMessage.find(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) !=
              std::string::npos,
          "no-external-memory: error names the missing extension");
    check(plan.fatalMessage.find("cannot run") != std::string::npos,
          "no-external-memory: error is actionable");
}

void testNoSubgroupControl() {
    auto exts = fullExtensions();
    exts.erase(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    const auto plan = planVulkanDeviceRequests(exts, fullAvailability(),
                                                rdna3Bounds());
    check(!plan.fatal, "no-subgroup-control: not fatal");
    check(!plan.requireSubgroupSize64,
          "no-subgroup-control: pipeline built without required subgroup size");
    check(!plan.fsr4ClassFeaturesPresent,
          "no-subgroup-control: fsr4-class feature set incomplete");
    check(!plan.enableSubgroupSizeControl && !plan.enableComputeFullSubgroups,
          "no-subgroup-control: features not requested without the extension");
    // Out-of-bounds case: extension and features present, 64 unsupported.
    VulkanSubgroupSizeBounds small;
    small.known = true;
    small.minSize = 8;
    small.maxSize = 32;
    const auto plan2 =
        planVulkanDeviceRequests(fullExtensions(), fullAvailability(), small);
    check(!plan2.fatal && !plan2.requireSubgroupSize64,
          "no-subgroup-control: 64 out of bounds -> no required size");
}

void testFatalBaselineFeatures() {
    auto avail = fullAvailability();
    avail.shaderStorageImageWriteWithoutFormat = false;
    const auto plan =
        planVulkanDeviceRequests(fullExtensions(), avail, rdna3Bounds());
    check(plan.fatal && plan.fatalMessage.find("shaderStorageImageWriteWithoutFormat") !=
                            std::string::npos,
          "no-storage-write: fatal with actionable message");

    auto avail2 = fullAvailability();
    avail2.shaderStorageImageExtendedFormats = false;
    const auto plan2 =
        planVulkanDeviceRequests(fullExtensions(), avail2, rdna3Bounds());
    check(plan2.fatal &&
              plan2.fatalMessage.find("shaderStorageImageExtendedFormats") !=
                  std::string::npos,
          "no-extended-formats: fatal with actionable message");
}

void testTolerableBaselineFeatures() {
    auto avail = fullAvailability();
    avail.samplerAnisotropy = false;
    avail.textureCompressionBC = false;
    avail.imageCubeArray = false;
    avail.timelineSemaphore = false;
    const auto plan =
        planVulkanDeviceRequests(fullExtensions(), avail, rdna3Bounds());
    check(!plan.fatal, "tolerable-features: not fatal");
    check(!plan.enableSamplerAnisotropy && !plan.enableTextureCompressionBC &&
              !plan.enableImageCubeArray && !plan.enableTimelineSemaphore,
          "tolerable-features: disabled");
    check(plan.warnings.size() == 4, "tolerable-features: warned per feature");
}

void testAmdRadvPreference() {
    check(planAmdRadvPreference(true, true, 0x1002),
          "radv-driver: +1000 bias applies");
    check(!planAmdRadvPreference(true, false, 0x1002),
          "amd-but-not-radv: no +1000 bias");
    check(!planAmdRadvPreference(true, false, 0x10DE),
          "nvidia: no bias");
    check(planAmdRadvPreference(false, false, 0x1002),
          "driver-props-unavailable: vendor-ID fallback keeps AMD bias");
    check(!planAmdRadvPreference(false, false, 0x8086),
          "driver-props-unavailable: non-AMD gets no bias");
}

} // namespace

int main() {
    testAllPresent();
    testMissing8BitStorage();
    testNoCoopMatrix();
    testNoExternalMemory();
    testNoSubgroupControl();
    testFatalBaselineFeatures();
    testTolerableBaselineFeatures();
    testAmdRadvPreference();
    if (failures == 0) {
        std::printf("vulkan_capability_plan_tests: ALL PASSED\n");
        return 0;
    }
    std::printf("vulkan_capability_plan_tests: %d FAILURES\n", failures);
    return 1;
}
