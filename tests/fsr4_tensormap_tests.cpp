// fsr4_tensormap_tests.cpp — validate the RE tensor-map parses to 78 tensors
// with the documented encoder1 weight at offset 0.
#include "backend/Fsr4TensorMap.hpp"
#include <cstdio>
#include <filesystem>
using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { if(!(cond)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);++g_failures;} }while(0)

int main() {
    std::string path;
    for (const char* c : {
        "RE-of-FSR-4.1.0-Upscaling-1.0/spec/tensor-map.json",
        "../RE-of-FSR-4.1.0-Upscaling-1.0/spec/tensor-map.json",
    }) {
        if (std::filesystem::exists(c)) { path = c; break; }
    }
    if (path.empty()) { std::fprintf(stderr,"SKIP (tensor-map.json not found)\n"); return 77; }

    Fsr4TensorMap map;
    CHECK(map.loadFromJson(path));
    // The RE documents 78 tensors.
    CHECK(map.count() == 78);

    // encoder1 weight: offset 0, shape 2,2,7,16, 1024 bytes, HNWC layout.
    auto* enc1 = map.find("encoder1_DownscaleStridedConv2x2_downscale_conv_weight");
    CHECK(enc1 != nullptr);
    CHECK(enc1->offset == 0);
    CHECK(enc1->byteSize == 1024);
    CHECK(enc1->dims.size() == 4);
    CHECK(enc1->dims[0] == 2 && enc1->dims[1] == 2 && enc1->dims[2] == 7 && enc1->dims[3] == 16);
    CHECK(enc1->zone == Fsr4Tensor::Zone::BiasFp16); // 0 < 7208

    // encoder1 bias: offset 1024.
    auto* enc1b = map.find("encoder1_DownscaleStridedConv2x2_downscale_conv_bias");
    CHECK(enc1b != nullptr);
    CHECK(enc1b->offset == 1024);

    // The weight zone starts at offset 7208. A weight tensor there.
    bool foundWeight = false;
    for (const auto& t : map.tensors())
        if (t.zone == Fsr4Tensor::Zone::WeightUint8) { foundWeight = true; break; }
    CHECK(foundWeight);

    if (g_failures == 0) { std::printf("fsr4_tensormap_tests: OK (78 tensors)\n"); return 0; }
    std::fprintf(stderr, "fsr4_tensormap_tests: %d FAILURES\n", g_failures);
    return 1;
}
