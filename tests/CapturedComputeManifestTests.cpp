#include "CapturedExternalLibraryFixture.h"

#include <cstring>
#include <iostream>

namespace Common {
int DbgExitHandler(const char *, int, std::string_view) { std::abort(); }
int DbgExitIfHandler(const char *, const char *, int) { std::abort(); }
int DbgNotImplementedHandler(const char *, const char *, int) { std::abort(); }
void DbgExit(int status) { std::exit(status); }
} // namespace Common

namespace {
using namespace CapturedExternalTest;

Fields AuthoredControls() {
  Fields fields;
  std::string failure;
  RequireParse(
      AppendFields(
          "stage=cs caller_hash=0x123456789abc caller_base=0x1200008000 "
          "caller_span_bytes=32 decoded_instructions=6 "
          "wave_size=64 user_data_base=8 user_data_count=2 threads_num=64,2,1 "
          "host_subgroup_size=32 "
          "lds_size_dwords=17 scratch_size_dwords=23 "
          "dispatch_threads_num=0,0,0 workgroup_counts=31,5,1 "
          "group_id=true,false,true thread_ids_num=2 workgroup_register=10 "
          "tg_size_en=true "
          "dispatch_thread_dimensions=false lds_storage=true float_mode=0x7f "
          "user_sgpr[8]=0xabcdef12 user_sgpr[9]=0x00000013",
          fields, failure),
      failure);
  return fields;
}

void TestCompleteControls() {
  ComputeControls controls;
  std::string failure;
  RequireParse(ParseComputeControls(AuthoredControls(), controls, failure),
               failure);
  const auto &input = controls.compute;
  Require(
      controls.caller_address == 0x1200008000ull &&
          controls.shader_hash == 0x123456789abcull &&
          controls.user_data_base == 8u &&
          controls.user_data == std::vector<uint32_t>{0xabcdef12u, 0x13u} &&
          controls.caller_bytes == 32u && controls.decoded_instructions == 6u &&
          input.wave_size == 64u && input.host_subgroup_size == 32u &&
          input.threads_num[0] == 64u && input.threads_num[1] == 2u &&
          input.threads_num[2] == 1u && input.workgroup_counts[0] == 31u &&
          input.workgroup_counts[1] == 5u && input.workgroup_counts[2] == 1u &&
          input.dispatch_threads_num[0] == 0u && input.group_id[0] &&
          !input.group_id[1] && input.group_id[2] &&
          input.thread_ids_num == 2 && input.workgroup_register == 10 &&
          input.tg_size_en && !input.dispatch_thread_dimensions &&
          input.lds_storage && input.float_mode == 0x7fu &&
          input.lds_size_dwords == 17u && input.scratch_size_dwords == 23u,
      "complete manifest controls did not preserve every captured field");
}

void TestEveryFieldRequired() {
  const auto complete = AuthoredControls();
  for (const auto &[name, value] : complete) {
    auto fields = complete;
    fields.erase(name);
    ComputeControls controls;
    std::string failure;
    Require(!ParseComputeControls(fields, controls, failure) &&
                !failure.empty(),
            "missing control or SGPR word silently received a default");
  }
}

void TestMalformedAndOutOfRange() {
  const auto complete = AuthoredControls();
  for (const auto &[name, value] :
       std::vector<std::pair<std::string, std::string>>{
           {"stage", "vs"},
           {"caller_base", "0x1000000000000"},
           {"caller_base", "0x1200008001"},
           {"caller_span_bytes", "33"},
           {"wave_size", "16"},
           {"host_subgroup_size", "31"},
           {"threads_num", "64,1"},
           {"threads_num", "64,1,1,1"},
           {"threads_num", "64,0,1"},
           {"workgroup_counts", "31,5,x"},
           {"group_id", "true,2,false"},
           {"tg_size_en", "1"},
           {"float_mode", "0x100"},
           {"user_data_base", "107"},
           {"user_data_count", "1"},
           {"user_sgpr[8]", "0x100000000"},
           {"thread_ids_num", "4"},
           {"workgroup_register", "108"}}) {
    auto fields = complete;
    fields[name] = value;
    ComputeControls controls;
    std::string failure;
    Require(!ParseComputeControls(fields, controls, failure) &&
                !failure.empty(),
            "malformed or out-of-range captured control was accepted");
  }
  auto alias = complete;
  alias.erase("user_sgpr[9]");
  alias["user_sgpr[08]"] = "0x1";
  ComputeControls controls;
  std::string failure;
  Require(!ParseComputeControls(alias, controls, failure),
          "two numeric spellings hid a missing SGPR");
}

void TestManifestRowsAndDuplicates() {
  constexpr std::string_view text =
      "stage=cs\nlimits: call_sites=64 targets=2048\n"
      "call[0]: pc=0x4 raw=0xbe8e210e\n  record_load_pc=0x0 "
      "descriptor_read=true\n"
      "target[0]: raw_address=0x1300000000 captured_bytes=65536 "
      "read_failed=false\n";
  CaptureManifest manifest;
  std::string failure;
  RequireParse(ParseCaptureManifest(text, manifest, failure), failure);
  Require(manifest.tables.size() == 1u && manifest.targets.size() == 1u &&
              manifest.tables[0].at("record_load_pc") == "0x0" &&
              manifest.global.at("stage") == "cs" &&
              !manifest.global.contains("targets"),
          "manifest sections were conflated");
  for (const auto invalid :
       {"stage=cs stage=cs\ncall[0]: pc=0x4\ntarget[0]: raw_address=0x1\n",
        "call[1]: pc=0x4\ntarget[0]: raw_address=0x1\n",
        "call[0]: pc=0x4\ntarget[1]: raw_address=0x1\n",
        "call[0]: pc=0x4\ntarget[0]: file=target_00.bin (write failed)\n"}) {
    Require(!ParseCaptureManifest(invalid, manifest, failure),
            "invalid or incomplete manifest rows were accepted");
  }
}

void TestCapturedMemoryBounds() {
  CapturedMemory memory{
      {{0x1000u, {1u, 2u, 3u}}, {0x1008u, {3u, 4u}}, {0x1010u, {5u}}}};
  memory.Merge();
  std::array<uint32_t, 4> words{};
  Require(memory.regions.size() == 1u &&
              CapturedMemory::Read(&memory, 0x1004u, words) &&
              words == std::array<uint32_t, 4>{2u, 3u, 4u, 5u},
          "consistent captured byte union was not retained");
  Require(!CapturedMemory::Read(&memory, 0x1008u, words) &&
              !CapturedMemory::Read(&memory, 0x1001u, words) &&
              !CapturedMemory::Read(&memory, 0xffcu, words),
          "offline reader accessed uncaptured or unaligned bytes");
}

CaptureManifest AuthoredDirectManifest() {
  constexpr std::string_view text =
      "stage=cs direct_user_loads=2 direct_user_load_limit=256 "
      "direct_user_load_max_dwords=16 direct_user_load_limit_reached=false "
      "direct_user_load_requested_bytes=72\n"
      "direct_user_load[0]: pc=0x0 destination_sgpr=68 user_sgpr=0 dword_count=16 "
      "offset=408 address=0x1198 read_failed=false rejection=none file=direct_user_000_00000000.bin\n"
      "direct_user_load[1]: pc=0x8 destination_sgpr=16 user_sgpr=0 dword_count=2 "
      "offset=336 address=0x1150 read_failed=false rejection=none file=direct_user_001_00000008.bin\n"
      "call[0]: pc=0xc\ntarget[0]: raw_address=0x1300000000\n";
  CaptureManifest manifest;
  std::string failure;
  RequireParse(ParseCaptureManifest(text, manifest, failure), failure);
  Require(manifest.direct_user_loads.size() == 2u &&
              !manifest.global.contains("destination_sgpr") &&
              manifest.global.at("direct_user_loads") == "2",
          "direct-user row fields escaped into global/table fields");
  return manifest;
}

void TestDirectUserMetadataAndCaps() {
  const auto complete = AuthoredDirectManifest();
  DirectUserEntries entries;
  std::string failure;
  RequireParse(ParseDirectUserEntries(complete, entries, failure), failure);
  Require(entries.present && !entries.limit_reached && entries.loads.size() == 2u &&
              entries.requested_bytes == 72u && entries.loads[0].address == 0x1198u,
          "direct-user snapshots lost exact origin/outcome/byte fields");
  for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>>{
           {"direct_user_loads", "1"}, {"direct_user_load_limit", "257"},
           {"direct_user_load_max_dwords", "17"}, {"direct_user_load_limit_reached", "1"},
           {"direct_user_load_requested_bytes", "71"}, {"direct_user_load_requested_bytes", "16385"}}) {
    auto bad = complete;
    bad.global[key] = value;
    Require(!ParseDirectUserEntries(bad, entries, failure), "invalid direct-user bounds/accounting accepted");
  }
  for (const auto &key : {"pc", "destination_sgpr", "user_sgpr", "dword_count", "offset", "address",
                         "read_failed", "rejection", "file"}) {
    auto bad = complete;
    bad.direct_user_loads[0].erase(key);
    Require(!ParseDirectUserEntries(bad, entries, failure), "missing direct-user field was accepted");
  }
  for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>>{
           {"pc", "0x1"}, {"dword_count", "3"}, {"dword_count", "32"},
           {"destination_sgpr", "100"}, {"user_sgpr", "107"},
           {"address", "0x1199"}, {"address", "0xfffffffffffc"},
           {"address", "0x1000000000000"}, {"read_failed", "true"},
           {"rejection", "modified"}, {"file", "none"},
           {"file", "../direct_user_000_00000000.bin"}, {"file", "direct_user_001_00000000.bin"}}) {
    auto bad = complete;
    bad.direct_user_loads[0][key] = value;
    Require(!ParseDirectUserEntries(bad, entries, failure), "malformed direct-user span/outcome/filename accepted");
  }
  auto failed = complete;
  failed.direct_user_loads[1]["read_failed"] = "true";
  failed.direct_user_loads[1]["file"] = "none";
  RequireParse(ParseDirectUserEntries(failed, entries, failure), failure);
  Require(entries.requested_bytes == 72u && entries.loads[1].read_failed,
          "failed read replenished the requested-byte budget");
  auto rejected = failed;
  rejected.direct_user_loads[1]["read_failed"] = "false";
  rejected.direct_user_loads[1]["rejection"] = "base";
  rejected.direct_user_loads[1]["address"] = "0";
  rejected.global["direct_user_load_requested_bytes"] = "64";
  RequireParse(ParseDirectUserEntries(rejected, entries, failure), failure);
  Require(entries.loads[1].rejected && entries.requested_bytes == 64u,
          "rejected origin was mapped or charged as a read");
  auto capped = complete;
  const auto first_row = capped.direct_user_loads[0];
  capped.direct_user_loads.assign(256u, first_row);
  for (size_t i = 0; i < capped.direct_user_loads.size(); ++i) {
    char filename[64];
    std::snprintf(filename, sizeof(filename), "direct_user_%03zu_00000000.bin", i);
    capped.direct_user_loads[i]["file"] = filename;
  }
  capped.global["direct_user_loads"] = "256";
  capped.global["direct_user_load_requested_bytes"] = "16384";
  capped.global["direct_user_load_limit_reached"] = "true";
  RequireParse(ParseDirectUserEntries(capped, entries, failure), failure);
  Require(entries.limit_reached && entries.loads.size() == 256u,
          "exact capture cap lost its explicit partial state");
  capped.direct_user_loads.push_back(capped.direct_user_loads.front());
  capped.global["direct_user_loads"] = "257";
  Require(!ParseDirectUserEntries(capped, entries, failure), "257 direct-user attempts accepted");
  CaptureManifest old;
  RequireParse(ParseDirectUserEntries(old, entries, failure), failure);
  Require(!entries.present && entries.loads.empty(), "legacy absence was treated as captured payload proof");
  old.direct_user_loads = complete.direct_user_loads;
  Require(!ParseDirectUserEntries(old, entries, failure), "orphan payload rows accepted");
  CaptureManifest parsed;
  Require(!ParseCaptureManifest("direct_user_load[1]: pc=0\ncall[0]: pc=0\ntarget[0]: raw_address=0\n", parsed, failure),
          "noncontiguous direct-user row index accepted");
  Require(!ParseCaptureManifest("direct_user_load[0]: pc=0 pc=0\ncall[0]: pc=0\ntarget[0]: raw_address=0\n", parsed, failure),
          "duplicate direct-user machine field accepted");
}

void TestDirectUserNativeOriginVerification() {
  auto manifest = AuthoredDirectManifest();
  DirectUserEntries entries;
  std::string failure;
  RequireParse(ParseDirectUserEntries(manifest, entries, failure), failure);
  Shader::Decoder::Program caller;
  const std::vector<uint32_t> code{0xf4101100u,0xfa000198u,0xf4040400u,0xfa000150u,0xbf810000u};
  Shader::Decoder::DecodeProgram(code, caller);
  ComputeControls controls;
  controls.user_data = {0x1000u,0u};
  CapturedMemory memory{{{0x1198u,std::vector<uint32_t>(16u,0xabcdefu)},{0x1150u,{7u,8u}}}};
  memory.Merge();
  RequireParse(VerifyDirectUserOrigins(entries,caller,controls,memory,failure),failure);
  for (size_t mutation = 0; mutation < 8u; ++mutation) {
    auto changed = entries;
    auto &row = changed.loads[0];
    switch (mutation) {
      case 0: row.pc=4u; break;
      case 1: row.destination_sgpr=64u; break;
      case 2: row.user_sgpr=2u; break;
      case 3: row.dword_count=8u; break;
      case 4: row.offset=404; break;
      case 5: row.address=0x1194u; break;
      case 6: row.rejected=true; break;
      case 7: changed.requested_bytes=64u; break;
    }
    Require(!VerifyDirectUserOrigins(changed,caller,controls,memory,failure),
            "captured metadata changed decoded instruction or pointer provenance");
  }
  auto relocated_controls = controls;
  relocated_controls.user_data[0] += 4u;
  Require(!VerifyDirectUserOrigins(entries,caller,relocated_controls,memory,failure),
          "changed captured pointer reused old snapshot addresses");
  CapturedMemory missing{{{0x1198u,std::vector<uint32_t>(16u,1u)}}};
  Require(!VerifyDirectUserOrigins(entries,caller,controls,missing,failure),
          "uncaptured direct payload was replaced by zeros or trusted metadata");
  manifest.direct_user_loads[1]["read_failed"]="true";
  manifest.direct_user_loads[1]["file"]="none";
  RequireParse(ParseDirectUserEntries(manifest,entries,failure),failure);
  RequireParse(VerifyDirectUserOrigins(entries,caller,controls,memory,failure),failure);
  Require(entries.loads[1].read_failed,
          "failed capture was silently promoted by unrelated overlapping saved bytes");
}
} // namespace

int main(int argc, char **argv) {
  TestCompleteControls();
  TestEveryFieldRequired();
  TestMalformedAndOutOfRange();
  TestManifestRowsAndDuplicates();
  TestCapturedMemoryBounds();
  TestDirectUserMetadataAndCaps();
  TestDirectUserNativeOriginVerification();
  std::puts("CapturedComputeManifestTests: all seven groups passed (strict "
            "captured inputs; no GPU/guest execution)");
  if (argc == 5 && std::strcmp(argv[1], "--inspect") == 0) {
    size_t limit = 0;
    Require(ParseNumber(std::string_view(argv[4]), limit),
            "inspection limit is not a positive number");
    const auto fixture = Load(argv[2], argv[3], limit);
    const auto &input = fixture.controls.compute;
    std::printf(
        "CapturedInputs: caller=0x%016llx wave=%u host_subgroup=%u "
        "threads=%u,%u,%u workgroups=%u,%u,%u user_base=%u user_words=%zu "
        "functions=%zu total_functions=%zu records=%zu total_records=%zu "
        "subset=%s\n",
        static_cast<unsigned long long>(fixture.library.caller_address),
        input.wave_size, input.host_subgroup_size, input.threads_num[0],
        input.threads_num[1], input.threads_num[2], input.workgroup_counts[0],
        input.workgroup_counts[1], input.workgroup_counts[2],
        fixture.controls.user_data_base, fixture.controls.user_data.size(),
        fixture.library.functions.size(), fixture.total_functions,
        fixture.library.call_sites[0].records.size(), fixture.total_records,
        limit == fixture.total_functions ? "false" : "true");
    for (const auto &site : fixture.library.call_sites)
      std::printf(
          "CapturedInputs: call_pc=0x%x record_load_pc=0x%x record_sgpr=%u "
          "auxiliary_sgpr=%u target_sgpr=%u return_sgpr=%u\n",
          site.caller_pc, site.record_load_pc, site.record_sgpr,
          site.auxiliary_sgpr, site.target_sgpr, site.return_sgpr);
    std::puts("CapturedInputs: production loader reconstructed captured bytes "
              "only; no translation, GPU, or live shader proof");
  } else
    Require(argc == 1, "usage: CapturedComputeManifestTests [--inspect "
                       "caller.bin capture-folder function-limit]");
}
