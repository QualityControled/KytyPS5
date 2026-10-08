#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace Libs::Graphics::ShaderRecompiler::Diagnostics {

inline constexpr size_t BvhDiagnosticWords = 32;
inline constexpr size_t BvhDiagnosticExtraWords = BvhDiagnosticWords - 8;
inline constexpr size_t BvhRayWord = 16;
inline constexpr size_t BvhRayWords = 10;
inline constexpr size_t BvhNodeWidthWord = 26;
inline constexpr size_t BvhNativeLaneWord = 27;
inline constexpr size_t BvhSchemaMagicWord = 28;
inline constexpr size_t BvhSchemaVersionWord = 29;
inline constexpr uint32_t BvhSchemaMagic = 0x42564852u;
inline constexpr uint32_t BvhSchemaVersion = 1u;

// The sidecar belongs to the same active invocation/half that won the event CAS.
// Zero node-high bits cannot establish the native opcode's pointer width.
inline constexpr bool ValidBvhRaySidecar(std::span<const uint32_t> words) {
    if (words.size() != BvhDiagnosticWords || words[0] != 1u || words[1] != 7u ||
        (words[BvhNodeWidthWord] != 1u && words[BvhNodeWidthWord] != 2u) ||
        words[BvhNativeLaneWord] >= 64u || words[BvhSchemaMagicWord] != BvhSchemaMagic ||
        words[BvhSchemaVersionWord] != BvhSchemaVersion || words[30] != 0u || words[31] != 0u ||
        (words[BvhNodeWidthWord] == 1u && words[13] != 0u)) return false;
    const auto exec = uint64_t{words[14]} | (uint64_t{words[15]} << 32u);
    return ((exec >> words[BvhNativeLaneWord]) & 1u) != 0u;
}

} // namespace Libs::Graphics::ShaderRecompiler::Diagnostics
