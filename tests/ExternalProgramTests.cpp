#include "graphics/shader/recompiler/ExternalProgram.h"
#include "graphics/shader/recompiler/ShaderCallDiagnostics.h"
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

int main(int argc, char** argv) {
	Check(argc == 1 || (argc == 4 && std::string_view(argv[1]) == "--captured-library"),
	      "usage: ExternalProgramTests [--captured-library caller.bin captured-prefix-folder]");
	TestAliasedFullAddressDispatcherAndCFG();
	TestDistinctPairAndCopiedReturn();
	TestMultipleCallSitesKeepIndependentContinuations();
	TestReachableForwardSkipAndNegativeBranchUnion();
	TestConsistentOverlapAndAdjacentRanges();
	TestUnsupportedLeafFormsAreRejected();
	TestSavedLinkMustSurviveEveryReturnPath();
	TestProvenanceAndCandidateSetValidation();
	TestUncoveredAndTruncatedBranchClosure();
	std::puts("ExternalProgramTests: all nine groups passed (offline native linkage/CFG only)");
	if (argc == 4) TestFullCapturedLibraryLink(argv[2], argv[3]);
}
