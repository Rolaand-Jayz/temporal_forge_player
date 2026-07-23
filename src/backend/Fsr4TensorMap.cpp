// Fsr4TensorMap.cpp — parse the RE tensor-map.json.
#include "backend/Fsr4TensorMap.hpp"
#include "util/Log.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace temporal_forge {

namespace {

// Tiny JSON helpers (schema-specific). The tensor-map.json is a flat array of
// objects with string fields, so a robust general JSON parser is overkill.

// Extract the string value of "key" from a JSON object body (the substring
// between { and the matching }).
std::string extractJsonString(std::string_view body, std::string_view key) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto k = body.find(needle);
    if (k == std::string_view::npos) return {};
    auto colon = body.find(':', k);
    if (colon == std::string_view::npos) return {};
    auto q1 = body.find('"', colon);
    if (q1 == std::string_view::npos) return {};
    auto q2 = body.find('"', q1 + 1);
    if (q2 == std::string_view::npos) return {};
    return std::string(body.substr(q1 + 1, q2 - q1 - 1));
}

long extractJsonInt(std::string_view body, std::string_view key, long fallback) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto k = body.find(needle);
    if (k == std::string_view::npos) return fallback;
    auto colon = body.find(':', k);
    if (colon == std::string_view::npos) return fallback;
    size_t i = colon + 1;
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    long v = 0; bool any = false; bool neg = false;
    if (i < body.size() && body[i] == '-') { neg = true; ++i; }
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
        v = v * 10 + (body[i] - '0'); ++i; any = true;
    }
    return any ? (neg ? -v : v) : fallback;
}

// Parse the "shape": "2, 2, 7, 16" into a list of ints.
std::vector<uint32_t> parseShape(const std::string& shape) {
    std::vector<uint32_t> out;
    std::stringstream ss(shape);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        // trim whitespace
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        try { out.push_back(static_cast<uint32_t>(std::stoul(tok.substr(a, b - a + 1)))); }
        catch (...) {}
    }
    return out;
}

Fsr4Tensor::Zone zoneForOffset(uint32_t offset) {
    if (offset < 7208) return Fsr4Tensor::Zone::BiasFp16;
    if (offset < 130088) return Fsr4Tensor::Zone::WeightUint8;
    return Fsr4Tensor::Zone::ScaleFp16;
}

} // namespace

std::vector<Fsr4Tensor> parseTensorMapJson(const std::string& body) {
    std::vector<Fsr4Tensor> out;
    // Locate the "tensors": [ ... ] array and bound the scan to within it.
    // (The JSON has other top-level keys after "tensors"; bounding prevents
    // catching their objects.)
    auto arr = body.find("\"tensors\"");
    if (arr == std::string::npos) return out;
    auto lbracket = body.find('[', arr);
    if (lbracket == std::string::npos) return out;
    // Find the matching close bracket at depth 0.
    auto rbracket = body.find(']', lbracket);
    if (rbracket == std::string::npos) return out;

    size_t i = lbracket + 1;
    while (i < rbracket) {
        // Find the next '{' opening an object within the array.
        size_t open = body.find('{', i);
        if (open == std::string::npos || open >= rbracket) break;
        // Match braces to find the closing '}'.
        int depth = 0;
        size_t close = open;
        for (; close < rbracket; ++close) {
            if (body[close] == '{') ++depth;
            else if (body[close] == '}') { --depth; if (depth == 0) break; }
        }
        if (close >= rbracket) break;
        std::string_view obj(body.data() + open, close - open + 1);

        Fsr4Tensor t;
        t.pass = extractJsonString(obj, "pass");
        t.name = extractJsonString(obj, "name");
        t.offset = static_cast<uint32_t>(extractJsonInt(obj, "offset", 0));
        t.shape = extractJsonString(obj, "shape");
        t.storageSize = extractJsonString(obj, "storage_size");
        t.tensorType = extractJsonString(obj, "tensor_type");
        t.byteSize = static_cast<uint32_t>(extractJsonInt(obj, "byte_size", 0));
        t.dims = parseShape(t.shape);
        t.zone = zoneForOffset(t.offset);
        out.push_back(std::move(t));

        i = close + 1;
    }
    return out;
}

bool Fsr4TensorMap::loadFromJson(const std::string& path) {
    std::ifstream f(path);
    if (!f) { logError("Fsr4TensorMap: cannot open {}", path); return false; }
    std::stringstream ss; ss << f.rdbuf();
    const std::string body = ss.str();
    tensors_ = parseTensorMapJson(body);
    if (tensors_.empty()) { logError("Fsr4TensorMap: parsed 0 tensors from {}", path); return false; }
    loaded_ = true;
    logInfo("Fsr4TensorMap: loaded {} tensors from {}", tensors_.size(), path);
    return true;
}

const Fsr4Tensor* Fsr4TensorMap::find(const std::string& nameContains) const {
    for (const auto& t : tensors_)
        if (t.name.find(nameContains) != std::string::npos) return &t;
    return nullptr;
}

std::vector<const Fsr4Tensor*> Fsr4TensorMap::tensorsForPass(const std::string& pass) const {
    std::vector<const Fsr4Tensor*> out;
    for (const auto& t : tensors_)
        if (t.pass == pass) out.push_back(&t);
    return out;
}

} // namespace temporal_forge
