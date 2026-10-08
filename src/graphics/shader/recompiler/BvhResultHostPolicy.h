#pragma once

#include "graphics/shader/recompiler/BvhDiagnosticRecord.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>

namespace Libs::Graphics::ShaderRecompiler::Diagnostics {

// Work-only host candidate. The opt-in is frozen once per process before the
// first cache/binding. A default process continues to allocate exactly128B.
struct AfterBvhEnvironment {
    const char* after = nullptr;
    const char* target = nullptr;
    const char* before = nullptr;
    const char* checked_vgpr = nullptr;
    const char* resource_capture = nullptr;
    const char* input_capture = nullptr;
    const char* structured = nullptr;
};
enum class AfterBvhModeError {
    None, InvalidValue, MissingTargetProbe, BeforeConflict, CheckedConflict,
    ResourceCaptureConflict, InputCaptureConflict, InvalidStructuredValue
};
struct AfterBvhMode {
    bool enabled = false;
    bool structured = false;
    AfterBvhModeError error = AfterBvhModeError::None;
};
inline AfterBvhMode ParseAfterBvhMode(const AfterBvhEnvironment& e) {
    if (!e.after) return {};
    const auto one = [](const char* value) { return value && std::strcmp(value, "1") == 0; };
    if (!one(e.after)) return {false,false,AfterBvhModeError::InvalidValue};
    if (!one(e.target)) return {false,false,AfterBvhModeError::MissingTargetProbe};
    if (e.before) return {false,false,AfterBvhModeError::BeforeConflict};
    if (e.checked_vgpr) return {false,false,AfterBvhModeError::CheckedConflict};
    if (e.resource_capture) return {false,false,AfterBvhModeError::ResourceCaptureConflict};
    if (e.input_capture) return {false,false,AfterBvhModeError::InputCaptureConflict};
    if (e.structured && !one(e.structured)) return {false,false,AfterBvhModeError::InvalidStructuredValue};
    return {true,one(e.structured),AfterBvhModeError::None};
}
inline const AfterBvhMode& FrozenAfterBvhMode() {
    static const auto mode = ParseAfterBvhMode({
        std::getenv("KYTY_PROBE_EXTERNAL_AFTER_BVH"),
        std::getenv("KYTY_PROBE_EXTERNAL_CALL_TARGET"),
        std::getenv("KYTY_PROBE_EXTERNAL_BEFORE_BVH"),
        std::getenv("KYTY_EXTERNAL_UNWRITTEN_VGPR"),
        std::getenv("KYTY_CAPTURE_EXTERNAL_RESOURCE_READS"),
        std::getenv("KYTY_CAPTURE_EXTERNAL_INPUTS_ONLY"),
        std::getenv("KYTY_PROBE_EXTERNAL_STRUCTURED")});
    return mode;
}
inline const char* AfterBvhModeErrorName(AfterBvhModeError error) {
    switch (error) {
        case AfterBvhModeError::None:return "none";
        case AfterBvhModeError::InvalidValue:return "after-BVH requires value1";
        case AfterBvhModeError::MissingTargetProbe:return "after-BVH requires target-probe1";
        case AfterBvhModeError::BeforeConflict:return "before/after BVH are mutually exclusive";
        case AfterBvhModeError::CheckedConflict:return "after-BVH conflicts with checked VGPR mode";
        case AfterBvhModeError::ResourceCaptureConflict:return "after-BVH requires separate resource-read capture run";
        case AfterBvhModeError::InputCaptureConflict:return "after-BVH requires separate input-only capture run";
        case AfterBvhModeError::InvalidStructuredValue:return "structured probe requires value1";
    }
    return "unknown invalid mode";
}
inline constexpr uint64_t FaultRecordBytes(bool after_bvh) {
    return (after_bvh ? BvhResultDiagnosticWords : BvhDiagnosticWords) * sizeof(uint32_t);
}
inline constexpr bool FaultAreaStride(uint64_t bytes,uint64_t atom,uint64_t areas,uint64_t& stride) {
    if ((bytes != FaultRecordBytes(false) && bytes != FaultRecordBytes(true)) || atom == 0 || areas == 0)
        return false;
    const auto remainder=bytes%atom;
    const auto padding=remainder==0 ? 0 : atom-remainder;
    if (padding>std::numeric_limits<uint64_t>::max()-bytes) return false;
    const auto aligned=bytes+padding;
    if (aligned>std::numeric_limits<uint64_t>::max()/areas) return false;
    stride=aligned;return true;
}
enum class BvhHostRecordKind { Empty, Ordinary, BeforeBvh, AfterBvh, Invalid };
inline BvhHostRecordKind ClassifyBvhHostRecord(std::span<const uint32_t> storage,bool after_bvh) {
    const auto words=after_bvh?BvhResultDiagnosticWords:BvhDiagnosticWords;
    if (storage.size()!=words) return BvhHostRecordKind::Invalid;
    if (storage[0]==0) return BvhHostRecordKind::Empty;
    if (storage[0]!=1) return BvhHostRecordKind::Invalid;
    if (storage[1]==7)
        return ValidBvhRaySidecar(storage.first(BvhDiagnosticWords)) ? BvhHostRecordKind::BeforeBvh : BvhHostRecordKind::Invalid;
    if (storage[1]==8)
        return after_bvh && ValidBvhResultSidecar(storage) ? BvhHostRecordKind::AfterBvh : BvhHostRecordKind::Invalid;
    return BvhHostRecordKind::Ordinary;
}
inline std::array<uint8_t,BvhResultDiagnosticWords*4> EncodeBvhResultLittleEndian(
    std::span<const uint32_t,BvhResultDiagnosticWords> record) {
    std::array<uint8_t,BvhResultDiagnosticWords*4> bytes{};
    for(size_t word=0;word<record.size();++word)
        for(size_t byte=0;byte<4;++byte)bytes[word*4+byte]=static_cast<uint8_t>(record[word]>>(byte*8));
    return bytes;
}
} // namespace Libs::Graphics::ShaderRecompiler::Diagnostics
