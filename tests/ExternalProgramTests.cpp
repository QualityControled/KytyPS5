#include "graphics/shader/recompiler/ExternalProgram.h"
#include "graphics/shader/recompiler/ShaderCallDiagnostics.h"
#include "CapturedExternalLibraryFixture.h"
#include "common/assert.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

// Unexpected decoder assertions terminate the test process rather than becoming
// accepted linker failures. Native code is decoded and graphed, never executed.
namespace Common {
int DbgExitHandler(const char*, int, std::string_view text) {
	std::fprintf(stderr, "unexpected decoder fatal: %.*s\n", int(text.size()), text.data()); std::exit(2);
}
int DbgExitIfHandler(const char* text, const char*, int) {
	std::fprintf(stderr, "unexpected decoder assertion: %s\n", text); std::exit(2);
}
int DbgNotImplementedHandler(const char* text, const char*, int) {
	std::fprintf(stderr, "unexpected decoder unsupported assertion: %s\n", text); std::exit(2);
}
void DbgExit(int status) { std::exit(status); }
} // namespace Common

namespace {
namespace Shader = Libs::Graphics::ShaderRecompiler;
namespace Decoder = Shader::Decoder;
namespace CFG = Shader::CFG;
size_t PeakWorkingSetBytes();

void Check(bool value, const char* text) {
	if (!value) { std::fprintf(stderr, "ExternalProgramTests: failed: %s\n", text); std::exit(1); }
}

struct Fixture {
	static constexpr uint64_t Caller = 0x1450008000ull;
	static constexpr uint64_t A = 0x1300100000ull;
	static constexpr uint64_t B = 0x1400100000ull;
	std::vector<uint32_t> caller_code {0xbe8e210eu, 0xbf810000u};
	Decoder::Program caller;
	Shader::ExternalLibraryPlan library;
	Fixture() {
		DecodeCaller(); library.complete = true; library.caller_address = Caller;
		library.functions.push_back({0u, A, {0x7e0e0281u, 0xbe80200eu}});
		library.functions.push_back({1u, B, {0x7e0e0282u, 0xbe80200eu}});
		Shader::ExternalCallSite site;
		site.caller_pc = 0; site.target_sgpr = 14; site.return_sgpr = 14;
		site.context_domain = 3; site.record_load_pc = 0x100u; site.auxiliary_sgpr = 16;
		site.candidate_addresses = {A, B}; library.call_sites.push_back(site);
	}
	void DecodeCaller() { Decoder::DecodeProgram(caller_code, caller); }
	Shader::LinkedExternalProgram Link() { return Shader::LinkExternalProgram(caller, library); }
	void OneFunction(std::vector<uint32_t> code) {
		library.functions.resize(1u); library.functions[0].code_prefix = std::move(code);
		library.call_sites[0].candidate_addresses = {A};
	}
};

void Rejected(Fixture& fixture, std::string_view reason) {
	const auto linked = fixture.Link();
	Check(!linked.success && !linked.failure.empty() && linked.failure.find(reason) != std::string::npos,
	      "invalid library lacked a precise nonfatal linker rejection");
}

// Pair writers use legal even-aligned SGPR pairs. Preserve and restore the
// low link word so only a missed high-word clobber can cause false admission.
void TestSavedLinkScalarWriteWidths() {
    const auto sop2=[](uint32_t op,uint32_t dst){return 0x80000000u|(op<<23u)|(dst<<16u)|(0x81u<<8u);};
    const auto sop1=[](uint32_t op,uint32_t dst){return 0xbe800000u|(dst<<16u)|(op<<8u);};
    struct Case { const char* name;std::vector<uint32_t> words;Decoder::Opcode op;uint32_t dst;bool restore_low;bool returns; };
    const std::vector<Case> cases {
      {"CMP pair14 clobber with restored low",{0xd401000eu,0x00020501u},Decoder::Opcode::V_CMP_LT_F32,14,true,false},
      {"CMP direct pair14 clobber",{0xd401000eu,0x00020501u},Decoder::Opcode::V_CMP_LT_F32,14,false,false},
      {"CMP disjoint pair12",{0xd401000cu,0x00020501u},Decoder::Opcode::V_CMP_LT_F32,12,false,true},
      {"I64 pair14 with restored low",{sop2(0x23,14)},Decoder::Opcode::S_ASHR_I64,14,true,false},
      {"I64 disjoint pair12",{sop2(0x23,12)},Decoder::Opcode::S_ASHR_I64,12,false,true},
      {"U64 pair14 with restored low",{sop2(0x29,14)},Decoder::Opcode::S_BFE_U64,14,true,false},
      {"U64 disjoint pair12",{sop2(0x29,12)},Decoder::Opcode::S_BFE_U64,12,false,true},
      {"B64 pair14 with restored low",{sop1(0x08,14)},Decoder::Opcode::S_NOT_B64,14,true,false},
      {"B64 disjoint pair12",{sop1(0x08,12)},Decoder::Opcode::S_NOT_B64,12,false,true},
      {"BITREPLICATE mixed output64 restored low",{sop1(0x3b,14)},Decoder::Opcode::S_BITREPLICATE_B64_B32,14,true,false},
      {"BITREPLICATE disjoint pair12",{sop1(0x3b,12)},Decoder::Opcode::S_BITREPLICATE_B64_B32,12,false,true},
      {"BCNT one-word result13",{sop1(0x10,13)},Decoder::Opcode::S_BCNT1_I32_B64,13,false,true},
      {"FF1 one-word result13",{sop1(0x14,13)},Decoder::Opcode::S_FF1_I32_B64,13,false,true},
      {"FLBIT one-word result13",{sop1(0x16,13)},Decoder::Opcode::S_FLBIT_I32_B64,13,false,true},
      {"BCNT direct link14 clobber",{sop1(0x10,14)},Decoder::Opcode::S_BCNT1_I32_B64,14,false,false},
      {"secondary carry pair14 restored low",{0xd70f0e01u,0x00020501u},Decoder::Opcode::V_ADD_I32,14,true,false},
    };
    size_t failures=0;
    for(const auto& test:cases) {
      std::array<uint32_t,32> padded{};std::copy(test.words.begin(),test.words.end(),padded.begin());
      Decoder::Instruction inst{};Decoder::DecodeInstruction(padded,0,inst);
      const auto& dst=test.op==Decoder::Opcode::V_ADD_I32?inst.dst2:inst.dst;
      Check(inst.opcode==test.op && inst.word_count==test.words.size() &&
            dst.kind==Decoder::OperandKind::Sgpr && dst.reg==test.dst,
            "scalar width fixture did not decode its intended legal native writer");
      Fixture fixture;std::vector<uint32_t> body;
      if(test.restore_low) body.push_back(0xbe94030eu); // s20=s14 (one DWORD)
      body.insert(body.end(),test.words.begin(),test.words.end());
      if(test.restore_low) body.push_back(0xbe8e0314u); // Restore only s14 from s20.
      body.push_back(0xbe80200eu);fixture.OneFunction(std::move(body));
      const auto linked=fixture.Link();
      const bool pass=test.returns?linked.success:!linked.success&&linked.failure.find("saved-link return")!=std::string::npos;
      std::printf("SavedLinkWidthLegal: %s expected_return=%u link_success=%u %s reason=%s\n",
                  test.name,unsigned(test.returns),unsigned(linked.success),pass?"PASS":"FAIL",linked.failure.c_str());
      failures+=!pass;
    }
    for(const auto& body:std::vector<std::vector<uint32_t>>{
          {0xbe92040eu,0xbe802012u}, // copy14:15 to18:19 then return18
          {0xbe8e040eu,0xbe80200eu}, // legal same-pair copy snapshots old14:15
          {0xbe92040eu,0xbe8e0380u,0xbe802012u}}) {
      Fixture f;f.OneFunction(body);Check(f.Link().success,"valid even-pair copied/self-copy return rejected");
    }
    std::printf("SavedLinkWidthLegal: 16 decoded cases +3 even-pair copied/self-copy controls; failures=%zu\n",failures);
    Check(failures==0,"native scalar destination extent corrupted saved-link provenance");
}

void TestCapturedBodySavedLinkWidth(const char* path) {
    // Body compatibility only: an authored aliased call/link pair, no claimed
    // runtime-held-plan identity, resource materialization, emission or execution.
    Fixture fixture;
    fixture.OneFunction(CapturedExternalTest::ReadWords(path));
    const auto linked=fixture.Link();
    if (!linked.success) std::fprintf(stderr,"captured body rejection: %s\n",linked.failure.c_str());
    Check(linked.success && linked.entries.size()==1u,"captured body no longer has its valid full saved-link return");
    std::printf("CapturedSavedLinkBody: appended_instructions=%zu entries=%zu transfers=%zu; authored caller, native body only, no resources/GPU\n",
                linked.program.instructions.size()-fixture.caller.instructions.size(),linked.entries.size(),linked.transfers.size());
}

void TestAliasedFullAddressDispatcherAndCFG() {
	Fixture fixture; const auto linked = fixture.Link();
	Check(linked.success && linked.failure.empty() && linked.entries.size() == 2u,
	      "valid two-function aliased call did not link");
	Check(linked.program.code.data() == linked.code.data() && linked.program.code.size() == linked.code.size(),
	      "combined decoded program lost its owned native storage");
	const auto call = std::find_if(linked.transfers.begin(), linked.transfers.end(), [](const auto& t) { return t.call; });
	Check(call != linked.transfers.end() && call->target_sgpr == 14u && call->return_sgpr == 14u &&
	          call->guest_pc == Fixture::Caller && call->link_address == Fixture::Caller + 4u &&
	          call->guest_addresses == std::vector<uint64_t>{Fixture::A, Fixture::B} &&
	          call->target_pcs.size() == 2u && call->target_pcs[0] != call->target_pcs[1],
	      "full high-word target identity or PC+4 link was narrowed or aliased");
	Check(std::all_of(linked.entries.begin(), linked.entries.end(), [](const auto& entry) {
		return entry.record_load_pc == 0x100u && entry.auxiliary_sgpr == 16u && entry.domain_id == 3u;
	}), "external entry context lost its originating call domain");
	const auto graph = CFG::BuildGraph(linked.program, linked.transfers);
	if (graph.unsupported) std::fprintf(stderr, "external CFG failure: %s (%s)\n",
	    graph.unsupported_reason.c_str(), CFG::FailureKindToString(graph.failure_kind).c_str());
	Check(!graph.unsupported && graph.irreducible &&
	          graph.failure_kind == CFG::FailureKind::IrreducibleControlFlow,
	      "linked U64 call/return transfers were rejected by native CFG");
	const auto block = graph.FindBlockByPc(0u);
	Check(block != nullptr && block->terminator.external_call && block->terminator.external_transfer &&
	          block->terminator.indirect_guest_addresses == call->guest_addresses &&
	          block->terminator.external_link_address == Fixture::Caller + 4u,
	      "CFG conflated guest addresses with dense labels or dropped saved continuation");
}

struct ProbeFixture {
  std::vector<uint32_t> code{0xf4280382u, 0x10000000u, 0xbe8e210eu,
                             0xbf810000u};
  Decoder::Program caller;
  Shader::ExternalLibraryPlan library;
  ProbeFixture() {
    Decoder::DecodeProgram(code, caller);
    library.complete = true;
    library.caller_address = Fixture::Caller;
    Shader::ExternalCallSite site;
    site.caller_pc = 8u;
    site.record_load_pc = 0u;
    site.record_sgpr = 14u;
    site.target_sgpr = site.return_sgpr = 14u;
    site.auxiliary_sgpr = 16u;
    site.context_domain = 11u;
    site.candidate_addresses = {Fixture::A, Fixture::B};
    site.records = {{0u, 0u, Fixture::A, 0x1200200300ull},
                    {7u, 1u, Fixture::B, 0x1400200300ull}};
    library.call_sites.push_back(site);
  }
  void Decode() { Decoder::DecodeProgram(code, caller); }
  Shader::LinkedExternalProgram Probe() {
    return Shader::BuildExternalCallProbe(caller, library);
  }
};

void TestProbePreservesCallBoundaryAndRegisters() {
  for (const uint32_t destination : {14u, 20u}) {
    ProbeFixture fixture;
    fixture.code[2] = 0xbe80210eu | (destination << 16u);
    fixture.Decode();
    fixture.library.call_sites[0].return_sgpr = destination;
    const auto probe = fixture.Probe();
    Check(probe.success && probe.failure.empty() && probe.entries.empty() &&
              probe.transfers.size() == 1u && probe.code == fixture.code &&
              probe.program.instructions.size() ==
                  fixture.caller.instructions.size(),
          "probe rewrote the native call or linked/examined an external "
          "function body");
    const auto &transfer = probe.transfers[0];
    Check(transfer.probe && transfer.call && transfer.target_sgpr == 14u &&
              transfer.return_sgpr == destination &&
              transfer.guest_pc == Fixture::Caller + 8u &&
              transfer.link_address == 0u && transfer.record_load_pc == 0u &&
              transfer.auxiliary_sgpr == 16u &&
              transfer.context_domain == 11u &&
              transfer.guest_addresses.empty() && transfer.target_pcs.empty(),
          "probe dropped record/auxiliary provenance or manufactured an "
          "ordinary saved return link");
    const auto graph = CFG::BuildGraph(probe.program, probe.transfers);
    const auto *block = graph.FindBlockByPc(0u);
    Check(!graph.unsupported && block != nullptr &&
              block->terminator.kind == CFG::TerminatorKind::Return &&
              block->terminator.external_call_probe &&
              block->terminator.external_transfer &&
              block->successors.empty() &&
              block->terminator.external_guest_pc == Fixture::Caller + 8u &&
              block->terminator.external_record_load_pc == 0u &&
              block->terminator.external_auxiliary_sgpr == 16u &&
              block->terminator.external_context_domain == 11u,
          "probe selected call retained an executable continuation or lost its "
          "exact metadata");
  }
}

void TestProbeRejectsMissingProvenance() {
  const auto reject = [](ProbeFixture &fixture, std::string_view reason) {
    const auto probe = fixture.Probe();
    Check(!probe.success && !probe.failure.empty() &&
              probe.failure.find(reason) != std::string::npos,
          "invalid probe provenance did not receive an explicit nonfatal "
          "rejection");
  };
  ProbeFixture partial;
  partial.library.complete = false;
  reject(partial, "complete");
  ProbeFixture unmatched;
  unmatched.library.call_sites[0].target_sgpr = 12u;
  reject(unmatched, "register pairs");
  ProbeFixture load;
  load.library.call_sites[0].record_load_pc = 4u;
  reject(load, "record load");
  ProbeFixture destination;
  destination.library.call_sites[0].record_sgpr = 12u;
  reject(destination, "record load");
  ProbeFixture auxiliary;
  auxiliary.library.call_sites[0].auxiliary_sgpr = UINT32_MAX;
  reject(auxiliary, "auxiliary pair");
  ProbeFixture domain;
  domain.library.call_sites[0].context_domain = UINT32_MAX;
  reject(domain, "domain");
  ProbeFixture records;
  records.library.call_sites[0].records.clear();
  reject(records, "domain");
  ProbeFixture duplicate;
  duplicate.library.call_sites.push_back(duplicate.library.call_sites[0]);
  reject(duplicate, "duplicate");
}

void TestProbeRejectsBarriersAndUnplannedCalls() {
  ProbeFixture barrier;
  barrier.code.insert(barrier.code.end() - 1u, 0xbf8a0000u);
  barrier.Decode();
  const auto rejected = barrier.Probe();
  Check(!rejected.success &&
            rejected.failure.find("barriers") != std::string::npos,
        "probe allowed early wave termination in a caller with a workgroup "
        "barrier");
  ProbeFixture extra;
  extra.code.insert(extra.code.end() - 1u, 0xbe8e210eu);
  extra.Decode();
  const auto unplanned = extra.Probe();
  Check(!unplanned.success &&
            unplanned.failure.find("without proved") != std::string::npos,
        "probe left an additional unplanned call executable");
}

void TestUnwrittenVgprExactRanges() {
  Decoder::Instruction inst;
  inst.family = Decoder::Family::VOP1;
  inst.opcode = Decoder::Opcode::V_MOV_B32;
  inst.dst = {.kind = Decoder::OperandKind::Vgpr, .reg = 27u};
  inst.src0 = {.kind = Decoder::OperandKind::IntegerInlineConstant, .value = 1u};
  inst.src_count = 1u;
  for (uint32_t reg = 0; reg < 256u; ++reg)
    Check(Decoder::ProvesVgprUnwritten(inst, reg) == (reg != 27u),
          "one-word vector destination proof lost exact register identity");
  inst.family = Decoder::Family::VOP3;
  inst.opcode = Decoder::Opcode::V_ADD_F64;
  inst.dst.reg = 26u;
  for (uint32_t reg = 0; reg < 256u; ++reg)
    Check(Decoder::ProvesVgprUnwritten(inst, reg) == (reg < 26u || reg > 27u),
          "64-bit vector destination proof missed the adjacent register");
  inst.family = Decoder::Family::MUBUF;
  inst.word_count = 2u;
  inst.opcode = Decoder::Opcode::BUFFER_LOAD_DWORDX4;
  inst.data_dwords = 4u;
  inst.data_components = 4u;
  inst.dst.reg = 25u;
  for (uint32_t reg = 0; reg < 256u; ++reg)
    Check(Decoder::ProvesVgprUnwritten(inst, reg) == (reg < 25u || reg > 28u),
          "memory vector destination proof narrowed its four-word width");
  inst.opcode = Decoder::Opcode::BUFFER_STORE_DWORDX4;
  Check(!Decoder::ProvesVgprUnwritten(inst, 27u),
        "store data in a decoder destination bypassed conservative coverage");
  inst.family = Decoder::Family::VOP1;
  inst.opcode = Decoder::Opcode::V_MOV_B32;
  inst.dst.reg = 3u;
  inst.dst2 = {.kind = Decoder::OperandKind::Vgpr, .reg = 26u};
  Check(!Decoder::ProvesVgprUnwritten(inst, 26u) &&
            !Decoder::ProvesVgprUnwritten(inst, 27u) &&
            Decoder::ProvesVgprUnwritten(inst, 28u),
        "secondary vector pair proof lost its second register");
}

void TestUnwrittenVgprConservativeUnknownsAndIndexing() {
  Decoder::Instruction valid;
  valid.family = Decoder::Family::VOP1;
  valid.opcode = Decoder::Opcode::V_MOV_B32;
  valid.dst = {.kind = Decoder::OperandKind::Vgpr, .reg = 3u};
  valid.src0 = {.kind = Decoder::OperandKind::IntegerInlineConstant, .value = 1u};
  valid.src_count = 1u;
  Check(Decoder::ProvesVgprUnwritten(valid, 27u), "valid disjoint move did not prove coverage");
  for (uint32_t variant = 0; variant < 11u; ++variant) {
    auto inst = valid;
    switch (variant) {
      case 0: inst.family = Decoder::Family::Unknown; break;
      case 1: inst.opcode = Decoder::Opcode::UNKNOWN; break;
      case 2: inst.opcode = Decoder::Opcode::UNSUPPORTED; break;
      case 3: inst.word_count = 0; break;
      case 4: inst.src_count = 5; break;
      case 5: inst.src0.kind = Decoder::OperandKind::Unknown; break;
      case 6: inst.dst.kind = Decoder::OperandKind::Unknown; break;
      case 7: inst.src0.kind = Decoder::OperandKind::M0; break;
      case 8: inst.opcode = Decoder::Opcode::V_MOVRELD_B32; break;
      case 9: inst.opcode = Decoder::Opcode::V_MOVRELS_B32; break;
      case 10: inst.unsupported_reason = "unproved encoding"; break;
    }
    Check(!Decoder::ProvesVgprUnwritten(inst, 27u),
          "unknown width/index/operand state falsely proved a protected register unchanged");
  }
  auto scalar = valid;
  scalar.family = Decoder::Family::SOPK;
  scalar.opcode = Decoder::Opcode::S_SETREG_B32;
  scalar.dst = {.kind = Decoder::OperandKind::Sgpr, .reg = 0u};
  Check(!Decoder::ProvesVgprUnwritten(scalar, 27u) &&
            !Decoder::ProvesVgprUnwritten(valid, 256u),
        "register bank state or out-of-range protection was admitted");
  auto memory = valid;
  memory.family = Decoder::Family::MUBUF;
  memory.opcode = Decoder::Opcode::BUFFER_LOAD_DWORDX4;
  memory.data_dwords = 0u;
  Check(!Decoder::ProvesVgprUnwritten(memory, 27u), "unknown memory width was admitted");
  valid.dst.reg = 255u;
  valid.opcode = Decoder::Opcode::V_ADD_F64;
  valid.family = Decoder::Family::VOP3;
  Check(!Decoder::ProvesVgprUnwritten(valid, 27u), "vector width overflow was admitted");
}

void TestUnwrittenVgprNativeStatusTuples() {
  // Native v26 data plus an unmodeled status DWORD can touch protected v27.
  // Ordinary one-word loads remain eligible; status/LDS modes do not.
  const std::array<uint32_t, 2> image{0xf0000108u, 0x00001a14u};
  Decoder::Instruction decoded;
  Decoder::DecodeInstruction(image, 0u, decoded);
  Check(decoded.opcode == Decoder::Opcode::IMAGE_LOAD && decoded.dst.reg == 26u &&
            decoded.data_dwords == 1u && Decoder::ProvesVgprUnwritten(decoded, 27u),
        "ordinary native one-word image load failed adjacent-register proof");
  for (uint32_t status : {1u << 16u, 1u << 17u}) {
    auto words = image;
    words[0] |= status;
    Decoder::Instruction inst;
    Decoder::DecodeInstruction(words, 0u, inst);
    Check(!Decoder::ProvesVgprUnwritten(inst, 27u),
          "native image TFE/LWE tuple bypassed protected adjacent-register proof");
  }
  const std::array<uint32_t, 2> buffer{0xe0301000u, 0x80001a00u};
  Decoder::Instruction normal;
  Decoder::DecodeInstruction(buffer, 0u, normal);
  Check(normal.opcode == Decoder::Opcode::BUFFER_LOAD_DWORD && normal.dst.reg == 26u &&
            normal.data_dwords == 1u && Decoder::ProvesVgprUnwritten(normal, 27u),
        "ordinary native one-word buffer load failed adjacent-register proof");
  for (uint32_t mode = 0; mode < 2u; ++mode) {
    auto words = buffer;
    words[mode == 0u ? 1u : 0u] |= mode == 0u ? (1u << 23u) : (1u << 16u);
    Decoder::Instruction inst;
    Decoder::DecodeInstruction(words, 0u, inst);
    Check(!Decoder::ProvesVgprUnwritten(inst, 27u),
          "native buffer TFE or LDS destination bypassed conservative coverage");
  }
}

void TestCheckedCoverageRetainsFullAddressAndStrictClosure() {
  ProbeFixture fixture;
  fixture.library.functions = {{0u, Fixture::A, {}}, {1u, Fixture::B, {}}};
  fixture.library.functions[0].code_prefix = {0x7e0e0281u, 0xbe80200eu}; // v7 only
  fixture.library.functions[1].code_prefix = {0x7e360282u, 0xbe80200eu}; // v27
  const auto checked = Shader::LinkExternalProgram(fixture.caller, fixture.library, 27u);
  Check(checked.success && checked.coverage_vgpr == 27u && checked.entries.size() == 1u &&
            checked.excluded_addresses == std::vector<uint64_t>{Fixture::B},
        "checked coverage failed to exclude a reachable write to exactly v27");
  const auto call = std::ranges::find_if(checked.transfers, [](const auto &transfer) { return transfer.call; });
  Check(call != checked.transfers.end() && call->guest_addresses == std::vector<uint64_t>{Fixture::A} &&
            call->link_address == Fixture::Caller + 12u && call->target_sgpr == 14u && call->return_sgpr == 14u,
        "checked dispatch narrowed high words or changed the aliased source/link semantics");
  const auto normal = Shader::LinkExternalProgram(fixture.caller, fixture.library);
  Check(normal.success && normal.entries.size() == 2u && normal.excluded_addresses.empty() &&
            normal.coverage_vgpr == UINT32_MAX, "ordinary mode inherited a partial-coverage filter");
  fixture.library.functions[1].code_prefix = {0xbf8a0000u, 0xbe80200eu}; // barrier
  Check(Shader::LinkExternalProgram(fixture.caller, fixture.library, 27u).excluded_addresses ==
            std::vector<uint64_t>{Fixture::B}, "barrier body was admitted to an early-ending checked wave");
  fixture.library.functions[1].code_prefix = {0xbe8e0381u, 0xbe80200eu}; // saved link clobber
  Check(Shader::LinkExternalProgram(fixture.caller, fixture.library, 27u).excluded_addresses ==
            std::vector<uint64_t>{Fixture::B}, "protected VGPR proof bypassed saved-link closure");
  fixture.library.functions[1].code_prefix = {0xbf820010u}; // uncovered branch
  Check(Shader::LinkExternalProgram(fixture.caller, fixture.library, 27u).excluded_addresses ==
            std::vector<uint64_t>{Fixture::B}, "uncovered branch body was silently admitted");
  fixture.library.functions[0].code_prefix = {0x7e360281u, 0xbe80200eu};
  const auto empty = Shader::LinkExternalProgram(fixture.caller, fixture.library, 27u);
  Check(!empty.success && empty.failure.find("no bodies satisfying") != std::string::npos,
        "empty checked domain incorrectly became successful call execution");
}

void TestCheckedCallerRejectsUnprovedBankAndBarrier() {
  for (uint32_t variant = 0; variant < 4u; ++variant) {
    Fixture fixture;
    auto bank = fixture.caller.instructions.front();
    bank.pc = 2u;
    if (variant == 0u) bank.opcode = Decoder::Opcode::S_SETREG_B32;
    if (variant == 1u) bank.opcode = Decoder::Opcode::V_MOVRELD_B32;
    if (variant == 2u) bank.src0.kind = Decoder::OperandKind::M0;
    if (variant == 3u) bank.opcode = Decoder::Opcode::S_BARRIER;
    fixture.caller.instructions.push_back(bank);
    const auto result = Shader::LinkExternalProgram(fixture.caller, fixture.library, 27u);
    Check(!result.success && result.failure.find(variant == 3u ? "barriers" : "bank/index") != std::string::npos,
          "checked body proof ignored caller-wide bank/index state or synchronization");
  }
}

void TestActualCapturedCheckedCoverage(const char *caller_path, const char *folder_path, const char *count_text) {
  size_t count = 0;
  const auto parsed = std::from_chars(count_text, count_text + std::strlen(count_text), count);
  Check(parsed.ec == std::errc{} && parsed.ptr == count_text + std::strlen(count_text),
        "actual coverage count must be a decimal integer");
  const auto started = std::chrono::steady_clock::now();
  auto fixture = CapturedExternalTest::Load(caller_path, folder_path, count, true);
  Check(count == fixture.total_functions && !fixture.synthetic_inputs,
        "coverage measurement requires every candidate and actual input metadata");
  Decoder::Program caller;
  Decoder::DecodeProgram(fixture.caller, caller);
  const auto loaded = std::chrono::steady_clock::now();
  const auto checked = Shader::LinkExternalProgram(caller, fixture.library, 27u);
  if (!checked.success) std::fprintf(stderr, "actual checked coverage rejection: %s\n", checked.failure.c_str());
  Check(checked.success, "actual complete candidate library failed strict checked linkage");
  const auto graph = CFG::BuildGraph(checked.program, checked.transfers);
  Check(!graph.unsupported && graph.irreducible, "actual checked CFG failed its dispatcher protocol");
  const auto finished = std::chrono::steady_clock::now();
  std::printf("ActualCheckedCoverage: total=%zu admitted=%zu excluded=%zu protected_vgpr=%u native_instructions=%zu blocks=%zu caller_base=0x%016llx\n",
              fixture.total_functions, checked.entries.size(), checked.excluded_addresses.size(), checked.coverage_vgpr,
              checked.program.instructions.size(), graph.blocks.size(),
              static_cast<unsigned long long>(fixture.controls.caller_address));
  Check(checked.entries.size() + checked.excluded_addresses.size() == fixture.total_functions,
        "actual coverage candidate accounting is incomplete");
  std::printf("ActualCheckedCoverage: preparation_seconds=%.3f linkage_and_cfg_seconds=%.3f peak_working_set_bytes=%zu\n",
              std::chrono::duration<double>(loaded - started).count(),
              std::chrono::duration<double>(finished - loaded).count(), PeakWorkingSetBytes());
  std::puts("ActualCheckedCoverage: full captured candidate classification; excluded targets fault at runtime; no IR/SPIRV/GPU execution");
}

void TestCheckedCoverageRequiresScalarRecordProvenance() {
  for (uint32_t missing = 0; missing < 5u; ++missing) {
    ProbeFixture fixture;
    fixture.library.functions = {{0u, Fixture::A, {0xbe80200eu}}, {1u, Fixture::B, {0xbe80200eu}}};
    auto &site = fixture.library.call_sites[0];
    if (missing == 0u) site.record_load_pc = UINT32_MAX;
    if (missing == 1u) site.record_sgpr = 4u;
    if (missing == 2u) site.auxiliary_sgpr = UINT32_MAX;
    if (missing == 3u) site.context_domain = UINT32_MAX;
    if (missing == 4u) site.records.clear();
    const auto result = Shader::LinkExternalProgram(fixture.caller, fixture.library, 27u);
    Check(!result.success && result.failure.find("proved record load") != std::string::npos,
          "checked scalar guard was constructed without complete record/auxiliary provenance");
  }
}

void TestDistinctPairAndCopiedReturn() {
	Fixture fixture; fixture.caller_code[0] = 0xbe90210eu; fixture.DecodeCaller();
	fixture.library.call_sites[0].return_sgpr = 16u;
	fixture.library.call_sites[0].auxiliary_sgpr = UINT32_MAX;
	fixture.OneFunction({0xbe940410u, 0xbe900381u, 0xbe802014u}); // save s16:s17 to s20:s21, clobber s16, return s20
	const auto linked = fixture.Link();
	Check(linked.success && std::any_of(linked.transfers.begin(), linked.transfers.end(), [](const auto& transfer) {
		return !transfer.call && transfer.target_sgpr == 20u && transfer.guest_addresses == std::vector<uint64_t>{Fixture::Caller + 4u};
	}), "distinct return destination or copied saved-link provenance was rejected");
}

void TestMultipleCallSitesKeepIndependentContinuations() {
	Fixture fixture;
	fixture.caller_code = {0xbe8e210eu, 0x7e120281u, 0xbe8e210eu, 0xbf810000u}; fixture.DecodeCaller();
	fixture.OneFunction({0xbe80200eu});
	auto second = fixture.library.call_sites[0]; second.caller_pc = 8u; second.context_domain = 4u;
	fixture.library.call_sites.push_back(second);
	const auto linked = fixture.Link();
	Check(linked.success && linked.entries.size() == 2u && linked.entries[0].pc != linked.entries[1].pc &&
	          linked.entries[0].domain_id == 3u && linked.entries[1].domain_id == 4u,
	      "shared leaf body was not cloned per call/context domain");
	std::vector<uint64_t> links;
	for (const auto& transfer: linked.transfers) if (transfer.call) links.push_back(transfer.link_address);
	Check(links == std::vector<uint64_t>{Fixture::Caller + 4u, Fixture::Caller + 12u},
	      "independent caller continuations were coalesced");
}

void TestReachableForwardSkipAndNegativeBranchUnion() {
	Fixture forward; forward.OneFunction({0xbf820001u, 0xffffffffu, 0xbe80200eu});
	const auto linked = forward.Link();
	Check(linked.success && linked.program.instructions.size() == forward.caller.instructions.size() + 2u,
	      "reachable-only decoding examined skipped metadata after a direct branch");
	Fixture negative; negative.OneFunction({0xbf82fffdu});
	negative.library.functions[0].guest_address = Fixture::A + 8u;
	negative.library.call_sites[0].candidate_addresses = {Fixture::A + 8u};
	negative.library.functions.push_back({1u, Fixture::A, {0xbe80200eu, 0xffffffffu, 0xbf82fffdu}});
	const auto backward = negative.Link();
	Check(backward.success && backward.entries[0].pc > negative.caller.instructions.size() * 4u &&
	          std::any_of(backward.transfers.begin(), backward.transfers.end(), [](const auto& transfer) {
		          return !transfer.call && transfer.guest_pc == Fixture::A;
	          }), "negative branch into a byte-consistent earlier captured prefix failed");
}

void TestConsistentOverlapAndAdjacentRanges() {
	Fixture overlap; overlap.OneFunction({0xbf820001u, 0x7e0e0281u, 0xbe80200eu});
	overlap.library.functions.push_back({1u, Fixture::A + 4u, {0x7e0e0281u, 0xbe80200eu}});
	Check(overlap.Link().success, "consistent overlapping prefix union was rejected");
	overlap.library.functions.back().code_prefix[0] ^= 1u;
	Rejected(overlap, "conflicting overlapping");
	Fixture adjacent; adjacent.OneFunction({0xbf820003u});
	adjacent.library.functions.push_back({1u, Fixture::A + 4u,
	                                     {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xbe80200eu}});
	Check(adjacent.Link().success, "checked contiguous prefix assembly could not close a reachable branch");
}

void TestUnsupportedLeafFormsAreRejected() {
	for (const auto& [code, reason]: std::vector<std::pair<std::vector<uint32_t>, std::string>>{
	         {{0xbe8e210eu, 0xbe80200eu}, "nested calls"},
	         {{0xbe8e1f00u, 0xbe80200eu}, "GETPC"},
	         {{0xbf810000u}, "without returning"},
	         {{0xffffffffu}, "unknown external"},
	         {{0xbe8e7f0eu}, "unsupported external"}}) {
		Fixture fixture; fixture.OneFunction(code); Rejected(fixture, reason);
	}
}

void TestSavedLinkMustSurviveEveryReturnPath() {
	Fixture wrong_pair; wrong_pair.OneFunction({0xbe802010u}); Rejected(wrong_pair, "saved-link return");
	Fixture clobber; clobber.OneFunction({0xbe8e0381u, 0xbe80200eu}); Rejected(clobber, "saved-link return");
	Fixture conditional;
	conditional.OneFunction({0xbf840002u, 0xbe8e0381u, 0xbf820000u, 0xbe80200eu});
	Rejected(conditional, "saved-link return");
}

void TestProvenanceAndCandidateSetValidation() {
	Fixture incomplete; incomplete.library.complete = false; Rejected(incomplete, "incomplete");
	Fixture absent; absent.library.call_sites[0].candidate_addresses.push_back(Fixture::A + 0x100000u);
	Rejected(absent, "absent external");
	Fixture bad_pair; bad_pair.library.call_sites[0].target_sgpr = 16u; Rejected(bad_pair, "register pairs");
	Fixture duplicate; duplicate.library.call_sites.push_back(duplicate.library.call_sites[0]); Rejected(duplicate, "duplicate external call");
	Fixture missing_continuation; missing_continuation.caller.instructions.pop_back(); Rejected(missing_continuation, "continuation");
	Fixture missing_domain; missing_domain.library.call_sites.clear(); Rejected(missing_domain, "no call sites");
	Fixture no_candidates; no_candidates.library.call_sites[0].candidate_addresses.clear(); Rejected(no_candidates, "no candidates");
	Fixture duplicate_id; duplicate_id.library.functions[1].function_id = 0u; Rejected(duplicate_id, "duplicate external function");
}

void TestUncoveredAndTruncatedBranchClosure() {
	Fixture missing; missing.OneFunction({0xbf820010u}); Rejected(missing, "uncovered reachable");
	Fixture truncated; truncated.OneFunction({0xbeeb03ffu}); Rejected(truncated, "incomplete external instruction");
	Fixture middle; middle.OneFunction({0xbf840001u, 0xbe8e03ffu, 0x7e0e0281u, 0xbe80200eu});
	Rejected(middle, "middle of an instruction");
}

std::vector<uint32_t> ReadCapturedWords(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	Check(bool(file), "cannot open captured binary");
	const auto bytes = file.tellg();
	Check(bytes > 0 && bytes <= 1024 * 1024 && bytes % 4 == 0, "captured binary exceeds bounded aligned input size");
	std::vector<uint32_t> words(static_cast<size_t>(bytes) / 4u);
	file.seekg(0); file.read(reinterpret_cast<char*>(words.data()), bytes);
	Check(bool(file), "captured binary read was incomplete");
	return words;
}

size_t PeakWorkingSetBytes() {
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS counters{};
	Check(GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0,
	      "cannot measure own offline test peak working set");
	return counters.PeakWorkingSetSize;
#else
	rusage usage{}; Check(getrusage(RUSAGE_SELF, &usage) == 0, "cannot measure own offline test peak RSS");
#ifdef __APPLE__
	return static_cast<size_t>(usage.ru_maxrss);
#else
	return static_cast<size_t>(usage.ru_maxrss) * 1024u;
#endif
#endif
}

void TestFullCapturedLibraryLink(const char* caller_path, const char* folder_path) {
	const auto started = std::chrono::steady_clock::now();
	const auto caller_words = ReadCapturedWords(caller_path);
	Decoder::Program caller; Decoder::DecodeProgram(caller_words, caller);
	const auto table = ReadCapturedWords(std::filesystem::path(folder_path) / "table_00.bin");
	Check(table.size() % 4u == 0u, "captured table is not a whole sequence of native records");
	std::map<uint64_t, std::vector<uint32_t>> prefixes;
	for (const auto& entry: std::filesystem::directory_iterator(folder_path)) {
		const auto filename = entry.path().filename().string();
		if (!filename.starts_with("target_") || !filename.ends_with(".bin")) continue;
		const auto begin = filename.find_last_of('_') + 1u;
		const auto hex = std::string_view(filename).substr(begin, filename.size() - begin - 4u);
		uint64_t address = 0;
		const auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), address, 16);
		Check(parsed.ec == std::errc{} && parsed.ptr == hex.data() + hex.size() && address &&
		          (address & 3u) == 0u && address < (uint64_t{1} << 48u), "captured prefix address is invalid");
		auto words = ReadCapturedWords(entry.path());
		Check(words.size() * sizeof(uint32_t) == 65536u, "full captured-link mode requires complete 64 KiB prefixes");
		Check(prefixes.emplace(address, std::move(words)).second, "duplicate captured prefix address");
	}
	Shader::ExternalLibraryPlan library;
	// The diagnostic manifest does not preserve a caller base. This synthetic high
	// base measures relative linkage/CFG scale and does not validate live link bits.
	library.caller_address = Fixture::Caller;
	std::map<uint64_t, uint32_t> ids;
	for (auto& [address, words]: prefixes) {
		const auto id = static_cast<uint32_t>(library.functions.size()); ids.emplace(address, id);
		library.functions.push_back({id, address, std::move(words)});
	}
	std::array<uint32_t, 64> dummy_user{}; dummy_user[0] = 0x2000u; dummy_user[1] = 0x10u;
	const auto traced = Shader::Diagnostics::TraceCallTables(caller, dummy_user);
	Check(traced.calls.size() == 1u && !traced.call_sites_truncated && traced.calls[0].rejection.empty(),
	      "actual captured caller did not prove exactly one function-table origin");
	const auto& trace = traced.calls[0];
	Shader::ExternalCallSite site;
	site.caller_pc = trace.call_pc; site.target_sgpr = trace.target_sgpr; site.return_sgpr = trace.return_sgpr;
	site.record_load_pc = trace.record_load_pc; site.context_domain = 0; site.auxiliary_sgpr = 16;
	for (const auto& [address, id]: ids) site.candidate_addresses.push_back(address);
	std::set<uint64_t> table_targets;
	for (uint32_t record = 0; record < table.size() / 4u; ++record) {
		const auto first = record * 4u;
		const auto code = uint64_t{table[first]} | uint64_t{table[first + 1u]} << 32u;
		const auto found = ids.find(code); Check(found != ids.end(), "captured table target lacks its own complete prefix");
		table_targets.insert(code);
		const auto context = uint64_t{table[first + 2u]} | uint64_t{table[first + 3u]} << 32u;
		site.records.push_back({record, found->second, code, context});
		site.context_records.push_back({record, found->second, {table[first], table[first + 1u], table[first + 2u], table[first + 3u]}});
	}
	Check(ids.size() == table_targets.size(), "prefix set contains an address not proven by the captured table");
	library.call_sites.push_back(std::move(site)); library.complete = true;
	const auto before_link = std::chrono::steady_clock::now();
	const auto linked = Shader::LinkExternalProgram(caller, library);
	const auto after_link = std::chrono::steady_clock::now();
	if (!linked.success) std::fprintf(stderr, "captured link rejection: %s\n", linked.failure.c_str());
	Check(linked.success, "full captured leaf library did not link");
	const auto graph = CFG::BuildGraph(linked.program, linked.transfers);
	const auto after_graph = std::chrono::steady_clock::now();
	if (graph.unsupported) std::fprintf(stderr, "captured CFG rejection: %s\n", graph.unsupported_reason.c_str());
	Check(!graph.unsupported && graph.irreducible &&
	          graph.failure_kind == CFG::FailureKind::IrreducibleControlFlow,
	      "full captured native CFG did not build its dispatcher fallback");
	const auto seconds = [](auto begin, auto end) { return std::chrono::duration<double>(end - begin).count(); };
	std::printf("CapturedExternalProgram: candidate_prefixes=%zu records=%zu caller_instructions=%zu linked_instructions=%zu blocks=%zu transfers=%zu entries=%zu\n",
	            library.functions.size(), table.size() / 4u, caller.instructions.size(), linked.program.instructions.size(),
	            graph.blocks.size(), linked.transfers.size(), linked.entries.size());
	std::printf("CapturedExternalProgram: preparation_seconds=%.3f link_seconds=%.3f cfg_seconds=%.3f peak_working_set_bytes=%zu native_instruction_bytes=%zu\n",
	            seconds(started, before_link), seconds(before_link, after_link), seconds(after_link, after_graph),
	            PeakWorkingSetBytes(), sizeof(Decoder::Instruction));
	std::puts("CapturedExternalProgram: caller base is synthetic; no IR/GPU/guest execution or live-link-bit validation");
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--saved-link-width-only") {
    TestSavedLinkScalarWriteWidths(); return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--saved-link-width-captured-body") {
    TestCapturedBodySavedLinkWidth(argv[2]); return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--checked-dominance-rejection") {
    ProbeFixture fixture;
    fixture.library.functions = {{0u, Fixture::A, {0xbe80200eu}}, {1u, Fixture::B, {0xbe80200eu}}};
    fixture.code.insert(fixture.code.begin(), 0xbf850002u);
    fixture.Decode();
    fixture.library.call_sites[0].record_load_pc = 4u;
    fixture.library.call_sites[0].caller_pc = 12u;
    const auto checked = Shader::LinkExternalProgram(fixture.caller, fixture.library, 27u);
    Check(checked.success, "checked dominance fixture did not reach CFG validation");
    (void)CFG::BuildGraph(checked.program, checked.transfers);
    Check(false, "checked call accepted a path skipping its scalar record load");
  }
  const bool checked_only = argc == 2 && std::string_view(argv[1]) == "--checked-only";
  const bool captured_checked = argc == 5 && std::string_view(argv[1]) == "--captured-checked-library";
  if (argc == 1 || checked_only || captured_checked) {
    TestUnwrittenVgprExactRanges();
    TestUnwrittenVgprConservativeUnknownsAndIndexing();
    TestUnwrittenVgprNativeStatusTuples();
    TestCheckedCoverageRetainsFullAddressAndStrictClosure();
    TestCheckedCallerRejectsUnprovedBankAndBarrier();
    TestCheckedCoverageRequiresScalarRecordProvenance();
    std::puts("ExternalProgramTests: all six checked-coverage groups passed (CPU native proof/linkage; no guest execution)");
    if (checked_only) return 0;
    if (captured_checked) {
      TestActualCapturedCheckedCoverage(argv[2], argv[3], argv[4]);
      return 0;
    }
  }
  if (argc == 2 && std::strcmp(argv[1], "--probe-dominance-rejection") == 0) {
    ProbeFixture fixture;
    fixture.code.insert(fixture.code.begin(), 0xbf850002u);
    fixture.Decode();
    fixture.library.call_sites[0].record_load_pc = 4u;
    fixture.library.call_sites[0].caller_pc = 12u;
    const auto probe = fixture.Probe();
    Check(probe.success, "dominance fixture did not reach CFG validation");
    (void)CFG::BuildGraph(probe.program, probe.transfers);
    Check(false, "probe accepted a path skipping its record load");
  }
  const bool probe_only =
      argc == 2 && std::string_view(argv[1]) == "--probe-only";
  Check(argc == 1 || probe_only ||
            (argc == 4 && std::string_view(argv[1]) == "--captured-library"),
        "usage: ExternalProgramTests [--probe-only | --captured-library "
        "caller.bin captured-prefix-folder]");
  TestProbePreservesCallBoundaryAndRegisters();
  TestProbeRejectsMissingProvenance();
  TestProbeRejectsBarriersAndUnplannedCalls();
  if (probe_only) {
    std::puts("ExternalProgramTests: all three probe groups passed "
              "(caller-only construction/CFG; no guest execution)");
    return 0;
  }
  TestAliasedFullAddressDispatcherAndCFG();
  TestDistinctPairAndCopiedReturn();
  TestMultipleCallSitesKeepIndependentContinuations();
  TestReachableForwardSkipAndNegativeBranchUnion();
  TestConsistentOverlapAndAdjacentRanges();
  TestUnsupportedLeafFormsAreRejected();
  TestSavedLinkMustSurviveEveryReturnPath();
  TestSavedLinkScalarWriteWidths();
  TestProvenanceAndCandidateSetValidation();
  TestUncoveredAndTruncatedBranchClosure();
  std::puts("ExternalProgramTests: all nineteen groups passed (offline native "
            "linkage/probe/CFG only)");
  if (argc == 4)
    TestFullCapturedLibraryLink(argv[2], argv[3]);
}
