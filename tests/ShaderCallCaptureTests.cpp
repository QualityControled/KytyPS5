#include "graphics/shader/recompiler/ShaderCallDiagnostics.h"
#include "common/assert.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The pure decoder/capture tests do not link or initialize the guest runtime.
namespace Common {
int DbgExitHandler(const char*, int, std::string_view text) {
	throw std::runtime_error(std::string(text));
}
int DbgExitIfHandler(const char* expression, const char*, int) {
	throw std::runtime_error(expression);
}
int DbgNotImplementedHandler(const char* expression, const char*, int) {
	throw std::runtime_error(expression);
}
void DbgExit(int status) { std::exit(status); }
} // namespace Common

namespace {
namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;
namespace Diagnostics = Libs::Graphics::ShaderRecompiler::Diagnostics;

void Check(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

struct Fixture {
	Decoder::Program program;
	std::array<uint32_t, 64> user_data {};

	Fixture() {
		user_data[0] = 0x2000u;
		user_data[1] = 0x1234u;
		// Small instruction snippets from the failing shader, with their actual PCs.
		Append(0x6c8u, {0xf4081500u, 0xfa000140u}); // s84..87 = [s0:s1 + 320]
		Append(0xac4u, {0x8f6a846au});              // runtime table key << 4
		Append(0xac8u, {0xf428012au, 0xd4000000u}); // s4..7 = table record
		Append(0xad0u, {0xbf8cc07fu});
		Append(0xad4u, {0xbe8e0304u});
		Append(0xad8u, {0xbe8f0305u});
		Append(0xadcu, {0xbe900306u});
		Append(0xae0u, {0xbe910307u});
		Append(0xae4u, {0xbe8e210eu}); // call target and return pair both s14:s15
	}

	void Append(uint32_t pc, std::initializer_list<uint32_t> code) {
		Decoder::Instruction instruction;
		Decoder::DecodeInstruction(std::span<const uint32_t>(code.begin(), code.size()), 0u, instruction);
		instruction.pc = pc;
		program.instructions.push_back(instruction);
	}

	uint64_t DescriptorAddress() const {
		return (static_cast<uint64_t>(user_data[1]) << 32u | user_data[0]) + 320u;
	}
};

struct ReadRequest {
	uint64_t address = 0;
	size_t bytes = 0;
};

struct Memory {
	struct Region {
		uint64_t base;
		std::vector<uint32_t> words;
	};
	std::vector<Region> regions;
	std::vector<ReadRequest> requests;
	bool scribble_failed_read = false;

	void Add(uint64_t base, std::vector<uint32_t> words) {
		regions.push_back({base, std::move(words)});
	}

	static bool Read(void* context, uint64_t address, std::span<uint32_t> words) {
		auto& memory = *static_cast<Memory*>(context);
		memory.requests.push_back({address, words.size_bytes()});
		Check(words.size_bytes() <= Diagnostics::ReadChunkBytes, "capture exceeded its read chunk limit");
		for (const auto& region: memory.regions) {
			if (address < region.base || (address - region.base) % sizeof(uint32_t) != 0u) continue;
			const uint64_t index = (address - region.base) / sizeof(uint32_t);
			if (index > region.words.size() || words.size() > region.words.size() - index) continue;
			std::copy_n(region.words.begin() + static_cast<size_t>(index), words.size(), words.begin());
			return true;
		}
		if (memory.scribble_failed_read && !words.empty()) words.front() = 0xdeadbeefu;
		return false;
	}

	size_t CountReads(uint64_t base, size_t size) const {
		return static_cast<size_t>(std::count_if(requests.begin(), requests.end(), [&](const auto& request) {
			return request.address >= base && request.address - base < size;
		}));
	}
};

std::vector<uint32_t> Descriptor(uint64_t base, uint32_t stride, uint32_t records) {
	return {static_cast<uint32_t>(base),
	        static_cast<uint32_t>(base >> 32u) | ((stride & 0x3fffu) << 16u), records, 0u};
}

std::array<uint32_t, 4> Record(uint64_t target, uint32_t aux0 = 0u, uint32_t aux1 = 0u) {
	return {static_cast<uint32_t>(target), static_cast<uint32_t>(target >> 32u), aux0, aux1};
}

void AppendRecord(std::vector<uint32_t>& table, uint64_t target, uint32_t aux0 = 0u, uint32_t aux1 = 0u) {
	const auto record = Record(target, aux0, aux1);
	table.insert(table.end(), record.begin(), record.end());
}

Diagnostics::CallCapture Capture(const Fixture& fixture, Memory& memory, bool enabled = true) {
	return Diagnostics::CaptureCallTables(enabled, fixture.program, fixture.user_data, Memory::Read, &memory);
}

void TestActualAliasedTraceAndUnchangedDecoder() {
	Fixture fixture;
	const auto original_call = fixture.program.instructions.back();
	Check(original_call.opcode == Decoder::Opcode::UNSUPPORTED && original_call.opcode_id == 0x21u,
	      "diagnostics changed the unsupported SWAPPC decoder contract");
	const auto trace = Diagnostics::TraceCallTables(fixture.program, fixture.user_data);
	Check(trace.calls.size() == 1u && !trace.call_sites_truncated, "actual call was not found once");
	const auto& call = trace.calls.front();
	Check(call.rejection.empty(), "actual aliased target provenance was rejected");
	Check(call.call_pc == 0xae4u && call.raw_call == 0xbe8e210eu && call.target_sgpr == 14u &&
	          call.return_sgpr == 14u, "aliased call used the return destination as a new target");
	Check(call.record_load_pc == 0xac8u && call.descriptor_load_pc == 0x6c8u &&
	          call.descriptor_user_sgpr == 0u && call.descriptor_offset == 320 &&
	          call.record_offset == 0 && call.user_words == std::array<uint32_t, 2>{0x2000u, 0x1234u} &&
	          call.descriptor_address == fixture.DescriptorAddress(), "descriptor provenance or address is wrong");
	Check(fixture.program.instructions.back().opcode == original_call.opcode &&
	          fixture.program.instructions.back().raw[0] == original_call.raw[0],
	      "pure tracing mutated the call instruction");

	Fixture distinct;
	distinct.program.instructions.pop_back();
	distinct.Append(0xae4u, {0xbe90210eu}); // return s16:s17, target s14:s15
	const auto separate = Diagnostics::TraceCallTables(distinct.program, distinct.user_data);
	Check(separate.calls.size() == 1u && separate.calls.front().rejection.empty() &&
	          separate.calls.front().target_sgpr == 14u && separate.calls.front().return_sgpr == 16u &&
	          separate.calls.front().descriptor_address == fixture.DescriptorAddress(),
	      "distinct return pair changed target provenance");
}

void TestDisabledCaptureMakesNoReads() {
	Fixture fixture;
	Memory memory;
	const auto capture = Capture(fixture, memory, false);
	Check(!capture.enabled && memory.requests.empty() && capture.tables.empty() && capture.targets.empty(),
	      "disabled capture performed reads or returned snapshots");
	const auto no_reader = Diagnostics::CaptureCallTables(false, fixture.program, fixture.user_data, nullptr);
	Check(!no_reader.enabled && no_reader.tables.empty(), "disabled capture required a reader callback");
	const auto enabled_no_reader = Diagnostics::CaptureCallTables(true, fixture.program, fixture.user_data, nullptr);
	Check(enabled_no_reader.enabled && enabled_no_reader.tables.size() == 1u &&
	          enabled_no_reader.tables.front().read_failed &&
	          !enabled_no_reader.tables.front().status.empty() && enabled_no_reader.targets.empty(),
	      "missing reader was not explicitly reported without following targets");
}

void TestInitialUserRegisterBaseAndNullHandoff() {
	for (const uint32_t user_base: {2u, 8u}) {
		Fixture shifted;
		shifted.program.instructions.front().src0.reg = user_base;
		const auto user_words = std::span<const uint32_t>(shifted.user_data).first(2u);
		const auto trace = Diagnostics::TraceCallTables(shifted.program, user_words, user_base);
		Check(trace.calls.size() == 1u && trace.calls.front().rejection.empty() &&
		          trace.calls.front().descriptor_user_sgpr == user_base &&
		          trace.calls.front().descriptor_address == shifted.DescriptorAddress(),
		      "nonzero initial user register base indexed the wrong user words");
		Memory memory;
		const auto capture = Diagnostics::CaptureCallTables(true, shifted.program, user_words, Memory::Read, &memory,
		                                                  user_base);
		Check(capture.tables.size() == 1u && memory.requests.size() == 1u &&
		          memory.requests.front().address == shifted.DescriptorAddress(),
		      "capture did not carry the stage-specific user register base into tracing");
	}
	Fixture below_base;
	const auto user_words = std::span<const uint32_t>(below_base.user_data).first(2u);
	const auto uninitialized = Diagnostics::TraceCallTables(below_base.program, user_words, 2u);
	Check(uninitialized.calls.size() == 1u && !uninitialized.calls.front().rejection.empty(),
	      "register below the user-data base was treated as initialized");

	Fixture null_handoff;
	null_handoff.program.instructions.pop_back();
	null_handoff.Append(0xae4u, {0xbefd2106u});
	Check(null_handoff.program.instructions.back().opcode == Decoder::Opcode::S_SETPC_B64,
	      "existing null-destination fused handoff decoding changed");
	Check(Diagnostics::TraceCallTables(null_handoff.program, null_handoff.user_data).calls.empty(),
	      "null-destination handoff was mistaken for an external subroutine call");
}

void TestFullAddressDescriptorAndPartialTargetPrefix() {
	Fixture fixture;
	Memory memory;
	memory.scribble_failed_read = true;
	const uint64_t table_base = 0x345600004000ull;
	const uint64_t target = 0x123450001000ull;
	auto descriptor = Descriptor(table_base, 16u, 2u);
	descriptor[1] |= 0xc0000000u; // descriptor control bits must not leak into Base48/Stride
	memory.Add(fixture.DescriptorAddress(), descriptor);
	std::vector<uint32_t> table;
	AppendRecord(table, target, 0x11223344u, 0x55667788u);
	AppendRecord(table, 0u);
	memory.Add(table_base, table);
	std::vector<uint32_t> prefix(2u * Diagnostics::ReadChunkBytes / sizeof(uint32_t));
	for (size_t index = 0; index < prefix.size(); ++index) prefix[index] = 0xa5000000u + static_cast<uint32_t>(index);
	memory.Add(target, prefix);
	const auto capture = Capture(fixture, memory);
	Check(capture.enabled && capture.tables.size() == 1u && capture.targets.size() == 1u,
	      "valid table/target capture is missing");
	const auto& snapshot = capture.tables.front();
	Check(snapshot.descriptor_read && snapshot.table_base == table_base && snapshot.table_size == 32u &&
	          snapshot.words == table && !snapshot.read_failed && !snapshot.table_truncated,
	      "descriptor bit masking or stride-record extent is incorrect");
	const auto& target_snapshot = capture.targets.front();
	Check(target_snapshot.raw_address == target && target_snapshot.table_index == 0u &&
	          target_snapshot.record_index == 0u &&
	          target_snapshot.auxiliary_words == std::array<uint32_t, 2>{0x11223344u, 0x55667788u} &&
	          target_snapshot.words == prefix && target_snapshot.read_failed &&
	          !target_snapshot.prefix_capped && !target_snapshot.status.empty(),
	      "partial target prefix lost its successful chunks or included a failed chunk");
	Check(capture.zero_targets == 1u && memory.CountReads(target, Diagnostics::MaxTargetBytes) == 3u,
	      "target prefix did not stop on its first unreadable chunk");
	Check(capture.table_bytes_reserved == 32u && capture.table_read_bytes_requested == 32u &&
	          capture.target_read_bytes_requested == 3u * Diagnostics::ReadChunkBytes &&
	          capture.descriptor_read_requests == 1u, "read accounting omitted a failed target request");
}

void TestDescriptorFailuresAndRejectedOrigins() {
	Fixture fixture;
	Memory unreadable;
	const auto missing = Capture(fixture, unreadable);
	Check(missing.tables.size() == 1u && !missing.tables.front().descriptor_read &&
	          missing.tables.front().read_failed && !missing.tables.front().status.empty() &&
	          missing.targets.empty() && unreadable.requests.size() == 1u,
	      "unreadable descriptor was followed or not reported");

	Fixture modified;
	Decoder::Instruction rewrite;
	rewrite.pc = 0x6c4u;
	rewrite.family = Decoder::Family::SOP1;
	rewrite.opcode = Decoder::Opcode::S_MOV_B32;
	rewrite.dst.kind = Decoder::OperandKind::Sgpr;
	rewrite.dst.reg = 0u;
	rewrite.src0.kind = Decoder::OperandKind::IntegerInlineConstant;
	rewrite.src0.value = 7u;
	rewrite.src_count = 1u;
	modified.program.instructions.insert(modified.program.instructions.begin(), rewrite);
	const auto rejected = Diagnostics::TraceCallTables(modified.program, modified.user_data);
	Check(rejected.calls.size() == 1u && !rejected.calls.front().rejection.empty(),
	      "rewritten user SGPR was treated as its initial user value");
	Memory forbidden;
	const auto rejected_capture = Capture(modified, forbidden);
	Check(forbidden.requests.empty() && rejected_capture.targets.empty(),
	      "unproved descriptor provenance caused a memory read");

	Fixture missing_user;
	const auto short_user = Diagnostics::TraceCallTables(missing_user.program, std::span<const uint32_t>{});
	Check(short_user.calls.size() == 1u && !short_user.calls.front().rejection.empty(),
	      "missing user data was accepted as a descriptor pointer");
}

Decoder::Instruction ScalarMove(uint32_t pc, uint32_t destination, uint32_t source) {
	Decoder::Instruction instruction;
	instruction.pc = pc;
	instruction.family = Decoder::Family::SOP1;
	instruction.opcode = Decoder::Opcode::S_MOV_B32;
	instruction.dst.kind = Decoder::OperandKind::Sgpr;
	instruction.dst.reg = destination;
	instruction.src0.kind = Decoder::OperandKind::Sgpr;
	instruction.src0.reg = source;
	instruction.src_count = 1u;
	return instruction;
}

void CheckRejectedWithoutReads(const Fixture& fixture, const char* message) {
	const auto trace = Diagnostics::TraceCallTables(fixture.program, fixture.user_data);
	Check(trace.calls.size() == 1u && !trace.calls.front().rejection.empty(), message);
	Memory forbidden;
	const auto capture = Capture(fixture, forbidden);
	Check(forbidden.requests.empty() && capture.targets.empty(),
	      "rejected trace invoked the reader or produced target snapshots");
}

void TestStraightLineMovesAndConservativeTraceRejection() {
	Fixture chain;
	chain.program.instructions[4].src0.reg = 20u;
	chain.program.instructions[5].src0.reg = 21u;
	chain.program.instructions.insert(chain.program.instructions.begin() + 4,
	                                  ScalarMove(0xad1u, 20u, 4u));
	chain.program.instructions.insert(chain.program.instructions.begin() + 5,
	                                  ScalarMove(0xad2u, 21u, 5u));
	const auto traced_chain = Diagnostics::TraceCallTables(chain.program, chain.user_data);
	Check(traced_chain.calls.size() == 1u && traced_chain.calls.front().rejection.empty() &&
	          traced_chain.calls.front().record_load_pc == 0xac8u,
	      "straight-line target MOV chain lost its table origin");

	Fixture split_target;
	split_target.program.instructions[5].src0.reg = 6u; // high half is auxiliary data, not pointer high
	CheckRejectedWithoutReads(split_target, "split target pair was treated as a function pointer");

	Fixture overwritten_descriptor;
	overwritten_descriptor.program.instructions.insert(overwritten_descriptor.program.instructions.begin() + 1,
	                                                    ScalarMove(0x6d0u, 85u, 20u));
	CheckRejectedWithoutReads(overwritten_descriptor, "partially overwritten descriptor was accepted");

	Fixture unsupported_offset;
	unsupported_offset.program.instructions[2].offset = 16u;
	CheckRejectedWithoutReads(unsupported_offset, "nonzero record offset invented a table scanning origin");

	Decoder::Instruction branch;
	branch.pc = 0xad2u;
	branch.family = Decoder::Family::SOPP;
	branch.opcode = Decoder::Opcode::S_CBRANCH_SCC1;
	branch.branch_target = 0xae4u;
	Fixture branch_target;
	branch_target.program.instructions.insert(branch_target.program.instructions.begin() + 4, branch);
	CheckRejectedWithoutReads(branch_target, "target copies across a control-flow branch were accepted");

	Fixture branch_descriptor;
	branch.pc = 0x700u;
	branch.branch_target = 0xac4u;
	branch_descriptor.program.instructions.insert(branch_descriptor.program.instructions.begin() + 1, branch);
	const auto unique_descriptor = Diagnostics::TraceCallTables(branch_descriptor.program, branch_descriptor.user_data);
	Check(unique_descriptor.calls.size() == 1u && unique_descriptor.calls.front().rejection.empty(),
	      "unique descriptor load from unchanged user SGPRs was rejected across a branch");

	Fixture rewritten_after_load;
	rewritten_after_load.program.instructions.insert(rewritten_after_load.program.instructions.begin() + 1,
	                                                ScalarMove(0x6d0u, 1u, 20u));
	CheckRejectedWithoutReads(rewritten_after_load,
	                         "globally rewritten user SGPR escaped conservative descriptor rejection");
}

void TestInvalidAndDuplicateTargets() {
	Fixture fixture;
	Memory memory;
	const uint64_t table_base = 0x4000u;
	const uint64_t target = 0x20000u;
	std::vector<uint32_t> table;
	AppendRecord(table, 0u);
	AppendRecord(table, target + 1u);
	AppendRecord(table, 0x1000000000000ull);
	AppendRecord(table, target, 9u, 10u);
	AppendRecord(table, target, 11u, 12u);
	memory.Add(fixture.DescriptorAddress(), Descriptor(table_base, 16u, 5u));
	memory.Add(table_base, table);
	memory.Add(target, std::vector<uint32_t>(Diagnostics::ReadChunkBytes / sizeof(uint32_t), 0xabcd1234u));
	const auto capture = Capture(fixture, memory);
	Check(capture.zero_targets == 1u && capture.misaligned_targets == 1u &&
	          capture.outside_address_space_targets == 1u && capture.duplicate_targets == 1u &&
	          capture.targets.size() == 1u, "invalid targets or duplicate targets were not classified");
	Check(capture.targets.front().raw_address == target && capture.targets.front().record_index == 3u &&
	          capture.targets.front().auxiliary_words == std::array<uint32_t, 2>{9u, 10u},
	      "duplicate target changed its first record provenance");
	for (const auto& request: memory.requests) {
		Check(request.address != target + 1u && request.address < 0x1000000000000ull,
		      "invalid target address reached the reader callback");
	}
}

void TestGuestAddressBoundaryStopsReads() {
	Fixture fixture;
	Memory memory;
	const uint64_t table_base = 0x4000u;
	const uint64_t last_page = 0xfffffffff000ull;
	std::vector<uint32_t> table;
	AppendRecord(table, last_page);
	memory.Add(fixture.DescriptorAddress(), Descriptor(table_base, 0u, 16u));
	memory.Add(table_base, table);
	const std::vector<uint32_t> prefix(Diagnostics::ReadChunkBytes / sizeof(uint32_t), 0x13572468u);
	memory.Add(last_page, prefix);
	const auto capture = Capture(fixture, memory);
	Check(capture.targets.size() == 1u && capture.targets.front().words == prefix &&
	          capture.targets.front().read_failed && !capture.targets.front().prefix_capped &&
	          memory.CountReads(last_page, 2u * Diagnostics::ReadChunkBytes) == 1u,
	      "target prefix crossed the 48-bit guest address boundary");
	for (const auto& request: memory.requests) {
		Check(request.address <= 0xffffffffffffull &&
		          request.bytes - 1u <= 0xffffffffffffull - request.address,
		      "out-of-address-space read reached the callback");
	}
}

void TestTableAndTargetHardCaps() {
	Fixture fixture;
	Memory table_memory;
	const uint64_t table_base = 0x4000u;
	constexpr uint32_t stride = 0x3fffu;
	constexpr uint32_t records = std::numeric_limits<uint32_t>::max();
	table_memory.Add(fixture.DescriptorAddress(), Descriptor(table_base, stride, records));
	table_memory.Add(table_base, std::vector<uint32_t>(Diagnostics::MaxTableBytes / sizeof(uint32_t), 0u));
	const auto table_capture = Capture(fixture, table_memory);
	Check(table_capture.tables.size() == 1u &&
	          table_capture.tables.front().table_size == static_cast<uint64_t>(stride) * records &&
	          table_capture.tables.front().words.size() * sizeof(uint32_t) == Diagnostics::MaxTableBytes &&
	          table_capture.tables.front().table_truncated && table_capture.table_budget_exhausted,
	      "large stride-record extent overflowed or exceeded the table budget");
	Check(table_memory.CountReads(table_base, Diagnostics::MaxTableBytes) ==
	          Diagnostics::MaxTableBytes / Diagnostics::ReadChunkBytes,
	      "table cap was not captured in bounded chunks");

	Memory target_memory;
	const uint64_t target = 0x20000u;
	std::vector<uint32_t> table;
	AppendRecord(table, target);
	target_memory.Add(fixture.DescriptorAddress(), Descriptor(table_base, 0u, 16u)); // stride 0 counts bytes
	target_memory.Add(table_base, table);
	std::vector<uint32_t> prefix(Diagnostics::MaxTargetBytes / sizeof(uint32_t), 0xfeedc0deu);
	target_memory.Add(target, prefix);
	const auto target_capture = Capture(fixture, target_memory);
	Check(target_capture.targets.size() == 1u && target_capture.targets.front().words == prefix &&
	          target_capture.targets.front().prefix_capped && !target_capture.targets.front().read_failed &&
	          !target_capture.targets.front().status.empty(), "target prefix cap was not explicitly reported");
	Check(target_memory.CountReads(target, Diagnostics::MaxTargetBytes + Diagnostics::ReadChunkBytes) ==
	          Diagnostics::MaxTargetBytes / Diagnostics::ReadChunkBytes,
	      "capture read beyond the target prefix cap");
}

void TestPartialTableAndAggregateBudget() {
	Fixture fixture;
	Memory partial_memory;
	const uint64_t table_base = 0x4000u;
	partial_memory.scribble_failed_read = true;
	partial_memory.Add(fixture.DescriptorAddress(), Descriptor(table_base, 0u, 2u * Diagnostics::ReadChunkBytes));
	const std::vector<uint32_t> first_chunk(Diagnostics::ReadChunkBytes / sizeof(uint32_t), 0u);
	partial_memory.Add(table_base, first_chunk);
	const auto partial = Capture(fixture, partial_memory);
	Check(partial.tables.size() == 1u && partial.tables.front().words == first_chunk &&
	          partial.tables.front().read_failed && !partial.tables.front().status.empty() &&
	          partial_memory.CountReads(table_base, 2u * Diagnostics::ReadChunkBytes) == 2u,
	      "partial table included a failed chunk or omitted its read failure");

	// Two independent descriptor register groups share a fixed aggregate byte budget.
	Fixture second;
	second.program.instructions[0].dst.reg = 88u;
	second.program.instructions[0].offset = 336u;
	second.program.instructions[2].src0.reg = 88u;
	second.program.instructions[2].dst.reg = 8u;
	for (size_t index = 4; index != 8; ++index) {
		second.program.instructions[index].dst.reg += 4u;
		second.program.instructions[index].src0.reg += 4u;
	}
	second.program.instructions.pop_back();
	second.Append(0xae4u, {0xbe922112u}); // second aliased pair s18:s19
	for (auto instruction: second.program.instructions) {
		instruction.pc += 0x2000u;
		fixture.program.instructions.push_back(instruction);
	}
	const auto trace = Diagnostics::TraceCallTables(fixture.program, fixture.user_data);
	Check(trace.calls.size() == 2u && trace.calls[0].rejection.empty() && trace.calls[1].rejection.empty(),
	      "independent descriptor register groups could not both be traced");
	Memory aggregate_memory;
	constexpr size_t first_size = Diagnostics::MaxTableBytes * 3u / 4u;
	constexpr uint64_t second_base = 0x4000000u;
	aggregate_memory.Add(fixture.DescriptorAddress(), Descriptor(table_base, 0u, first_size));
	aggregate_memory.Add(fixture.DescriptorAddress() + 16u, Descriptor(second_base, 0u, first_size));
	aggregate_memory.Add(table_base, std::vector<uint32_t>(first_size / sizeof(uint32_t), 0u));
	aggregate_memory.Add(second_base, std::vector<uint32_t>(first_size / sizeof(uint32_t), 0u));
	const auto aggregate = Capture(fixture, aggregate_memory);
	Check(aggregate.tables.size() == 2u && aggregate.tables[0].words.size() * sizeof(uint32_t) == first_size &&
	          aggregate.tables[1].words.size() * sizeof(uint32_t) == Diagnostics::MaxTableBytes - first_size &&
	          !aggregate.tables[0].table_truncated && aggregate.tables[1].table_truncated &&
	          aggregate.table_budget_exhausted,
	      "per-table reads escaped the aggregate table byte budget");
	const size_t total_table_bytes = aggregate.tables[0].words.size() * sizeof(uint32_t) +
	                                 aggregate.tables[1].words.size() * sizeof(uint32_t);
	Check(total_table_bytes == Diagnostics::MaxTableBytes, "aggregate table cap was not exact");

	Memory failed_last_chunk;
	failed_last_chunk.scribble_failed_read = true;
	failed_last_chunk.Add(fixture.DescriptorAddress(), Descriptor(table_base, 0u, Diagnostics::MaxTableBytes));
	failed_last_chunk.Add(fixture.DescriptorAddress() + 16u, Descriptor(second_base, 0u, first_size));
	const size_t retained_size = Diagnostics::MaxTableBytes - Diagnostics::ReadChunkBytes;
	failed_last_chunk.Add(table_base, std::vector<uint32_t>(retained_size / sizeof(uint32_t), 0u));
	failed_last_chunk.Add(second_base, std::vector<uint32_t>(first_size / sizeof(uint32_t), 0u));
	const auto failed_budget = Capture(fixture, failed_last_chunk);
	Check(failed_budget.tables.size() == 2u && failed_budget.tables[0].read_failed &&
	          failed_budget.tables[0].words.size() * sizeof(uint32_t) == retained_size &&
	          failed_budget.tables[1].descriptor_read && failed_budget.tables[1].words.empty() &&
	          failed_budget.tables[1].table_truncated && failed_budget.table_budget_exhausted,
	      "failed last chunk made the reserved table budget available to a later call");
	size_t requested_table_bytes = 0u;
	for (const auto& request: failed_last_chunk.requests) {
		if ((request.address >= table_base && request.address - table_base < Diagnostics::MaxTableBytes) ||
		    (request.address >= second_base && request.address - second_base < first_size)) {
			requested_table_bytes += request.bytes;
		}
	}
	Check(requested_table_bytes == Diagnostics::MaxTableBytes &&
	          failed_budget.table_bytes_reserved == Diagnostics::MaxTableBytes &&
	          failed_budget.table_read_bytes_requested == requested_table_bytes &&
	          failed_budget.descriptor_read_requests == 2u,
	      "aggregate read budget counted retained words instead of attempted reads");
}

void TestDistinctTargetAndCallSiteCaps() {
	Fixture fixture;
	Memory memory;
	const uint64_t table_base = 0x4000u;
	std::vector<uint32_t> table;
	for (size_t index = 0; index <= Diagnostics::MaxTargets; ++index) {
		const uint64_t target = 0x100000u + index * 0x10000u;
		AppendRecord(table, target);
		memory.Add(target, std::vector<uint32_t>(Diagnostics::ReadChunkBytes / sizeof(uint32_t),
		                                      static_cast<uint32_t>(index)));
	}
	memory.Add(fixture.DescriptorAddress(), Descriptor(table_base, 16u, Diagnostics::MaxTargets + 1u));
	memory.Add(table_base, table);
	const auto capture = Capture(fixture, memory);
	Check(capture.targets.size() == Diagnostics::MaxTargets && capture.target_limit_reached &&
	          capture.target_budget_exhausted &&
	          capture.target_bytes_reserved == Diagnostics::MaxAggregateTargetBytes &&
	          capture.target_read_bytes_requested == 2u * Diagnostics::ReadChunkBytes * Diagnostics::MaxTargets,
	      "distinct target count was not capped or truncation was hidden");
	Check(std::all_of(capture.targets.begin(), capture.targets.end(), [](const auto& target) {
		      return target.read_failed && !target.prefix_capped;
	      }), "failed target attempts escaped the target count or reservation budget");
	const uint64_t excluded_target = 0x100000u + Diagnostics::MaxTargets * 0x10000u;
	Check(memory.CountReads(excluded_target, Diagnostics::MaxTargetBytes) == 0u,
	      "target beyond the hard count cap was read");

	Fixture many_calls;
	const auto block = many_calls.program.instructions;
	many_calls.program.instructions.clear();
	for (size_t index = 0; index <= Diagnostics::MaxCallSites; ++index) {
		for (auto instruction: block) {
			instruction.pc += static_cast<uint32_t>(index * 0x2000u);
			many_calls.program.instructions.push_back(instruction);
		}
	}
	const auto trace = Diagnostics::TraceCallTables(many_calls.program, many_calls.user_data);
	Check(trace.calls.size() == Diagnostics::MaxCallSites && trace.call_sites_truncated,
	      "call site scan did not enforce or report its hard cap");
}

void TestCompleteObservedTableTailAndDuplicateContexts() {
	Fixture fixture;
	Memory memory;
	constexpr uint32_t records = 10500u;
	constexpr size_t table_bytes = 168000u;
	constexpr uint64_t table_base = 0x8000000u;
	constexpr uint64_t first_target = 0x133add0e00ull;
	constexpr uint64_t tail_target = 0x133bcd2c00ull;
	constexpr uint64_t last_target = 0x133bcd3500ull;
	std::vector<uint32_t> table;
	table.reserve(table_bytes / sizeof(uint32_t));
	for (uint32_t record = 0u; record < records; ++record) {
		const uint64_t target = record == 4096u ? tail_target
		                        : record == records - 1u ? last_target : first_target;
		AppendRecord(table, target, 0x50000000u + record * 16u, 0x10u);
	}
	// Auxiliary words are data, and must never be masked or interpreted as code pointers.
	table[4096u * 4u + 2u] = 0x77778888u;
	table[4096u * 4u + 3u] = 0x12u;
	table[(records - 1u) * 4u + 2u] = 0xfedcba98u;
	table[(records - 1u) * 4u + 3u] = 0x76543210u;
	memory.Add(fixture.DescriptorAddress(), Descriptor(table_base, 16u, records));
	memory.Add(table_base, table);
	for (const auto target: {first_target, tail_target, last_target}) {
		memory.Add(target, std::vector<uint32_t>(Diagnostics::ReadChunkBytes / sizeof(uint32_t), 0xbf810000u));
	}
	const auto capture = Capture(fixture, memory);
	Check(capture.tables.size() == 1u && capture.tables.front().table_size == table_bytes &&
	          capture.tables.front().words == table && !capture.tables.front().table_truncated &&
	          !capture.tables.front().read_failed && !capture.table_budget_exhausted &&
	          capture.table_bytes_reserved == table_bytes && capture.table_read_bytes_requested == table_bytes,
	      "observed-size table or its tail was truncated despite sufficient bounded budget");
	Check(capture.targets.size() == 3u && capture.duplicate_targets == records - 3u &&
	          !capture.target_limit_reached && !capture.target_budget_exhausted &&
	          capture.target_bytes_reserved == 3u * Diagnostics::MaxTargetBytes,
	      "duplicate code targets consumed capture slots or erased unique table-tail targets");
	Check(capture.targets[1].raw_address == tail_target && capture.targets[1].record_index == 4096u &&
	          capture.targets[1].auxiliary_words == std::array<uint32_t, 2>{0x77778888u, 0x12u} &&
	          capture.targets[2].raw_address == last_target && capture.targets[2].record_index == records - 1u &&
	          capture.targets[2].auxiliary_words == std::array<uint32_t, 2>{0xfedcba98u, 0x76543210u},
	      "unique tail target provenance or raw auxiliary words changed");
	for (const auto& request: memory.requests) {
		Check(request.address != 0x1277778888ull && request.address < 0x1000000000000ull,
		      "capture dereferenced auxiliary context words or an invalid address");
	}
}

void TestAggregateTargetReadBudget() {
	Fixture fixture;
	Memory memory;
	constexpr uint64_t table_base = 0x8000000u;
	constexpr uint64_t target_base = 0x10000000u;
	std::vector<uint32_t> table;
	for (size_t index = 0u; index <= Diagnostics::MaxTargets; ++index) {
		AppendRecord(table, target_base + index * Diagnostics::MaxTargetBytes);
	}
	memory.Add(fixture.DescriptorAddress(), Descriptor(table_base, 16u, Diagnostics::MaxTargets + 1u));
	memory.Add(table_base, table);
	memory.Add(target_base, std::vector<uint32_t>(Diagnostics::MaxAggregateTargetBytes / sizeof(uint32_t),
	                                             0x12345678u));
	const auto capture = Capture(fixture, memory);
	Check(capture.targets.size() == Diagnostics::MaxTargets && capture.target_limit_reached &&
	          capture.target_budget_exhausted &&
	          capture.target_bytes_reserved == Diagnostics::MaxAggregateTargetBytes &&
	          capture.target_read_bytes_requested == Diagnostics::MaxAggregateTargetBytes,
	      "successful target prefixes exceeded or failed to report aggregate read budget");
	Check(std::all_of(capture.targets.begin(), capture.targets.end(), [](const auto& target) {
		      return !target.read_failed && target.prefix_capped &&
		             target.words.size() * sizeof(uint32_t) == Diagnostics::MaxTargetBytes;
	      }), "per-target prefixes did not obey their individual cap within the aggregate budget");
	size_t requested_target_bytes = 0u;
	for (const auto& request: memory.requests) {
		if (request.address >= target_base && request.address - target_base < Diagnostics::MaxAggregateTargetBytes) {
			requested_target_bytes += request.bytes;
		}
	}
	Check(requested_target_bytes == Diagnostics::MaxAggregateTargetBytes &&
	          memory.CountReads(target_base + Diagnostics::MaxAggregateTargetBytes,
	                            Diagnostics::MaxTargetBytes) == 0u,
	      "callback requests escaped aggregate target bounds");
}

void TestObservedDistinctLibraryFitsCaptureCaps() {
	Fixture fixture;
	Memory memory;
	constexpr uint32_t records = 10644u;
	constexpr uint32_t unique_targets = 1684u;
	constexpr uint64_t table_base = 0x8000000u;
	constexpr uint64_t target_base = 0x1350000000ull;
	std::vector<uint32_t> table;
	table.reserve(records * 4u);
	for (uint32_t record = 0; record < records; ++record) {
		AppendRecord(table, target_base + (record % unique_targets) * Diagnostics::MaxTargetBytes,
		             0x2870018u + record * 24u, 0x10u);
	}
	memory.Add(fixture.DescriptorAddress(), Descriptor(table_base, 16u, records));
	memory.Add(table_base, table);
	for (uint32_t target = 0; target < unique_targets; ++target) {
		memory.Add(target_base + target * Diagnostics::MaxTargetBytes,
		           std::vector<uint32_t>(Diagnostics::ReadChunkBytes / sizeof(uint32_t), 0xbe80200eu));
	}
	const auto capture = Capture(fixture, memory);
	Check(capture.tables.size() == 1u && capture.tables.front().words == table &&
	          !capture.tables.front().table_truncated && !capture.table_budget_exhausted,
	      "observed full library table was truncated despite sufficient table budget");
	Check(capture.targets.size() == unique_targets && !capture.target_limit_reached &&
	          !capture.target_budget_exhausted && capture.duplicate_targets == records - unique_targets &&
	          capture.target_bytes_reserved == unique_targets * Diagnostics::MaxTargetBytes &&
	          capture.target_read_bytes_requested == unique_targets * 2u * Diagnostics::ReadChunkBytes,
	      "observed distinct library exceeded capture count or aggregate reservation budget");
	Check(capture.targets.back().raw_address == target_base + (unique_targets - 1u) * Diagnostics::MaxTargetBytes &&
	          capture.targets.back().record_index == unique_targets - 1u &&
	          std::all_of(capture.targets.begin(), capture.targets.end(), [](const auto& target) {
		          return target.read_failed && !target.prefix_capped &&
		                 target.words.size() * sizeof(uint32_t) == Diagnostics::ReadChunkBytes;
	          }),
	      "observed target tail or failed-prefix labels changed");
}

void TestCapturedCallerFromFile(const char* path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	Check(static_cast<bool>(file), "cannot open supplied native shader capture");
	const auto bytes = file.tellg();
	Check(bytes > 0 && bytes <= Diagnostics::MaxTargetBytes && bytes % sizeof(uint32_t) == 0,
	      "supplied shader capture must be nonempty, DWORD aligned, and at most 64 KiB");
	std::vector<uint32_t> code(static_cast<size_t>(bytes) / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(bytes));
	Check(static_cast<bool>(file), "cannot read complete supplied native shader capture");
	Decoder::Program program;
	Decoder::DecodeProgram(code, program);
	std::array<uint32_t, 64> user_data {};
	user_data[0] = 0x2000u;
	user_data[1] = 0x1234u;
	const auto trace = Diagnostics::TraceCallTables(program, user_data);
	Check(trace.calls.size() == 1u && !trace.call_sites_truncated && trace.calls.front().rejection.empty(),
	      "full captured caller was rejected by global descriptor/user-data writer constraints");
	const auto& call = trace.calls.front();
	Check(call.call_pc == 0xae4u && call.raw_call == 0xbe8e210eu &&
	          call.target_sgpr == 14u && call.return_sgpr == 14u &&
	          call.record_load_pc == 0xac8u && call.descriptor_load_pc == 0x6c8u &&
	          call.descriptor_user_sgpr == 0u && call.descriptor_offset == 320 &&
	          call.descriptor_address == 0x123400002140ull,
	      "full captured caller did not identify the observed external table origin");
	const auto decoded_call = std::find_if(program.instructions.begin(), program.instructions.end(),
	                                     [](const auto& instruction) { return instruction.pc == 0xae4u; });
	Check(decoded_call != program.instructions.end() && decoded_call->opcode == Decoder::Opcode::UNSUPPORTED,
	      "diagnostic tracing accidentally enabled execution of the actual call");
	std::puts("ShaderCallCaptureTests: full captured caller origin passed (no guest memory reads)");
}

} // namespace

int main(int argc, char** argv) {
	try {
		Check(argc == 1 || (argc == 3 && std::string_view(argv[1]) == "--trace-gt7-file"),
		      "usage: ShaderCallCaptureTests [--trace-gt7-file captured-native-shader.bin]");
		TestActualAliasedTraceAndUnchangedDecoder();
		TestDisabledCaptureMakesNoReads();
		TestInitialUserRegisterBaseAndNullHandoff();
		TestFullAddressDescriptorAndPartialTargetPrefix();
		TestDescriptorFailuresAndRejectedOrigins();
		TestStraightLineMovesAndConservativeTraceRejection();
		TestInvalidAndDuplicateTargets();
		TestGuestAddressBoundaryStopsReads();
		TestTableAndTargetHardCaps();
		TestPartialTableAndAggregateBudget();
		TestDistinctTargetAndCallSiteCaps();
		TestCompleteObservedTableTailAndDuplicateContexts();
		TestAggregateTargetReadBudget();
		TestObservedDistinctLibraryFitsCaptureCaps();
		if (argc == 3) TestCapturedCallerFromFile(argv[2]);
		std::puts("ShaderCallCaptureTests: all cases passed");
		return 0;
	} catch (const std::exception& exception) {
		std::fprintf(stderr, "ShaderCallCaptureTests: failed: %s\n", exception.what());
		return 1;
	}
}
