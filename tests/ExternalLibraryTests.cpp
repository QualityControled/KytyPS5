#include "graphics/shader/recompiler/ExternalLibrary.h"
#include "graphics/shader/recompiler/ShaderCallDiagnostics.h"
#include "common/assert.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// These tests use a guarded mock address space and never execute guest code or CFGs.
namespace Common {
int DbgExitHandler(const char*, int, std::string_view text) { throw std::runtime_error(std::string(text)); }
int DbgExitIfHandler(const char* expression, const char*, int) { throw std::runtime_error(expression); }
int DbgNotImplementedHandler(const char* expression, const char*, int) { throw std::runtime_error(expression); }
void DbgExit(int status) { std::exit(status); }
} // namespace Common

namespace {
namespace Shader = Libs::Graphics::ShaderRecompiler;
namespace Decoder = Shader::Decoder;
namespace Diagnostics = Shader::Diagnostics;

void Check(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }

struct Memory {
	struct Region {
		uint64_t base;
		std::vector<uint32_t> words;
		size_t generated_words = 0;
		uint32_t generated_seed = 0;
	};
	struct Request { uint64_t address; size_t bytes; };
	std::vector<Region> regions;
	std::vector<Request> requests;
	uint64_t fail_address = UINT64_MAX;
	uint64_t corrupt_second_read_address = UINT64_MAX;
	size_t matching_corrupt_reads = 0;

	void Add(uint64_t base, std::vector<uint32_t> words) { regions.push_back({base, std::move(words)}); }
	void AddGenerated(uint64_t base, size_t bytes, uint32_t seed) {
		regions.push_back({base, {}, bytes / sizeof(uint32_t), seed});
	}
	std::vector<uint32_t>& Words(uint64_t base) {
		const auto found = std::find_if(regions.begin(), regions.end(), [&](const auto& r) { return r.base == base; });
		Check(found != regions.end(), "test region missing");
		return found->words;
	}
	static bool Read(void* context, uint64_t address, std::span<uint32_t> words) {
		auto& memory = *static_cast<Memory*>(context);
		memory.requests.push_back({address, words.size_bytes()});
		Check(words.size_bytes() <= Diagnostics::ReadChunkBytes, "guarded loader exceeded chunk bound");
		if (address == memory.fail_address) {
			if (!words.empty()) words.front() = 0xbadbad00u;
			return false;
		}
		for (const auto& region: memory.regions) {
			if (address < region.base || (address - region.base) % 4u) continue;
			const auto offset = (address - region.base) / 4u;
			const auto available = region.generated_words ? region.generated_words : region.words.size();
			if (offset > available || words.size() > available - offset) continue;
			if (region.generated_words) {
				for (size_t word = 0; word < words.size(); ++word)
					words[word] = region.generated_seed + static_cast<uint32_t>(offset + word);
			} else {
				std::copy_n(region.words.begin() + static_cast<size_t>(offset), words.size(), words.begin());
			}
			if (address == memory.corrupt_second_read_address &&
			    ++memory.matching_corrupt_reads >= 2u && !words.empty()) words.front() ^= 1u;
			return true;
		}
		return false;
	}
};

std::vector<uint32_t> Descriptor(uint64_t base, uint32_t stride, uint32_t records) {
	return {static_cast<uint32_t>(base), static_cast<uint32_t>(base >> 32u) | (stride << 16u), records, 0u};
}
void AppendRecord(std::vector<uint32_t>& words, uint64_t code, uint64_t context) {
	words.insert(words.end(), {static_cast<uint32_t>(code), static_cast<uint32_t>(code >> 32u),
	                          static_cast<uint32_t>(context), static_cast<uint32_t>(context >> 32u)});
}

struct Fixture {
	static constexpr uint64_t Caller = 0x1350008000ull;
	static constexpr uint64_t Table = 0xec10000000ull;
	// Deliberately share low DWORDs to exercise full raw 64-bit code identity.
	static constexpr uint64_t TargetA = 0x1300100000ull;
	static constexpr uint64_t TargetB = 0x1200100000ull;
	static constexpr uint64_t AuxiliaryA = 0x10abcdef00ull;
	static constexpr uint64_t AuxiliaryB = 0x76543210fedcba98ull;
	Decoder::Program caller;
	std::array<uint32_t, 64> user_data {};
	Memory memory;

	Fixture() {
		user_data[0] = 0x2000u; user_data[1] = 0x10u;
		Append(0x6c8u, {0xf4081500u, 0xfa000140u});
		Append(0xac4u, {0x8f6a846au});
		Append(0xac8u, {0xf428012au, 0xd4000000u});
		Append(0xad0u, {0xbf8cc07fu});
		Append(0xad4u, {0xbe8e0304u}); Append(0xad8u, {0xbe8f0305u});
		Append(0xadcu, {0xbe900306u}); Append(0xae0u, {0xbe910307u});
		Append(0xae4u, {0xbe8e210eu});
		std::vector<uint32_t> table;
		AppendRecord(table, TargetA, AuxiliaryA); AppendRecord(table, TargetB, AuxiliaryB);
		AppendRecord(table, TargetA, AuxiliaryA + 0x100u);
		memory.Add(DescriptorAddress(), Descriptor(Table, 16u, 3u)); memory.Add(Table, std::move(table));
		for (const auto target: {TargetA, TargetB}) {
			std::vector<uint32_t> prefix(Diagnostics::MaxTargetBytes / 4u, 0xfeed0000u);
			prefix[0] = 0x7e0e0281u; prefix[1] = 0xbe80200eu;
			memory.Add(target, std::move(prefix));
		}
	}
	void Append(uint32_t pc, std::initializer_list<uint32_t> code) {
		Decoder::Instruction inst;
		Decoder::DecodeInstruction(std::span<const uint32_t>(code.begin(), code.size()), 0u, inst);
		inst.pc = pc; caller.instructions.push_back(inst);
	}
	uint64_t DescriptorAddress() const { return (uint64_t{user_data[1]} << 32u | user_data[0]) + 320u; }
	Shader::ExternalLibraryLoadResult Load(uint64_t caller_address = Caller, uint32_t user_data_base = 0u) {
		return Shader::LoadExternalLibrary(caller, caller_address, user_data, user_data_base, Memory::Read, &memory);
	}
};

void Rejected(const Shader::ExternalLibraryLoadResult& result, const char* message) {
	Check(result.has_calls && !result.plan.complete && !result.failure.empty(), message);
}

void TestNoCallAndConservativeOrigins() {
	Fixture fixture;
	const auto no_call = Shader::LoadExternalLibrary({}, 0u, {}, 0u, Memory::Read, &fixture.memory);
	Check(!no_call.has_calls && no_call.failure.empty() && !no_call.plan.complete && fixture.memory.requests.empty(),
	      "call-free program performed reads or claimed a library snapshot");
	for (const auto address: {uint64_t{0}, Fixture::Caller + 1u, uint64_t{1} << 48u}) {
		Rejected(fixture.Load(address), "invalid caller address was accepted");
		Check(fixture.memory.requests.empty(), "invalid caller address reached memory reader");
	}
	Rejected(fixture.Load(Fixture::Caller, 8u), "uninitialized user-data base was accepted");
	Check(fixture.memory.requests.empty(), "unproven user-data origin performed reads");
	const auto original = fixture.caller.instructions[4];
	Decoder::DecodeInstruction(std::array<uint32_t, 1>{0xbe8e0381u}, 0u, fixture.caller.instructions[4] = {});
	fixture.caller.instructions[4].pc = original.pc;
	Rejected(fixture.Load(), "immediate target pair was treated as table provenance");
	Check(fixture.memory.requests.empty(), "unproven target origin performed reads");
}

void TestAliasedSnapshotAndRawContexts() {
	Fixture fixture;
	const auto call_raw = fixture.caller.instructions.back().raw[0];
	const auto result = fixture.Load();
	Check(result.has_calls && result.failure.empty() && result.plan.complete, "valid full snapshot was rejected");
	const auto& plan = result.plan;
	Check(plan.functions.size() == 2u && plan.functions[0].guest_address == Fixture::TargetB &&
	          plan.functions[0].function_id == 0u && plan.functions[1].guest_address == Fixture::TargetA &&
	          plan.functions[1].function_id == 1u, "full-address ordering or deterministic function IDs changed");
	Check(plan.call_sites.size() == 1u && plan.call_sites[0].caller_pc == 0xae4u &&
	          plan.call_sites[0].target_sgpr == 14u && plan.call_sites[0].return_sgpr == 14u &&
	          plan.call_sites[0].record_sgpr == 4u && plan.call_sites[0].auxiliary_sgpr == 16u &&
	          fixture.caller.instructions.back().raw[0] == call_raw, "aliased call or auxiliary-pair trace changed");
	const auto& site = plan.call_sites[0];
	Check(site.records.size() == 3u && site.records[0].function_id == 1u && site.records[1].function_id == 0u &&
	          site.records[2].function_id == 1u && site.records[1].auxiliary_address == Fixture::AuxiliaryB &&
	          site.context_records[1].words == std::array<uint32_t, 4>{0x00100000u, 0x12u, 0xfedcba98u, 0x76543210u},
	      "raw auxiliary contexts were masked or duplicate-code records were coalesced");
	const auto domains = Shader::ExternalContextDomains(plan);
	Check(domains.size() == 1u && domains[0].complete && domains[0].domain_id == 0u &&
	          domains[0].records.size() == 3u && domains[0].records[2].ordinal == 2u,
	      "resource context domains lost per-record identity");
	Check(plan.dependencies.size() == 4u && plan.dependencies[0].address == Fixture::TargetB &&
	          plan.dependencies[1].address == Fixture::TargetA && plan.dependencies[2].address == fixture.DescriptorAddress() &&
	          plan.dependencies[3].address == Fixture::Table, "exact snapshot dependency order changed");
	Check(std::none_of(fixture.memory.requests.begin(), fixture.memory.requests.end(), [](const auto& request) {
		return request.address == Fixture::AuxiliaryA || request.address == Fixture::AuxiliaryB;
	}), "loader dereferenced auxiliary context as code");
}

void TestIncompleteSnapshotsAndDescriptors() {
	for (const uint32_t stride: {0u, 8u, 24u}) {
		Fixture fixture; fixture.memory.Words(fixture.DescriptorAddress())[1] =
		    static_cast<uint32_t>(Fixture::Table >> 32u) | stride << 16u;
		Rejected(fixture.Load(), "nonlinear or non-16-byte stride was accepted");
	}
	for (const bool swizzle: {false, true}) {
		Fixture fixture;
		if (swizzle) fixture.memory.Words(fixture.DescriptorAddress())[1] |= 1u << 31u;
		else fixture.memory.Words(fixture.DescriptorAddress())[3] |= 1u << 30u;
		Rejected(fixture.Load(), "swizzled or typed table descriptor was accepted");
	}
	Fixture short_table; short_table.memory.Words(Fixture::Table).resize(4u);
	Rejected(short_table.Load(), "partial table was treated as complete");
	Fixture short_code; short_code.memory.Words(Fixture::TargetB).resize(Diagnostics::MaxTargetBytes / 4u - 1u);
	Rejected(short_code.Load(), "partial candidate prefix was treated as complete");
	Fixture failed; failed.memory.fail_address = Fixture::TargetA + Diagnostics::MaxTargetBytes - Diagnostics::ReadChunkBytes;
	const auto result = failed.Load(); Rejected(result, "failed candidate last chunk was cached");
	Check(!Shader::ValidateExternalLibraryDependencies(result.plan, Memory::Read, &failed.memory),
	      "incomplete library passed dependency validation");
	Fixture no_reader;
	Rejected(Shader::LoadExternalLibrary(no_reader.caller, Fixture::Caller, no_reader.user_data, 0u, nullptr),
	         "null reader fabricated a full library");
}

void TestInvalidTargetsAndHardCaps() {
	for (const auto bad_target: {uint64_t{0}, Fixture::TargetA + 1u, uint64_t{1} << 48u}) {
		Fixture fixture; auto& table = fixture.memory.Words(Fixture::Table);
		table[0] = static_cast<uint32_t>(bad_target); table[1] = static_cast<uint32_t>(bad_target >> 32u);
		Rejected(fixture.Load(), "invalid code pointer was accepted");
	}
	Fixture cap; constexpr uint64_t base = 0x5000000000ull;
	std::vector<uint32_t> table;
	for (size_t target = 0u; target <= Diagnostics::MaxTargets; ++target)
		AppendRecord(table, base + target * Diagnostics::MaxTargetBytes, 0x10u);
	cap.memory.Words(Fixture::Table) = table;
	cap.memory.Words(cap.DescriptorAddress()) = Descriptor(Fixture::Table, 16u, Diagnostics::MaxTargets + 1u);
	cap.memory.AddGenerated(base, (Diagnostics::MaxTargets + 1u) * Diagnostics::MaxTargetBytes, 0xbe80200eu);
	Rejected(cap.Load(), "over-cap candidate set was treated as complete");
	Check(std::none_of(cap.memory.requests.begin(), cap.memory.requests.end(), [&](const auto& request) {
		return request.address >= base + Diagnostics::MaxAggregateTargetBytes &&
		       request.address < base + (Diagnostics::MaxTargets + 1u) * Diagnostics::MaxTargetBytes;
	}), "loader read candidate beyond count/aggregate bound");
	Fixture table_cap;
	constexpr auto too_many_records = Diagnostics::MaxTableBytes / 16u + 1u;
	std::vector<uint32_t> large_table;
	for (size_t record = 0u; record < too_many_records; ++record)
		AppendRecord(large_table, Fixture::TargetA, record);
	table_cap.memory.Words(Fixture::Table) = std::move(large_table);
	table_cap.memory.Words(table_cap.DescriptorAddress()) = Descriptor(Fixture::Table, 16u, too_many_records);
	Rejected(table_cap.Load(), "table beyond aggregate byte budget was treated as complete");
}

void TestFullTableTailContextsAndSnapshotOnlyCompleteness() {
	Fixture fixture; constexpr uint32_t records = 10644u;
	std::vector<uint32_t> table;
	for (uint32_t record = 0u; record < records; ++record)
		AppendRecord(table, record == 4096u ? Fixture::TargetB : Fixture::TargetA,
		             0x7654321000000000ull | record);
	fixture.memory.Words(Fixture::Table) = table;
	fixture.memory.Words(fixture.DescriptorAddress()) = Descriptor(Fixture::Table, 16u, records);
	// Candidate snapshots are complete even though a function decoder may later reject this code.
	fixture.memory.Words(Fixture::TargetB)[0] = 0xffffffffu;
	const auto result = fixture.Load();
	Check(result.plan.complete && result.failure.empty() && result.plan.functions.size() == 2u,
	      "snapshot-only completeness was confused with function closure");
	const auto& site = result.plan.call_sites[0];
	Check(site.records.size() == records && site.context_records.size() == records &&
	          site.records[4096u].function_address == Fixture::TargetB &&
	          site.context_records.back().ordinal == records - 1u &&
	          site.context_records.back().words[2] == records - 1u &&
	          site.context_records.back().words[3] == 0x76543210u &&
	          result.plan.dependencies.back().words == table, "raw table tail or auxiliary context was lost");
}

void TestDependencyValidationAndExactIdentity() {
	Fixture fixture; const auto first = fixture.Load(); const auto second = fixture.Load();
	Check(first.plan.complete && second.plan.complete && first.plan.dependency_hash == second.plan.dependency_hash &&
	          first.plan.dependencies == second.plan.dependencies, "unchanged snapshot identity was unstable");
	Check(Shader::ValidateExternalLibraryDependencies(first.plan, Memory::Read, &fixture.memory),
	      "unchanged mapped dependencies failed validation");
	const auto last_word = fixture.memory.Words(Fixture::TargetA).back();
	fixture.memory.Words(Fixture::TargetA).back() ^= 1u;
	Check(!Shader::ValidateExternalLibraryDependencies(first.plan, Memory::Read, &fixture.memory),
	      "code-prefix tail mutation reused stale compiled mapping");
	const auto changed_code = fixture.Load();
	Check(changed_code.plan.complete && changed_code.plan.dependencies != first.plan.dependencies &&
	          changed_code.plan.dependency_hash != first.plan.dependency_hash, "code-content mutation failed to change identity");
	fixture.memory.Words(Fixture::TargetA).back() = last_word;
	fixture.memory.Words(Fixture::Table)[2] ^= 4u;
	Check(!Shader::ValidateExternalLibraryDependencies(first.plan, Memory::Read, &fixture.memory),
	      "auxiliary-context mutation reused stale library mapping");
	auto changed_context = fixture.Load();
	Check(changed_context.plan.complete && changed_context.plan.dependency_hash != first.plan.dependency_hash,
	      "raw context change did not affect snapshot identity");
	changed_context.plan.dependency_hash = first.plan.dependency_hash;
	Check(changed_context.plan.dependencies != first.plan.dependencies,
	      "equal bucket hashes substituted for exact dependency equality");
	fixture.memory.Words(Fixture::Table)[2] ^= 4u;
	fixture.memory.Words(fixture.DescriptorAddress())[3] ^= 1u;
	Check(!Shader::ValidateExternalLibraryDependencies(first.plan, Memory::Read, &fixture.memory),
	      "descriptor content mutation reused stale library mapping");
	fixture.memory.Words(fixture.DescriptorAddress())[3] ^= 1u;
	fixture.memory.fail_address = Fixture::TargetB + Diagnostics::MaxTargetBytes - Diagnostics::ReadChunkBytes;
	Check(!Shader::ValidateExternalLibraryDependencies(first.plan, Memory::Read, &fixture.memory),
	      "failed dependency re-read used scribbled or stale values");
}

void TestMappingRelocationAndCallerIdentity() {
	Fixture fixture; const auto original = fixture.Load();
	const auto relocated_caller = fixture.Load(Fixture::Caller + 0x100000u);
	Check(relocated_caller.plan.complete && original.plan.dependencies == relocated_caller.plan.dependencies &&
	          original.plan.dependency_hash != relocated_caller.plan.dependency_hash,
	      "guest caller relocation did not invalidate address-based continuation identity");
	constexpr uint64_t relocation = 0x18bd5000u;
	fixture.user_data[0] += static_cast<uint32_t>(relocation);
	for (auto& region: fixture.memory.regions) region.base += relocation;
	fixture.memory.Words(fixture.DescriptorAddress()) = Descriptor(Fixture::Table + relocation, 16u, 3u);
	auto& table = fixture.memory.Words(Fixture::Table + relocation);
	for (size_t word = 0u; word < table.size(); word += 4u) {
		const auto address = (uint64_t{table[word + 1u]} << 32u | table[word]) + relocation;
		table[word] = static_cast<uint32_t>(address); table[word + 1u] = static_cast<uint32_t>(address >> 32u);
	}
	const auto moved = fixture.Load(Fixture::Caller + relocation);
	Check(moved.plan.complete && moved.plan.functions[0].guest_address == Fixture::TargetB + relocation &&
	          moved.plan.functions[0].code_prefix == original.plan.functions[0].code_prefix &&
	          moved.plan.dependencies != original.plan.dependencies && moved.plan.dependency_hash != original.plan.dependency_hash &&
	          !Shader::ValidateExternalLibraryDependencies(original.plan, Memory::Read, &fixture.memory),
	      "identical relocated bytes reused old guest mapping or function address");
}

void TestMergedCodeDependenciesAndConflict() {
	for (const size_t offset: {Diagnostics::MaxTargetBytes / 2u, Diagnostics::MaxTargetBytes}) {
		Fixture fixture;
		const auto target = Fixture::TargetA + offset;
		auto& table = fixture.memory.Words(Fixture::Table);
		table[4] = static_cast<uint32_t>(target); table[5] = static_cast<uint32_t>(target >> 32u);
		auto& mapped = fixture.memory.Words(Fixture::TargetA);
		mapped.resize((Diagnostics::MaxTargetBytes + offset) / 4u);
		for (size_t word = 0; word < mapped.size(); ++word) mapped[word] = 0x70000000u + static_cast<uint32_t>(word);
		const auto result = fixture.Load();
		Check(result.plan.complete && result.plan.dependencies.size() == 3u &&
		          result.plan.dependencies[0].address == Fixture::TargetA &&
		          result.plan.dependencies[0].words == mapped,
		      "consistent overlapping/adjacent code snapshots did not form an exact byte union");
		fixture.memory.requests.clear();
		Check(Shader::ValidateExternalLibraryDependencies(result.plan, Memory::Read, &fixture.memory),
		      "merged code dependencies did not validate against unchanged bytes");
		const auto code_reads = std::count_if(fixture.memory.requests.begin(), fixture.memory.requests.end(), [&](const auto& read) {
			return read.address >= Fixture::TargetA && read.address < Fixture::TargetA + Diagnostics::MaxTargetBytes + offset;
		});
		Check(static_cast<size_t>(code_reads) == (Diagnostics::MaxTargetBytes + offset) / Diagnostics::ReadChunkBytes,
		      "warm dependency validation read overlapping code more than once");
		mapped.back() ^= 1u;
		Check(!Shader::ValidateExternalLibraryDependencies(result.plan, Memory::Read, &fixture.memory),
		      "mutation in unique merged tail reused stale code dependencies");
	}
	Fixture conflict;
	const auto target = Fixture::TargetA + Diagnostics::MaxTargetBytes / 2u;
	conflict.memory.Words(Fixture::Table)[4] = static_cast<uint32_t>(target);
	conflict.memory.Words(Fixture::Table)[5] = static_cast<uint32_t>(target >> 32u);
	conflict.memory.Words(Fixture::TargetA).resize(3u * Diagnostics::MaxTargetBytes / 8u, 0x12345678u);
	conflict.memory.corrupt_second_read_address = target;
	const auto failed = conflict.Load();
	Rejected(failed, "conflicting overlapping code snapshots were accepted");
	Check(failed.failure.find("overlapping") != std::string::npos,
	      "overlap conflict was not diagnosed explicitly");
}

void TestOnlyProvedTableOriginInputsMatter() {
	Fixture fixture; const auto loaded = fixture.Load();
	Check(Shader::ExternalLibraryInputsMatch(loaded.plan, fixture.user_data), "unchanged table origin did not match");
	auto changed = fixture.user_data; changed.back() ^= 1u;
	Check(Shader::ExternalLibraryInputsMatch(loaded.plan, changed), "unrelated per-dispatch word invalidated the table origin");
	changed[0] ^= 4u;
	Check(!Shader::ExternalLibraryInputsMatch(loaded.plan, changed), "changed pointer low half matched old table origin");
	changed = fixture.user_data; changed[1] ^= 1u;
	Check(!Shader::ExternalLibraryInputsMatch(loaded.plan, changed), "changed pointer high half matched old table origin");
	Check(!Shader::ExternalLibraryInputsMatch(loaded.plan, std::span(fixture.user_data).first(1u)) &&
	          !Shader::ExternalLibraryInputsMatch(loaded.plan, fixture.user_data, 1u),
	      "missing pointer word or below-base origin was accepted");
	auto shifted = loaded.plan; shifted.call_sites[0].descriptor_user_sgpr = 2u;
	Check(Shader::ExternalLibraryInputsMatch(shifted, fixture.user_data, 2u) &&
	          !Shader::ExternalLibraryInputsMatch(shifted, fixture.user_data, 0u) &&
	          !Shader::ExternalLibraryInputsMatch(shifted, fixture.user_data, 3u),
	      "user-data register base was ignored in origin matching");
	auto negative = loaded.plan;
	negative.call_sites[0].descriptor_offset = -16;
	negative.call_sites[0].descriptor_address = 0xff0u;
	Check(Shader::ExternalLibraryInputsMatch(negative, std::array<uint32_t, 2>{0x1000u, 0u}),
	      "valid signed descriptor offset did not match");
	negative.call_sites[0].descriptor_address = UINT64_MAX - 7u;
	Check(!Shader::ExternalLibraryInputsMatch(negative, std::array<uint32_t, 2>{8u, 0u}),
	      "negative descriptor offset wrapped below zero");
	auto overflow = loaded.plan;
	overflow.call_sites[0].descriptor_offset = 8;
	overflow.call_sites[0].descriptor_address = 4u;
	Check(!Shader::ExternalLibraryInputsMatch(overflow, std::array<uint32_t, 2>{0xfffffffcu, 0xffffffffu}),
	      "positive descriptor offset overflow wrapped into matching address");
	auto incomplete = loaded.plan; incomplete.complete = false;
	Check(!Shader::ExternalLibraryInputsMatch(incomplete, fixture.user_data), "incomplete snapshot matched inputs");
}

void TestNativeDescriptorAddressNormalization() {
	Fixture fixture;
	// Native scalar loads ignore the upper 16 address bits and the low two base/offset bits.
	fixture.user_data[0] |= 3u;
	fixture.user_data[1] |= 0xabcd0000u;
	const auto loaded = fixture.Load();
	Check(loaded.plan.complete && loaded.failure.empty(), "tagged native descriptor base was not captured");
	Check(Shader::ExternalLibraryInputsMatch(loaded.plan, fixture.user_data),
	      "warm-cache descriptor origin did not use native 48-bit aligned address semantics");
	auto plan = loaded.plan;
	plan.call_sites[0].descriptor_offset = 323;
	Check(Shader::ExternalLibraryInputsMatch(plan, fixture.user_data), "positive offset low bits changed native origin");
	plan.call_sites[0].descriptor_offset = -1;
	plan.call_sites[0].descriptor_address = 0xffcu;
	Check(Shader::ExternalLibraryInputsMatch(plan, std::array<uint32_t, 2>{0x1003u, 0xabcd0000u}),
	      "signed offset was not aligned before native subtraction");
	plan.call_sites[0].descriptor_offset = 0;
	plan.call_sites[0].descriptor_address = 0x0000fffffffffff0ull;
	Check(Shader::ExternalLibraryInputsMatch(plan, std::array<uint32_t, 2>{0xfffffff3u, 0xffffffffu}),
	      "last complete aligned 16-byte descriptor was rejected");
	plan.call_sites[0].descriptor_address = 0x0000fffffffffff8ull;
	Check(!Shader::ExternalLibraryInputsMatch(plan, std::array<uint32_t, 2>{0xfffffff8u, 0x0000ffffu}),
	      "descriptor span crossed the 48-bit address boundary");
	plan.call_sites[0].descriptor_offset = std::numeric_limits<int32_t>::min();
	plan.call_sites[0].descriptor_address = 0u;
	Check(Shader::ExternalLibraryInputsMatch(plan, std::array<uint32_t, 2>{0x80000003u, 0xabcd0000u}) &&
	          !Shader::ExternalLibraryInputsMatch(plan, std::array<uint32_t, 2>{0x7fffffffu, 0u}),
	      "minimum signed offset overflowed or wrapped below the native address space");
}

void TestAuxiliaryPairWriteWidthsAndClobbers() {
	const auto check_write = [](std::initializer_list<uint32_t> code, Decoder::Opcode opcode,
	                            uint32_t expected_auxiliary, bool secondary) {
		Fixture fixture;
		fixture.caller.instructions.pop_back();
		fixture.Append(0xae4u, code);
		const auto& write = fixture.caller.instructions.back();
		Check(write.opcode == opcode, "auxiliary clobber fixture opcode encoding changed");
		Check(secondary ? write.dst2.kind == Decoder::OperandKind::Sgpr && write.dst2.reg == 15u
		                : write.dst.kind == Decoder::OperandKind::Sgpr && write.dst.reg == 15u,
		      "auxiliary clobber fixture did not write the intended scalar destination");
		// Restore the code-pointer high word, leaving a possible s16 auxiliary-word clobber intact.
		fixture.Append(0xaecu, {0xbe8f0305u});
		fixture.Append(0xaf0u, {0xbe8e210eu});
		const auto loaded = fixture.Load();
		Check(loaded.plan.complete && loaded.failure.empty(), "proven code pointer was lost during auxiliary tracing");
		Check(loaded.plan.call_sites[0].auxiliary_sgpr == expected_auxiliary,
		      "auxiliary provenance survived a scalar pair clobber or lost a one-word write");
	};
	check_write({(0x35u << 26u) | (0x01u << 16u) | 15u, 0x00020501u},
	            Decoder::Opcode::V_CMP_LT_F32, UINT32_MAX, false);
	check_write({(0x35u << 26u) | (0x30fu << 16u) | (15u << 8u) | 1u, 0x00020501u},
	            Decoder::Opcode::V_ADD_I32, UINT32_MAX, true);
	check_write({0x878f0606u}, Decoder::Opcode::S_AND_B64, UINT32_MAX, false);
	check_write({0xbe8f1006u}, Decoder::Opcode::S_BCNT1_I32_B64, 16u, false);
	Fixture copied;
	copied.caller.instructions.pop_back();
	copied.Append(0xae4u, {0xbe910410u}); // Overlapping MOV64 snapshots s16:s17 before writing s17:s18.
	copied.Append(0xae8u, {0xbe8e210eu});
	const auto copied_result = copied.Load();
	Check(copied_result.plan.complete && copied_result.plan.call_sites[0].auxiliary_sgpr == 17u,
	      "overlapping scalar MOV64 did not preserve its source-before-destination provenance");
	for (const auto opcode: {Decoder::Opcode::UNKNOWN, Decoder::Opcode::UNSUPPORTED,
	                         Decoder::Opcode::S_ENDPGM, Decoder::Opcode::S_BRANCH,
	                         Decoder::Opcode::S_SETPC_B64, Decoder::Opcode::S_SWAPPC_B64}) {
		Fixture barrier;
		Decoder::Instruction inst;
		inst.pc = 0xae2u;
		inst.opcode = opcode;
		if (opcode == Decoder::Opcode::S_SWAPPC_B64) {
			inst.family = Decoder::Family::SOP1;
			inst.opcode_id = 0x21u;
			inst.raw[0] = 0xbe8e210eu;
		}
		barrier.caller.instructions.insert(barrier.caller.instructions.end() - 1u, inst);
		Rejected(barrier.Load(), "unknown writer or control-flow edge retained linear table provenance");
		Check(barrier.memory.requests.empty(), "unproven flow or writer reached guest-memory reader");
	}
}
} // namespace

int main(int argc, char** argv) {
	try {
		if (argc == 2 && std::string_view(argv[1]) == "--address-only") {
			TestNativeDescriptorAddressNormalization();
			std::puts("ExternalLibraryTests: native descriptor normalization passed");
			return 0;
		}
		if (argc == 2 && std::string_view(argv[1]) == "--auxiliary-only") {
			TestAuxiliaryPairWriteWidthsAndClobbers();
			std::puts("ExternalLibraryTests: auxiliary pair provenance passed");
			return 0;
		}
		TestNoCallAndConservativeOrigins();
		TestAliasedSnapshotAndRawContexts();
		TestIncompleteSnapshotsAndDescriptors();
		TestInvalidTargetsAndHardCaps();
		TestFullTableTailContextsAndSnapshotOnlyCompleteness();
		TestDependencyValidationAndExactIdentity();
		TestMappingRelocationAndCallerIdentity();
		TestMergedCodeDependenciesAndConflict();
		TestOnlyProvedTableOriginInputsMatter();
		TestNativeDescriptorAddressNormalization();
		TestAuxiliaryPairWriteWidthsAndClobbers();
		std::puts("ExternalLibraryTests: all eleven groups passed (mock mapped reads; no guest execution)");
		return 0;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "ExternalLibraryTests: failed: %s\n", error.what());
		return 1;
	}
}
