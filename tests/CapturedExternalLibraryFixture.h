#pragma once

#include "CapturedComputeManifest.h"
#include "graphics/shader/recompiler/ExternalLibrary.h"
#include "graphics/shader/recompiler/ShaderCallDiagnostics.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace CapturedExternalTest {
namespace Shader = Libs::Graphics::ShaderRecompiler;

inline void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "CapturedExternalFixture: %s\n", message);
    std::exit(1);
  }
}
inline void RequireParse(bool condition, const std::string &failure) {
  Require(condition, failure.c_str());
}

inline std::vector<uint32_t> ReadWords(const std::filesystem::path &path,
                                       size_t max_bytes = 1024u * 1024u) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  Require(bool(file), "cannot open explicitly selected captured binary");
  const auto bytes = file.tellg();
  Require(bytes > 0 && bytes <= max_bytes && bytes % 4 == 0,
          "captured file exceeds bounded aligned input size");
  std::vector<uint32_t> words(static_cast<size_t>(bytes) / sizeof(uint32_t));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(words.data()), bytes);
  Require(bool(file), "captured read was incomplete");
  return words;
}

struct Region {
  uint64_t address;
  std::vector<uint32_t> words;
};

struct CapturedMemory {
  std::vector<Region> regions;
  bool report_unmapped = false;
  size_t denied_read_requests = 0;
  void Merge() {
    std::ranges::sort(regions, {}, &Region::address);
    std::vector<Region> merged;
    for (auto &region : regions) {
      Require(region.address < (uint64_t{1} << 48u) &&
                  (region.address & 3u) == 0u && !region.words.empty() &&
                  region.words.size() * sizeof(uint32_t) <=
                      (uint64_t{1} << 48u) - region.address,
              "captured region has an invalid address span");
      if (merged.empty() ||
          region.address >
              merged.back().address + merged.back().words.size() * 4u) {
        merged.push_back(std::move(region));
        continue;
      }
      auto &previous = merged.back();
      const size_t offset =
          static_cast<size_t>((region.address - previous.address) / 4u);
      const size_t overlap =
          std::min(region.words.size(), previous.words.size() - offset);
      Require(std::equal(region.words.begin(), region.words.begin() + overlap,
                         previous.words.begin() + offset),
              "captured mapped regions conflict in overlapping bytes");
      previous.words.insert(previous.words.end(),
                            region.words.begin() + overlap, region.words.end());
    }
    regions = std::move(merged);
  }
  static bool Read(void *context, uint64_t address,
                   std::span<uint32_t> output) {
    auto &memory = *static_cast<CapturedMemory *>(context);
    const auto &regions = memory.regions;
    const auto unavailable = [&] {
      ++memory.denied_read_requests;
      if (memory.report_unmapped && memory.denied_read_requests <= 16u)
        std::fprintf(
            stderr,
            "CapturedMemory: unmapped request address=0x%016llx dwords=%zu\n",
            static_cast<unsigned long long>(address), output.size());
      return false;
    };
    if (output.empty() || (address & 3u) != 0u)
      return unavailable();
    const auto after =
        std::upper_bound(regions.begin(), regions.end(), address,
                         [](uint64_t value, const Region &region) {
                           return value < region.address;
                         });
    if (after == regions.begin())
      return unavailable();
    const auto &region = *std::prev(after);
    const uint64_t offset = (address - region.address) / 4u;
    if (offset > region.words.size() ||
        output.size() > region.words.size() - offset)
      return unavailable();
    std::copy_n(region.words.begin() + static_cast<size_t>(offset),
                output.size(), output.begin());
    return true;
  }
};

struct Fixture {
  std::vector<uint32_t> caller;
  Shader::ExternalLibraryPlan library;
  CapturedMemory memory;
  DirectUserEntries direct_user;
  ComputeControls controls;
  size_t total_functions = 0;
  size_t total_records = 0;
  bool synthetic_inputs = false;
};

inline bool VerifyDirectUserOrigins(const DirectUserEntries &entries,
                                    const Shader::Decoder::Program &caller,
                                    const ComputeControls &controls,
                                    CapturedMemory &memory,
                                    std::string &failure) {
  if (!entries.present)
    return true; // Older captures remain translation-only inputs.
  struct Reader {
    CapturedMemory &memory;
    const DirectUserEntries &entries;
    static bool Read(void *context, uint64_t address,
                     std::span<uint32_t> words) {
      auto &self = *static_cast<Reader *>(context);
      // Preserve a recorded failed read even if an unrelated captured region
      // happens to cover it. Failed callbacks contribute no payload bytes.
      for (const auto &entry : self.entries.loads)
        if (!entry.rejected && entry.read_failed && entry.address == address &&
            entry.dword_count == words.size())
          return false;
      return CapturedMemory::Read(&self.memory, address, words);
    }
  } reader{memory, entries};
  const auto recreated = Shader::Diagnostics::CaptureDirectUserLoads(
      true, caller, controls.user_data, Reader::Read, &reader,
      controls.user_data_base);
  if (recreated.loads.size() != entries.loads.size() ||
      recreated.limit_reached != entries.limit_reached ||
      recreated.requested_bytes != entries.requested_bytes) {
    failure = "decoded direct-user origins differ from captured row "
              "count/attempt accounting";
    return false;
  }
  for (size_t i = 0; i < recreated.loads.size(); ++i) {
    const auto &native = recreated.loads[i];
    const auto &saved = entries.loads[i];
    if (native.pc != saved.pc ||
        native.destination_sgpr != saved.destination_sgpr ||
        native.user_sgpr != saved.user_sgpr ||
        native.dword_count != saved.dword_count ||
        native.offset != saved.offset || native.address != saved.address ||
        native.read_failed != saved.read_failed ||
        (!native.rejection.empty()) != saved.rejected ||
        (!saved.rejected && !saved.read_failed &&
         native.words.size() != saved.dword_count)) {
      failure = "direct-user snapshot does not match decoded instruction and "
                "captured user-pointer provenance";
      return false;
    }
  }
  return true;
}

inline void AddDirectUserPayloads(const DirectUserEntries &entries,
                                  const std::filesystem::path &folder,
                                  CapturedMemory &memory) {
  for (const auto &entry : entries.loads) {
    if (entry.rejected || entry.read_failed)
      continue;
    auto words =
        ReadWords(folder / entry.file, entry.dword_count * sizeof(uint32_t));
    Require(words.size() == entry.dword_count,
            "direct-user snapshot payload differs from its exact native DWORD "
            "count");
    memory.regions.push_back({entry.address, std::move(words)});
  }
}

inline CaptureManifest ReadManifest(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  Require(bool(file), "capture manifest is required; legacy data needs "
                      "explicit synthetic mode");
  const auto bytes = file.tellg();
  Require(bytes > 0 && bytes <= 2 * 1024 * 1024,
          "manifest exceeds bounded size");
  std::string text(static_cast<size_t>(bytes), '\0');
  file.seekg(0);
  file.read(text.data(), bytes);
  Require(bool(file), "manifest read was incomplete");
  CaptureManifest manifest;
  std::string failure;
  RequireParse(ParseCaptureManifest(text, manifest, failure), failure);
  return manifest;
}

inline std::filesystem::path CapturedFile(const std::filesystem::path &folder,
                                          const Fields &fields) {
  const auto found = fields.find("file");
  Require(found != fields.end() && found->second != "none" &&
              std::filesystem::path(found->second).filename().string() ==
                  found->second &&
              found->second.ends_with(".bin"),
          "manifest capture file is absent or not a local binary name");
  return folder / found->second;
}

inline std::vector<uint32_t> BracketWords(const Fields &fields,
                                          const char *name, size_t count) {
  const auto found = fields.find(name);
  Require(found != fields.end() && found->second.starts_with('[') &&
              found->second.ends_with(']'),
          "manifest lacks the exact descriptor/user DWORD list");
  auto text =
      std::string_view(found->second).substr(1u, found->second.size() - 2u);
  std::vector<uint32_t> result(count);
  for (size_t word = 0; word < count; ++word) {
    const auto comma = text.find(',');
    Require((word + 1u < count) == (comma != std::string_view::npos) &&
                ParseNumber(text.substr(0u, comma), result[word]),
            "malformed manifest DWORD list");
    if (comma != std::string_view::npos)
      text.remove_prefix(comma + 1u);
  }
  return result;
}

// Actual mode requires every recorded compile input. Legacy mode must be
// explicit. No guest code executes, and neither mode establishes live GPU
// correctness.
inline Fixture Load(const char *caller_path, const char *folder_path,
                    size_t limit, bool actual_inputs = true) {
  Fixture fixture;
  fixture.synthetic_inputs = !actual_inputs;
  const std::filesystem::path folder(folder_path);
  const auto manifest = ReadManifest(folder / "manifest.txt");
  std::string failure;
  const auto number = [&](const Fields &fields, const char *name,
                          auto &output) {
    RequireParse(Number(fields, name, output, failure), failure);
  };
  const auto flag = [&](const Fields &fields, const char *name, bool expected) {
    bool value = !expected;
    RequireParse(Boolean(fields, name, value, failure), failure);
    Require(value == expected,
            "capture flag prevents a complete bounded snapshot");
  };
  fixture.caller = ReadWords(caller_path);
  if (actual_inputs) {
    RequireParse(
        ParseComputeControls(manifest.global, fixture.controls, failure),
        failure);
    flag(manifest.global, "caller_file_written", true);
    const auto caller_file = manifest.global.find("caller_file");
    Require(caller_file != manifest.global.end() &&
                caller_file->second == "caller.bin",
            "actual manifest does not identify its checked caller binary");
    Require(ReadWords(folder / "caller.bin") == fixture.caller,
            "selected caller bytes differ from the caller captured with this "
            "input manifest");
  } else {
    fixture.controls.caller_address = 0x1450008000ull;
    fixture.controls.compute.wave_size = 64u;
    fixture.controls.compute.host_subgroup_size = 32u;
    std::fill_n(fixture.controls.compute.threads_num, 3u, 1u);
    fixture.controls.user_data.resize(108u);
    number(manifest.global, "caller_hash", fixture.controls.shader_hash);
    number(manifest.global, "caller_span_bytes", fixture.controls.caller_bytes);
    number(manifest.global, "decoded_instructions",
           fixture.controls.decoded_instructions);
  }
  Require(fixture.caller.size() * 4u == fixture.controls.caller_bytes,
          "caller byte count differs from this capture manifest");
  for (const auto *name : {"call_sites_truncated", "table_budget_exhausted",
                           "target_limit_reached", "target_budget_exhausted"})
    flag(manifest.global, name, false);
  for (const auto *name :
       {"zero_targets", "misaligned_targets", "outside_48bit_targets"}) {
    size_t count = 1u;
    number(manifest.global, name, count);
    Require(count == 0u,
            "captured table contains an excluded invalid code target");
  }
  CapturedMemory memory;
  std::set<uint64_t> table_targets;
  size_t table_bytes = 0;
  for (const auto &fields : manifest.tables) {
    flag(fields, "descriptor_read", true);
    flag(fields, "truncated", false);
    flag(fields, "read_failed", false);
    const auto rejection = fields.find("rejection");
    Require(rejection != fields.end() && rejection->second == "none",
            "captured call origin was unproven");
    uint64_t descriptor_address = 0, table_address = 0;
    size_t declared = 0, captured = 0;
    number(fields, "descriptor_address", descriptor_address);
    number(fields, "aligned_table_base", table_address);
    number(fields, "declared_table_bytes", declared);
    number(fields, "captured_bytes", captured);
    auto words = ReadWords(CapturedFile(folder, fields));
    Require(declared != 0u && declared == captured &&
                declared == words.size() * 4u && declared % 16u == 0u,
            "captured table is partial or differs from the declared native "
            "record span");
    table_bytes += declared;
    fixture.total_records += words.size() / 4u;
    for (size_t first = 0; first < words.size(); first += 4u)
      table_targets.insert(uint64_t{words[first]} |
                           (uint64_t{words[first + 1u]} << 32u));
    memory.regions.push_back(
        {descriptor_address, BracketWords(fields, "descriptor_words", 4u)});
    memory.regions.push_back({table_address, std::move(words)});
    if (!actual_inputs) {
      uint32_t reg = 0;
      number(fields, "user_sgpr_pair", reg);
      Require(reg < 107u,
              "synthetic origin pair lies outside scalar registers");
      const auto user = BracketWords(fields, "user_words", 2u);
      fixture.controls.user_data[reg] = user[0];
      fixture.controls.user_data[reg + 1u] = user[1];
    }
  }
  size_t reserved_tables = 0, requested_tables = 0;
  number(manifest.global, "table_bytes_reserved", reserved_tables);
  number(manifest.global, "table_read_bytes_requested", requested_tables);
  Require(table_bytes == reserved_tables && table_bytes == requested_tables &&
              table_bytes <= Shader::Diagnostics::MaxTableBytes,
          "captured table accounting does not match complete bounded reads");
  std::set<uint64_t> prefix_targets;
  for (const auto &fields : manifest.targets) {
    uint64_t address = 0;
    size_t bytes = 0;
    number(fields, "raw_address", address);
    number(fields, "captured_bytes", bytes);
    flag(fields, "read_failed", false);
    Require(
        table_targets.contains(address) &&
            prefix_targets.insert(address).second,
        "candidate prefix is duplicated or absent from this captured table");
    const auto path = CapturedFile(folder, fields);
    const auto name = path.filename().string();
    const auto start = name.find_last_of('_') + 1u;
    uint64_t named_address = 0;
    Require(name.starts_with("target_") &&
                ParseNumber("0x" + name.substr(start, name.size() - start - 4u),
                            named_address) &&
                named_address == address,
            "prefix filename and manifest target address disagree");
    auto words = ReadWords(path);
    Require(bytes == Shader::Diagnostics::MaxTargetBytes &&
                bytes == words.size() * 4u,
            "candidate prefix is incomplete");
    memory.regions.push_back({address, std::move(words)});
  }
  Require(prefix_targets == table_targets,
          "one or more captured native table targets lack their prefix");
  fixture.total_functions = prefix_targets.size();
  Require(limit > 0u && limit <= fixture.total_functions,
          "candidate subset exceeds captured library");
  size_t reserved_targets = 0, requested_targets = 0;
  number(manifest.global, "target_bytes_reserved", reserved_targets);
  number(manifest.global, "target_read_bytes_requested", requested_targets);
  Require(reserved_targets == fixture.total_functions *
                                  Shader::Diagnostics::MaxTargetBytes &&
              requested_targets == reserved_targets &&
              reserved_targets <= Shader::Diagnostics::MaxAggregateTargetBytes,
          "captured prefix accounting does not match complete bounded reads");
  std::string direct_failure;
  RequireParse(
      ParseDirectUserEntries(manifest, fixture.direct_user, direct_failure),
      direct_failure);
  AddDirectUserPayloads(fixture.direct_user, folder, memory);
  memory.Merge();
  Shader::Decoder::Program caller;
  Shader::Decoder::DecodeProgram(fixture.caller, caller);
  Require(caller.instructions.size() == fixture.controls.decoded_instructions,
          "decoded caller count differs from this capture manifest");
  RequireParse(VerifyDirectUserOrigins(fixture.direct_user, caller,
                                       fixture.controls, memory,
                                       direct_failure),
               direct_failure);
  auto loaded = Shader::LoadExternalLibrary(
      caller, fixture.controls.caller_address, fixture.controls.user_data,
      fixture.controls.user_data_base, CapturedMemory::Read, &memory);
  if (!loaded.failure.empty())
    std::fprintf(stderr, "CapturedExternalFixture loader: %s\n",
                 loaded.failure.c_str());
  Require(loaded.has_calls && loaded.plan.complete &&
              loaded.plan.functions.size() == fixture.total_functions &&
              loaded.plan.call_sites.size() == manifest.tables.size(),
          "production guarded loader rejected captured inputs");
  fixture.library = std::move(loaded.plan);
  for (size_t call = 0; call < manifest.tables.size(); ++call) {
    uint32_t pc = 0, target = 0, return_pair = 0, record_pc = 0, user_reg = 0;
    const auto &fields = manifest.tables[call];
    const auto &site = fixture.library.call_sites[call];
    number(fields, "pc", pc);
    number(fields, "target_encoded_pair", target);
    number(fields, "return_encoded_pair", return_pair);
    number(fields, "record_load_pc", record_pc);
    number(fields, "user_sgpr_pair", user_reg);
    Require(site.caller_pc == pc && site.target_sgpr == target &&
                site.return_sgpr == return_pair &&
                site.record_load_pc == record_pc &&
                site.descriptor_user_sgpr == user_reg &&
                site.record_sgpr != UINT32_MAX &&
                site.auxiliary_sgpr != UINT32_MAX,
            "decoded register/context provenance disagrees with the captured "
            "call origin");
  }
  // Partial offline programs preserve original sparse ordinals and proved
  // registers.
  fixture.library.functions.resize(limit);
  for (auto &site : fixture.library.call_sites) {
    std::erase_if(site.records, [limit](const auto &record) {
      return record.function_id >= limit;
    });
    std::erase_if(site.context_records, [limit](const auto &record) {
      return record.function_id >= limit;
    });
    std::set<uint64_t> selected;
    for (const auto &record : site.records)
      selected.insert(record.function_address);
    site.candidate_addresses.assign(selected.begin(), selected.end());
  }
  fixture.memory = std::move(memory);
  return fixture;
}
} // namespace CapturedExternalTest
