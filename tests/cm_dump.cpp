// cm_dump.cpp — dump every cooperative-matrix mode the device exposes, so we
// know exactly what INT8/FP8/FP16 tile shapes RADV makes available.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace std;

const char* compName(VkComponentTypeKHR t) {
    switch (t) {
        case VK_COMPONENT_TYPE_FLOAT16_KHR: return "FLOAT16";
        case VK_COMPONENT_TYPE_FLOAT32_KHR: return "FLOAT32";
        case VK_COMPONENT_TYPE_FLOAT64_KHR: return "FLOAT64";
        case VK_COMPONENT_TYPE_SINT8_KHR:   return "SINT8";
        case VK_COMPONENT_TYPE_SINT16_KHR:  return "SINT16";
        case VK_COMPONENT_TYPE_SINT32_KHR:  return "SINT32";
        case VK_COMPONENT_TYPE_SINT64_KHR:  return "SINT64";
        case VK_COMPONENT_TYPE_UINT8_KHR:   return "UINT8";
        case VK_COMPONENT_TYPE_UINT16_KHR:  return "UINT16";
        case VK_COMPONENT_TYPE_UINT32_KHR:  return "UINT32";
        case VK_COMPONENT_TYPE_UINT64_KHR:  return "UINT64";
        default: return "?";
    }
}

int main() {
    VkInstanceCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3; ici.pApplicationInfo = &app;
    VkInstance inst; if (vkCreateInstance(&ici, 0, &inst)) return 2;
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,0);
    vector<VkPhysicalDevice> d(n); vkEnumeratePhysicalDevices(inst,&n,d.data());
    VkPhysicalDevice amd = VK_NULL_HANDLE;
    for (auto pd : d) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd,&p);
        if (strstr(p.deviceName,"7900")) { amd = pd; break; }
    }
    if (!amd) { printf("no AMD\n"); return 1; }

    // Resolve the extension function pointer.
    auto getProps = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
        vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    printf("getProps resolved: %s\n", getProps ? "yes" : "no");
    if (!getProps) { vkDestroyInstance(inst,0); return 1; }

    uint32_t mc = 0;
    VkResult r = getProps(amd, &mc, 0);
    printf("count query result: %d, modes: %u\n", r, mc);
    if (mc == 0) { vkDestroyInstance(inst,0); return 0; }

    vector<VkCooperativeMatrixPropertiesKHR> props(mc);
    for (auto& p : props) { p.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR; p.pNext=nullptr; }
    r = getProps(amd, &mc, props.data());
    printf("props query result: %d\n\n", r);
    printf("%-8s x %-8s -> %-8s (result %-8s)  MxNxK  sat=%c scope=%d\n",
           "A","B","C/D","R",' ',0);
    int int8modes = 0;
    for (const auto& p : props) {
        bool int8ish = (p.AType==VK_COMPONENT_TYPE_SINT8_KHR||p.AType==VK_COMPONENT_TYPE_UINT8_KHR||
                        p.BType==VK_COMPONENT_TYPE_SINT8_KHR||p.BType==VK_COMPONENT_TYPE_UINT8_KHR);
        if (int8ish) int8modes++;
        printf("%-8s x %-8s -> %-8s (result %-8s)  %ux%ux%u  sat=%c scope=%d%s\n",
               compName(p.AType), compName(p.BType), compName(p.CType), compName(p.ResultType),
               p.MSize, p.NSize, p.KSize,
               p.saturatingAccumulation ? 'Y':'N', (int)p.scope,
               int8ish ? "  <-- INT8" : "");
    }
    printf("\nINT8-touching modes: %d / %u\n", int8modes, mc);
    vkDestroyInstance(inst,0);
    return 0;
}
