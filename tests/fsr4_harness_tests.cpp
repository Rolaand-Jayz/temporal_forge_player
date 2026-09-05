// fsr4_harness_tests.cpp — P-B integration test.
// Creates a real Vulkan device on the RX 7900 GRE, runs the capability probe,
// creates the Fsr4DispatchHarness, uploads the RE weight blob, and runs a
// dispatch. This proves the INT8 backend's pipeline + binding + GPU-queue
// path works end-to-end on RDNA3.
//
// Disabled from ctest by default (needs a live GPU + display-free context);
// run manually with: ./build/tests/fsr4_harness_tests
#include "backend/GpuCapabilityProbe.hpp"
#include "backend/WeightBlob.hpp"
#include "backend/Fsr4ProofRunner.hpp"
#include "render/Fsr4DispatchHarness.hpp"

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <string>

using namespace temporal_forge;

int main() {
    // --- Vulkan instance + AMD device ---
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "fsr4-harness-test"; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    const char* exts[] = { VK_KHR_SURFACE_EXTENSION_NAME };
    ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = exts;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        std::fprintf(stderr, "FATAL: vkCreateInstance failed\n"); return 2;
    }

    // Capability probe (auto-picks the AMD device).
    auto cap = GpuCapabilityProbe::probe(inst);
    std::printf("Capability: %s -> %s\n", cap.deviceName.c_str(),
                GpuCapabilityProbe::profileName(cap.profile));
    if (!cap.valid) {
        std::fprintf(stderr, "SKIP: GPU not viable for FSR4 INT8 (%s)\n",
                     cap.failReason.c_str());
        vkDestroyInstance(inst, nullptr);
        return 77;
    }

    // Create the logical device with compute + cooperative-matrix enabled.
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{}; qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* devExt[] = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME };
    VkPhysicalDeviceFeatures2 f2{}; f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features v13{};
    v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cmf{};
    cmf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    f2.pNext = &v12;
    v12.pNext = &v13;
    v13.pNext = &cmf;
    vkGetPhysicalDeviceFeatures2(cap.physicalDevice, &f2);
    // cmf.cooperativeMatrix must be true to enable it.
    if (cmf.cooperativeMatrix == VK_FALSE) {
        std::fprintf(stderr, "SKIP: cooperativeMatrix feature not enabled-able\n");
        vkDestroyInstance(inst, nullptr); return 77;
    }
    VkDeviceCreateInfo dci{}; dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = devExt;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(cap.physicalDevice, &dci, nullptr, &device) != VK_SUCCESS) {
        std::fprintf(stderr, "FATAL: vkCreateDevice failed\n");
        vkDestroyInstance(inst, nullptr); return 2;
    }
    VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);

    // --- Harness ---
    Fsr4DispatchHarness harness;
    if (!harness.init(cap.physicalDevice, device, queue, 0, cap)) {
        std::fprintf(stderr, "FAIL: harness init\n");
        vkDestroyDevice(device, nullptr); vkDestroyInstance(inst, nullptr); return 1;
    }

    // Allocate resources for a test frame.
    Fsr4DispatchResources r{};
    r.sourceWidth = 1280; r.sourceHeight = 720;
    r.outputWidth = 3840; r.outputHeight = 2160;
    if (!harness.allocateResources(r)) {
        std::fprintf(stderr, "FAIL: allocateResources\n");
        vkDestroyDevice(device, nullptr); vkDestroyInstance(inst, nullptr); return 1;
    }

    // Upload the RE weight blob (standard preset).
    std::string blobDir;
    for (const char* c : {
        "/mnt/workdrive/fsr-re/extracted/v410_initializers",
        "/mnt/workdrive/fsr-re/dist/fsr4-swap/extracted/v410_initializers",
        "RE-of-FSR-4.1.0-Upscaling-1.0/extracted/v410_initializers",
        "../RE-of-FSR-4.1.0-Upscaling-1.0/extracted/v410_initializers",
    }) {
        if (std::filesystem::exists(std::string(c) + std::string("/quality.bin"))) {
            blobDir = c; break;
        }
    }
    if (!blobDir.empty()) {
        auto loaded = WeightBlobLoader::load(Fsr4Preset::Quality, blobDir + "/quality.bin");
        if (loaded.ok) {
            auto view = WeightBlobLoader::view(loaded);
            if (!harness.uploadWeights(view)) {
                std::fprintf(stderr, "WARN: weight upload staged (full upload in P-C)\n");
            }
        }
    }

    // Seed synthetic input ONCE (not per-frame).
    harness.seedSyntheticInput();

    // Run the proof validation — the honesty gate (it times the dispatch internally).
    std::printf("\n=== Running FSR4 INT8 Proof Runner ===\n");
    const auto& actualRes = harness.resources();
    auto proof = Fsr4ProofRunner::run(harness, device, actualRes);
    std::printf("%s\n", proof.report.c_str());
    std::printf("Proof state: %s\n", Fsr4ProofRunner::stateName(proof.state));
    const bool proofPassed = proof.state == Fsr4ProofState::Passed;

    harness.destroy();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(inst, nullptr);
    if (!proofPassed) {
        std::fprintf(stderr, "fsr4_harness_tests: FAIL (proof gate did not pass)\n");
        return 1;
    }
    std::printf("fsr4_harness_tests: OK (harness init + weight stage + dispatch on %s)\n",
                cap.deviceName.c_str());
    return 0;
}
