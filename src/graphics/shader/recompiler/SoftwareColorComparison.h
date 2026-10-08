#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace SoftwareColorComparison {

enum class Mode : uint8_t { ExistingNativePath, SoftwarePointR8, SoftwareLinearR8, RejectUnsupported };
enum class Reason : uint8_t {
    Disabled, NotComparison, NativeCapability, MissingCapability, UnsupportedImage,
    UnsupportedInstruction, UnsupportedSampler, SupportedPointR8, SupportedLinearR8
};
struct Decision { Mode mode; Reason reason; };
struct Scope {
    bool enabled = false, capability_known = false, native_compare_supported = false;
    bool comparison = false, direct = true, sampled = true, read_only = true;
    bool image_sample_opcode = true, converted = false, status = false;
    uint32_t host_format = 0, dimensions = 2, samples = 1, mip_levels = 1;
    uint32_t data_bits = 32, dmask = 1, width = 1, height = 1;
    std::array<uint32_t, 4> sampler {};
};

inline Decision Classify(const Scope& s) {
    if (!s.enabled) return {Mode::ExistingNativePath, Reason::Disabled};
    if (!s.comparison) return {Mode::ExistingNativePath, Reason::NotComparison};
    if (!s.capability_known) return {Mode::RejectUnsupported, Reason::MissingCapability};
    if (s.native_compare_supported) return {Mode::ExistingNativePath, Reason::NativeCapability};
    // Supported experimental scope is R8_UNORM with point or four-tap bilinear comparison.
    if (s.host_format != 9 || s.dimensions != 2 || s.samples != 1 || s.mip_levels != 1 ||
        s.width == 0 || s.height == 0 || !s.direct || !s.sampled || !s.read_only || s.converted)
        return {Mode::RejectUnsupported, Reason::UnsupportedImage};
    if (!s.image_sample_opcode || s.status || s.data_bits != 32 || s.dmask == 0 || s.dmask > 15)
        return {Mode::RejectUnsupported, Reason::UnsupportedInstruction};
    const auto word0 = s.sampler[0], word2 = s.sampler[2], word3 = s.sampler[3];
    const auto mag = (word2 >> 20) & 3, min = (word2 >> 22) & 3;
    const auto point = mag == 0 && min == 0;
    const auto linear = mag == 1 && min == 1;
    const auto mip_none = ((word2 >> 26) & 3) == 0;
    const auto clamp = [](uint32_t value) { return value == 0 || value == 2 || value == 6; };
    // Reuse unchanged native sampler addressing/LOD/border. Unknown native approximations stay out.
    if ((!point && !linear) || !mip_none || (word0 & (1u << 15)) || ((word0 >> 29) & 3) != 0 ||
        ((word0 >> 20) & 1) || ((word0 >> 27) & 1) || ((word2 >> 14) & 63) ||
        ((word2 >> 30) & 1) || ((word3 >> 30) & 3) == 3 ||
        !clamp(word0 & 7) || !clamp((word0 >> 3) & 7))
        return {Mode::RejectUnsupported, Reason::UnsupportedSampler};
    return linear ? Decision{Mode::SoftwareLinearR8, Reason::SupportedLinearR8}
                  : Decision{Mode::SoftwarePointR8, Reason::SupportedPointR8};
}

enum class SamplerVariant : uint8_t { NonComparisonFloat, NativeComparisonFloat, Integer, PointInteger };
inline SamplerVariant FloatVariant(bool comparison, bool software) {
    return comparison && !software ? SamplerVariant::NativeComparisonFloat
                                   : SamplerVariant::NonComparisonFloat;
}
inline std::array<uint32_t, 4> NonComparisonSampler(std::array<uint32_t, 4> raw) {
    raw[0] &= ~(7u << 12u);
    return raw; // No filtering/wrapping/LOD/border fields change.
}

// Independent policy model for owned finite reference tests, not an AMD-hardware oracle.
// A NaN operand is explicitly unproved rather than silently assigned an output.
inline std::optional<float> Compare(float reference, float texel, uint32_t function, bool unorm) {
    if (std::isnan(reference) || std::isnan(texel) || function > 7) return {};
    if (unorm) reference = std::clamp(reference, 0.0f, 1.0f);
    bool result = false;
    switch (function) {
        case 0: break;
        case 1: result = reference < texel; break;
        case 2: result = reference == texel; break;
        case 3: result = reference <= texel; break;
        case 4: result = reference > texel; break;
        case 5: result = reference != texel; break;
        case 6: result = reference >= texel; break;
        case 7: result = true; break;
    }
    return result ? 1.0f : 0.0f;
}

// Native T# red selector: ZERO=0, ONE=1, R/G/B/A=4/5/6/7.
// Declared emulation policy: replace source R with comparison and project the R8 tuple
// {comparison,0,0,1}. AMD color-comparison ordering has not been independently proved.
inline std::optional<float> ProjectRed(float comparison, uint32_t guest_red_selector) {
    switch (guest_red_selector) {
        case 0: case 5: case 6: return 0.0f;
        case 1: case 7: return 1.0f;
        case 4: return comparison;
        default: return {};
    }
}

inline std::optional<float> PointClampBorderR8(std::span<const uint8_t> pixels,
    uint32_t width, uint32_t height, float u, float v, float border) {
    if (!width || !height || pixels.size() != uint64_t(width) * height ||
        !std::isfinite(u) || !std::isfinite(v)) return {};
    const auto x = std::floor(double(u) * width), y = std::floor(double(v) * height);
    if (x < 0 || y < 0 || x >= width || y >= height) return border;
    return pixels[uint64_t(y) * width + uint64_t(x)] / 255.0f;
}

inline std::optional<float> LinearCompareClampBorderR8(std::span<const uint8_t> pixels,
    uint32_t width, uint32_t height, float u, float v, float border, float reference,
    uint32_t function) {
    if (!width || !height || pixels.size() != uint64_t(width) * height ||
        !std::isfinite(u) || !std::isfinite(v)) return {};
    const float px = u * width - .5f, py = v * height - .5f;
    const float lx = std::floor(px), ly = std::floor(py), fx = px-lx, fy = py-ly;
    const auto tap = [&](float x, float y) -> std::optional<float> {
        const float value = x < 0 || y < 0 || x >= width || y >= height ? border
            : pixels[uint64_t(y) * width + uint64_t(x)] / 255.0f;
        return Compare(reference,value,function,true);
    };
    const auto a=tap(lx,ly), b=tap(lx+1.f,ly), c=tap(lx,ly+1.f), d=tap(lx+1.f,ly+1.f);
    if (!a || !b || !c || !d) return {};
    return (*a*(1.f-fx)+*b*fx)*(1.f-fy)+(*c*(1.f-fx)+*d*fx)*fy;
}

} // namespace SoftwareColorComparison
