// gpu_probe.cpp — standalone GPU capability probe for FSR 4.1 INT8.
// Creates a Vulkan instance, runs the capability probe, prints the result.
// This is the P-A verification that the RX 7900 GRE / RADV exposes the
// INT8 cooperative-matrix path FSR 4.1.1 RDNA3 requires.
#include "backend/GpuCapabilityProbe.hpp"

#include <vulkan/vulkan.h>
#include <cstdio>

using namespace temporal_forge;

int main() {
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "tf-gpu-probe";
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        std::fprintf(stderr, "FATAL: cannot create Vulkan instance\n");
        return 2;
    }

    auto cap = GpuCapabilityProbe::probe(inst);  // instance overload auto-picks AMD
    std::printf("=== FSR 4.1 INT8 Capability Probe ===\n");
    std::printf("Device:        %s\n", cap.deviceName.c_str());
    std::printf("Driver:        %s %s\n", cap.driverName.c_str(), cap.driverInfo.c_str());
    std::printf("API:           %u.%u\n",
                VK_API_VERSION_MAJOR(cap.apiVersion), VK_API_VERSION_MINOR(cap.apiVersion));
    std::printf("RDNA3:         %s\n", cap.isRdna3 ? "yes" : "no");
    std::printf("RDNA4:         %s\n", cap.isRdna4 ? "yes" : "no");
    std::printf("CoopMatrix ext:%s\n", cap.hasCooperativeMatrix ? "yes" : "no");
    std::printf("INT8 input:    %s\n", cap.hasUint8Input ? "yes" : "no");
    std::printf("INT32 accum:   %s\n", cap.hasInt32Accum ? "yes" : "no");
    std::printf("FP16 fallback: %s\n", cap.hasFp16Fallback ? "yes" : "no");
    std::printf("Profile:       %s\n", GpuCapabilityProbe::profileName(cap.profile));
    std::printf("Viable:        %s\n", cap.valid ? "YES" : "NO");
    if (!cap.valid)
        std::printf("Fail reason:   %s\n", cap.failReason.c_str());

    vkDestroyInstance(inst, nullptr);
    return cap.valid ? 0 : 1;
}
