#pragma once

#include "graphics/shader/recompiler/ExternalLibrary.h"
#include "graphics/shader/recompiler/ShaderCallDiagnostics.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>

namespace CapturedExternalTest {
namespace Shader = Libs::Graphics::ShaderRecompiler;

inline void Require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "CapturedExternalFixture: %s\n", message); std::exit(1); }
}

inline std::vector<uint32_t> ReadWords(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(bool(file), "cannot open explicitly selected captured binary");
    const auto bytes = file.tellg();
    Require(bytes > 0 && bytes <= 1024 * 1024 && bytes % 4 == 0, "captured file exceeds bounded aligned input size");
    std::vector<uint32_t> words(static_cast<size_t>(bytes) / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), bytes);
    Require(bool(file), "captured read was incomplete");
    return words;
}

struct Fixture {
    std::vector<uint32_t> caller;
    Shader::ExternalLibraryPlan library;
    size_t total_functions = 0;
    size_t total_records = 0;
};

// Every candidate comes from this single captured table and its own captured prefix.
// A smaller limit constructs a synthetic subset for offline translation scale only;
// it cannot establish correctness for the omitted live dispatch targets or contexts.
inline Fixture Load(const char* caller_path, const char* folder_path, size_t limit) {
    Fixture fixture;
    fixture.caller = ReadWords(caller_path);
    const auto table = ReadWords(std::filesystem::path(folder_path) / "table_00.bin");
    Require(table.size() % 4u == 0u, "captured table has a partial native record");
    std::map<uint64_t, std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(folder_path)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("target_") || !name.ends_with(".bin")) continue;
        const auto begin = name.find_last_of('_') + 1u;
        const auto hex = std::string_view(name).substr(begin, name.size() - begin - 4u);
        uint64_t address = 0;
        const auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), address, 16);
        Require(parsed.ec == std::errc{} && parsed.ptr == hex.data() + hex.size() &&
                    address != 0 && address < (uint64_t{1} << 48u) && (address & 3u) == 0,
                "captured prefix filename has an invalid entry address");
        Require(paths.emplace(address, entry.path()).second, "duplicate captured prefix address");
    }
    fixture.total_functions = paths.size();
    fixture.total_records = table.size() / 4u;
    Require(limit > 0u && limit <= paths.size(), "candidate subset exceeds captured library");
    std::set<uint64_t> targets;
    for (size_t record = 0; record < fixture.total_records; ++record) {
        const auto code = uint64_t{table[record * 4u]} | uint64_t{table[record * 4u + 1u]} << 32u;
        Require(paths.contains(code), "captured table entry lacks its captured code prefix");
        targets.insert(code);
    }
    Require(targets.size() == paths.size(), "captured prefix is not proven by this captured table");
    std::map<uint64_t, uint32_t> ids;
    for (const auto& [address, path] : paths) {
        if (ids.size() == limit) break;
        auto words = ReadWords(path);
        Require(words.size() * sizeof(uint32_t) == 65536u, "candidate prefix is incomplete");
        const auto id = static_cast<uint32_t>(ids.size());
        ids.emplace(address, id);
        fixture.library.functions.push_back({id, address, std::move(words)});
    }
    Shader::Decoder::Program caller;
    Shader::Decoder::DecodeProgram(fixture.caller, caller);
    std::array<uint32_t, 64> dummy_user {};
    dummy_user[0] = 0x2000u; dummy_user[1] = 0x10u;
    const auto traced = Shader::Diagnostics::TraceCallTables(caller, dummy_user);
    Require(traced.calls.size() == 1u && !traced.call_sites_truncated && traced.calls[0].rejection.empty(),
            "captured caller did not prove one function table origin");
    const auto& trace = traced.calls[0];
    Shader::ExternalCallSite site;
    site.caller_pc = trace.call_pc; site.target_sgpr = trace.target_sgpr; site.return_sgpr = trace.return_sgpr;
    site.record_load_pc = trace.record_load_pc; site.record_sgpr = 4u;
    site.context_domain = 0; site.auxiliary_sgpr = 16u;
    for (const auto& [address, id] : ids) site.candidate_addresses.push_back(address);
    for (uint32_t record = 0; record < fixture.total_records; ++record) {
        const auto first = record * 4u;
        const auto code = uint64_t{table[first]} | uint64_t{table[first + 1u]} << 32u;
        const auto found = ids.find(code); if (found == ids.end()) continue;
        const auto aux = uint64_t{table[first + 2u]} | uint64_t{table[first + 3u]} << 32u;
        site.records.push_back({record, found->second, code, aux});
        site.context_records.push_back({record, found->second,
            {table[first], table[first + 1u], table[first + 2u], table[first + 3u]}});
    }
    // Capture omitted the caller base; this is explicitly synthetic and does not validate live link bits.
    fixture.library.caller_address = 0x1450008000ull;
    fixture.library.call_sites.push_back(std::move(site));
    fixture.library.complete = true;
    return fixture;
}
} // namespace CapturedExternalTest
