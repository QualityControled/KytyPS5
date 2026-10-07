#include "graphics/shader/recompiler/ShaderCallDiagnostics.h"

#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::Diagnostics {
namespace {

using Decoder::Instruction;
using Decoder::Opcode;
using Decoder::OperandKind;
constexpr uint64_t AddressMask   = 0x0000ffffffffffffull;
constexpr size_t   NoInstruction = std::numeric_limits<size_t>::max();

bool IsSwappc(const Instruction& inst) {
	return inst.family == Decoder::Family::SOP1 && inst.opcode_id == 0x21u &&
	       ((inst.raw[0] >> 23u) & 0x1ffu) == 0x17du;
}

bool IsUnknownWriter(const Instruction& inst) {
	return (inst.opcode == Opcode::UNKNOWN || inst.opcode == Opcode::UNSUPPORTED) &&
	       !IsSwappc(inst);
}

uint32_t ScalarWriteWords(const Instruction& inst) {
	if (inst.family == Decoder::Family::VOPC ||
	    magic_enum::enum_name(inst.opcode).starts_with("V_CMP")) {
		return 2u;
	}
	const auto name = magic_enum::enum_name(inst.opcode);
	const bool count_to_i32 =
	    name.starts_with("S_BCNT") || name.starts_with("S_FF") || name.starts_with("S_FLBIT");
	return !count_to_i32 &&
	               (name.ends_with("_B64") || name.ends_with("_U64") || name.ends_with("_I64"))
	           ? 2u
			   : std::max(inst.data_dwords, 1u);
}

bool WritesScalar(const Instruction& inst, uint32_t reg) {
	if (IsSwappc(inst)) {
		const auto dst = (inst.raw[0] >> 16u) & 0x7fu;
		return dst != 125u && reg >= dst && reg - dst < 2u;
	}
	const auto writes = [&](const Decoder::Operand& dst, uint32_t words) {
		return dst.kind == OperandKind::Sgpr && reg >= dst.reg && reg - dst.reg < words;
	};
	return writes(inst.dst, ScalarWriteWords(inst)) || writes(inst.dst2, 2u);
}

bool IsFlowBarrier(const Instruction& inst) {
	return Decoder::IsDirectBranch(inst.opcode) || inst.opcode == Opcode::S_SETPC_B64 ||
	       inst.opcode == Opcode::S_ENDPGM || IsSwappc(inst);
}

struct WordOrigin {
	size_t   instruction = NoInstruction;
	uint32_t component   = 0;
};

WordOrigin TraceRecordWord(const Decoder::Program& program, size_t before, uint32_t reg) {
	for (uint32_t depth = 0; depth < 32u; depth++) {
		size_t writer = NoInstruction;
		for (size_t i = before; i > 0u; --i) {
			const auto& inst = program.instructions[i - 1u];
			if (IsFlowBarrier(inst) || IsUnknownWriter(inst)) return {};
			if (WritesScalar(inst, reg)) {
				writer = i - 1u;
				break;
			}
		}
		if (writer == NoInstruction) return {};
		const auto& inst = program.instructions[writer];
		if (inst.opcode == Opcode::S_BUFFER_LOAD_DWORDX4 && inst.dst.kind == OperandKind::Sgpr &&
		    inst.data_dwords == 4u) {
			return {writer, reg - inst.dst.reg};
		}
		if ((inst.opcode != Opcode::S_MOV_B32 && inst.opcode != Opcode::S_MOV_B64) ||
		    inst.src0.kind != OperandKind::Sgpr || inst.dst.kind != OperandKind::Sgpr) {
			return {};
		}
		reg = inst.src0.reg + reg - inst.dst.reg;
		if (reg > 105u) return {};
		before = writer;
	}
	return {};
}

bool IsZeroOffset(const Decoder::Operand& operand) {
	return operand.kind == OperandKind::Null ||
	       (operand.kind == OperandKind::IntegerInlineConstant && operand.signed_val == 0) ||
	       (operand.kind == OperandKind::LiteralConstant && operand.value == 0u);
}

bool AddOffset(uint64_t base, int32_t offset, uint64_t& address) {
	base &= AddressMask & ~uint64_t {3};
	const auto signed_offset = static_cast<int64_t>(offset) & ~int64_t {3};
	if (signed_offset < 0) {
		const auto magnitude = static_cast<uint64_t>(-signed_offset);
		if (magnitude > base) return false;
		address = base - magnitude;
	} else {
		const auto magnitude = static_cast<uint64_t>(signed_offset);
		if (magnitude > AddressMask - base) return false;
		address = base + magnitude;
	}
	return sizeof(uint32_t) * 4u - 1u <= AddressMask - address;
}

CallTableTrace TraceCall(const Decoder::Program& program, size_t call_index,
                         std::span<const uint32_t> user_data, uint32_t user_data_base) {
	const auto&    call = program.instructions[call_index];
	CallTableTrace trace;
	trace.call_pc     = call.pc;
	trace.raw_call    = call.raw[0];
	trace.target_sgpr = call.raw[0] & 0xffu;
	trace.return_sgpr = (call.raw[0] >> 16u) & 0x7fu;
	if (trace.target_sgpr > 104u) {
		trace.rejection = "call target is not a plain SGPR pair";
		return trace;
	}
	const auto low  = TraceRecordWord(program, call_index, trace.target_sgpr);
	const auto high = TraceRecordWord(program, call_index, trace.target_sgpr + 1u);
	if (low.instruction == NoInstruction || low.instruction != high.instruction ||
	    low.component != 0u || high.component != 1u) {
		trace.rejection = "target pair lacks one straight-line buffer-record origin";
		return trace;
	}
	const auto& record   = program.instructions[low.instruction];
	trace.record_load_pc = record.pc;
	trace.record_offset  = static_cast<int32_t>(record.offset);
	if (record.src0.kind != OperandKind::Sgpr || record.src0.reg > 102u ||
	    trace.record_offset != 0) {
		trace.rejection = "record descriptor is not a SGPR quartet or record offset is nonzero";
		return trace;
	}
	const auto descriptor_reg    = record.src0.reg;
	size_t     descriptor_writer = NoInstruction;
	for (size_t i = 0; i < program.instructions.size(); ++i) {
		const auto& inst = program.instructions[i];
		if (IsUnknownWriter(inst)) {
			trace.rejection = "unknown instruction prevents descriptor provenance";
			return trace;
		}
		for (uint32_t word = 0; word < 4u; ++word) {
			if (!WritesScalar(inst, descriptor_reg + word)) continue;
			if (i >= low.instruction || inst.opcode != Opcode::S_LOAD_DWORDX4 ||
			    inst.dst.kind != OperandKind::Sgpr || inst.dst.reg != descriptor_reg ||
			    inst.data_dwords != 4u ||
			    (descriptor_writer != NoInstruction && descriptor_writer != i)) {
				trace.rejection = "descriptor quartet has split, partial, or competing writers";
				return trace;
			}
			descriptor_writer = i;
		}
	}
	if (descriptor_writer == NoInstruction) {
		trace.rejection = "descriptor quartet lacks a user-table load";
		return trace;
	}
	const auto& load         = program.instructions[descriptor_writer];
	trace.descriptor_load_pc = load.pc;
	trace.descriptor_offset  = static_cast<int32_t>(load.offset);
	if (load.src0.kind != OperandKind::Sgpr || load.src0.reg > 104u || !IsZeroOffset(load.src1)) {
		trace.rejection = "descriptor address uses a dynamic or non-SGPR base";
		return trace;
	}
	trace.descriptor_user_sgpr = load.src0.reg;
	if (load.src0.reg < user_data_base || load.src0.reg - user_data_base >= user_data.size() ||
	    user_data.size() - (load.src0.reg - user_data_base) < 2u) {
		trace.rejection = "descriptor address pair is not initialized user data";
		return trace;
	}
	for (const auto& inst: program.instructions) {
		if (WritesScalar(inst, load.src0.reg) || WritesScalar(inst, load.src0.reg + 1u)) {
			trace.rejection = "descriptor user-data address pair is modified in the caller";
			return trace;
		}
	}
	const auto user_index = load.src0.reg - user_data_base;
	trace.user_words      = {user_data[user_index], user_data[user_index + 1u]};
	const auto base       = (uint64_t {trace.user_words[1]} << 32u) | trace.user_words[0];
	if (!AddOffset(base, trace.descriptor_offset, trace.descriptor_address)) {
		trace.rejection = "descriptor address exceeds the 48-bit scalar address bounds";
	}
	return trace;
}

bool ReadPrefix(MemoryReader reader, void* context, uint64_t address, size_t bytes,
                std::vector<uint32_t>& words, size_t& requested_bytes) {
	words.clear();
	words.reserve(bytes / sizeof(uint32_t));
	for (size_t offset = 0; offset < bytes;) {
		const auto chunk = std::min(ReadChunkBytes, bytes - offset);
		if (address > AddressMask || offset > AddressMask - address ||
		    chunk - 1u > AddressMask - address - offset)
			return false;
		const auto first = words.size();
		words.resize(first + chunk / sizeof(uint32_t));
		requested_bytes += chunk;
		if (!reader(context, address + offset, std::span(words).subspan(first))) {
			words.resize(first);
			return false;
		}
		offset += chunk;
	}
	return true;
}

std::vector<bool> ReachableAfterExternalReturn(const Decoder::Program& program) {
	std::vector<bool> reachable(program.instructions.size(), false);
	std::vector<size_t> pending;
	std::unordered_map<uint32_t, size_t> indices;
	for (size_t i = 0; i < program.instructions.size(); ++i) {
		indices.emplace(program.instructions[i].pc, i);
		if (IsSwappc(program.instructions[i]) && i + 1u < program.instructions.size())
			pending.push_back(i + 1u);
	}
	while (!pending.empty()) {
		const auto i = pending.back();
		pending.pop_back();
		if (reachable[i]) continue;
		reachable[i] = true;
		const auto& inst = program.instructions[i];
		if (inst.opcode == Opcode::S_ENDPGM) continue;
		if (inst.opcode == Opcode::S_SETPC_B64) {
			// An unknown caller jump after a call can reach any root load.
			std::fill(reachable.begin(), reachable.end(), true);
			break;
		}
		if (Decoder::IsDirectBranch(inst.opcode)) {
			const auto target = indices.find(inst.branch_target);
			if (target == indices.end()) {
				std::fill(reachable.begin(), reachable.end(), true);
				break;
			}
			pending.push_back(target->second);
			if (inst.opcode == Opcode::S_BRANCH) continue;
		}
		if (i + 1u < program.instructions.size()) pending.push_back(i + 1u);
	}
	return reachable;
}

} // namespace

DirectUserLoadCapture CaptureDirectUserLoads(
    bool enabled, const Decoder::Program& program, std::span<const uint32_t> user_data,
    MemoryReader reader, void* reader_context, uint32_t user_data_base) {
	DirectUserLoadCapture capture;
	if (!enabled) return capture;
	const bool unknown_writer = std::ranges::any_of(program.instructions, IsUnknownWriter);
	const auto post_call = ReachableAfterExternalReturn(program);
	for (size_t instruction_index = 0; instruction_index < program.instructions.size(); ++instruction_index) {
		const auto& inst = program.instructions[instruction_index];
		const bool direct_load = inst.opcode == Opcode::S_LOAD_DWORD ||
		    inst.opcode == Opcode::S_LOAD_DWORDX2 || inst.opcode == Opcode::S_LOAD_DWORDX4 ||
		    inst.opcode == Opcode::S_LOAD_DWORDX8 || inst.opcode == Opcode::S_LOAD_DWORDX16;
		if (!direct_load) continue;
		if (capture.loads.size() == MaxDirectUserLoads) {
			capture.limit_reached = true;
			break;
		}
		DirectUserLoadSnapshot load;
		load.pc = inst.pc;
		load.destination_sgpr = inst.dst.reg;
		load.user_sgpr = inst.src0.reg;
		load.dword_count = inst.data_dwords;
		load.offset = static_cast<int32_t>(inst.offset);
		if (post_call[instruction_index]) {
			load.rejection = "load can follow an external call which may clobber its base";
		} else if (unknown_writer || inst.dst.kind != OperandKind::Sgpr ||
		    inst.src0.kind != OperandKind::Sgpr || inst.src0.reg > 104u ||
		    !IsZeroOffset(inst.src1) || load.dword_count == 0u || load.dword_count > 16u) {
			load.rejection = "dynamic/unknown scalar load origin";
		} else if (load.user_sgpr < user_data_base ||
		           load.user_sgpr - user_data_base >= user_data.size() ||
		           user_data.size() - (load.user_sgpr - user_data_base) < 2u) {
			load.rejection = "base is not an initialized user pointer";
		} else if (std::ranges::any_of(program.instructions, [&](const auto& other) {
			return WritesScalar(other, load.user_sgpr) || WritesScalar(other, load.user_sgpr + 1u);
		})) {
			load.rejection = "user pointer pair is modified in caller";
		} else {
			const auto first = load.user_sgpr - user_data_base;
			const auto base = uint64_t {user_data[first]} | (uint64_t {user_data[first + 1u]} << 32u);
			// AddOffset checks a quartet range as well; the exact load's upper bound is checked
			// independently. Rejecting a smaller load near the end is conservative.
			if (!AddOffset(base, load.offset, load.address) ||
			    load.dword_count * sizeof(uint32_t) - 1u > AddressMask - load.address) {
				load.rejection = "load exceeds 48-bit scalar address bounds";
			} else if (reader == nullptr) {
				load.rejection = "memory reader unavailable";
			} else {
				load.words.resize(load.dword_count);
				capture.requested_bytes += load.words.size() * sizeof(uint32_t);
				load.read_failed = !reader(reader_context, load.address, load.words);
				if (load.read_failed) load.words.clear(); // failed callbacks may scribble bytes
			}
		}
		capture.loads.push_back(std::move(load));
	}
	return capture;
}

CallTableTraceResult TraceCallTables(const Decoder::Program&   program,
                                     std::span<const uint32_t> user_data, uint32_t user_data_base) {
	CallTableTraceResult result;
	for (size_t i = 0; i < program.instructions.size(); ++i) {
		const auto& inst = program.instructions[i];
		if (!IsSwappc(inst) || ((inst.raw[0] >> 16u) & 0x7fu) == 125u) continue;
		if (result.calls.size() == MaxCallSites) {
			result.call_sites_truncated = true;
			break;
		}
		result.calls.push_back(TraceCall(program, i, user_data, user_data_base));
	}
	return result;
}

CallCapture CaptureCallTables(bool enabled, const Decoder::Program& program,
                              std::span<const uint32_t> user_data, MemoryReader reader,
                              void* reader_context, uint32_t user_data_base) {
	CallCapture capture;
	capture.enabled = enabled;
	if (!enabled) return capture;
	const auto traces            = TraceCallTables(program, user_data, user_data_base);
	capture.call_sites_truncated = traces.call_sites_truncated;
	std::unordered_set<uint64_t> seen_targets;
	for (const auto& trace: traces.calls) {
		CallTableSnapshot table;
		table.trace = trace;
		if (!trace.rejection.empty()) {
			table.status = "trace rejected";
			capture.tables.push_back(std::move(table));
			continue;
		}
		if (reader != nullptr) ++capture.descriptor_read_requests;
		if (reader == nullptr ||
		    !reader(reader_context, trace.descriptor_address, table.descriptor)) {
			table.descriptor.fill(0u);
			table.read_failed = true;
			table.status =
			    reader == nullptr ? "memory reader unavailable" : "descriptor read failed";
			capture.tables.push_back(std::move(table));
			continue;
		}
		table.descriptor_read = true;
		ShaderBufferResource descriptor;
		std::copy(table.descriptor.begin(), table.descriptor.end(), descriptor.fields);
		table.table_base = descriptor.Base48() & ~uint64_t {3};
		table.table_size = descriptor.GetSize();
		if (descriptor.Type() != 0u || table.table_base == 0u || table.table_size == 0u) {
			table.status = "descriptor has no nonempty linear buffer range";
			capture.tables.push_back(std::move(table));
			continue;
		}
		const auto remaining = MaxTableBytes - capture.table_bytes_reserved;
		const auto bytes =
		    static_cast<size_t>(std::min<uint64_t>(table.table_size, remaining)) & ~size_t {3};
		table.table_truncated = bytes < table.table_size;
		if (bytes == 0u) {
			capture.table_budget_exhausted = remaining == 0u;
			table.status =
			    remaining == 0u ? "aggregate table budget exhausted" : "partial DWORD table";
			capture.tables.push_back(std::move(table));
			continue;
		}
		capture.table_bytes_reserved += bytes;
		table.read_failed = !ReadPrefix(reader, reader_context, table.table_base, bytes,
		                                table.words, capture.table_read_bytes_requested);
		capture.table_budget_exhausted |= capture.table_bytes_reserved == MaxTableBytes;
		table.status           = table.read_failed       ? "table prefix read failed"
		                         : table.table_truncated ? "table prefix capped"
		                                                 : "table captured";
		const auto table_index = capture.tables.size();
		for (size_t record = 0; record < table.words.size() / 4u; ++record) {
			const auto first  = record * 4u;
			const auto target = (uint64_t {table.words[first + 1u]} << 32u) | table.words[first];
			if (target == 0u) {
				++capture.zero_targets;
				continue;
			}
			if ((target & 3u) != 0u) {
				++capture.misaligned_targets;
				continue;
			}
			if (target > AddressMask) {
				++capture.outside_address_space_targets;
				continue;
			}
			if (!seen_targets.insert(target).second) {
				++capture.duplicate_targets;
				continue;
			}
			if (capture.targets.size() == MaxTargets) {
				capture.target_limit_reached = true;
				continue;
			}
			const auto target_remaining = MaxAggregateTargetBytes - capture.target_bytes_reserved;
			const auto target_bytes = std::min(MaxTargetBytes, target_remaining) & ~size_t {3};
			if (target_bytes == 0u) {
				capture.target_budget_exhausted = true;
				continue;
			}
			TargetSnapshot candidate;
			candidate.raw_address     = target;
			candidate.table_index     = table_index;
			candidate.record_index    = record;
			candidate.auxiliary_words = {table.words[first + 2u], table.words[first + 3u]};
			// Reserve failed attempts too: an unreadable prefix must not replenish the budget.
			capture.target_bytes_reserved += target_bytes;
			candidate.read_failed =
			    !ReadPrefix(reader, reader_context, target, target_bytes, candidate.words,
				            capture.target_read_bytes_requested);
			capture.target_budget_exhausted |= capture.target_bytes_reserved == MaxAggregateTargetBytes;
			candidate.prefix_capped = !candidate.read_failed;
			candidate.status = candidate.read_failed ? "prefix read failed; function extent unknown"
			                   : target_bytes < MaxTargetBytes
			                       ? "aggregate target budget prefix capped; function extent unknown"
			                       : "prefix capped; function extent unknown";
			capture.targets.push_back(std::move(candidate));
		}
		capture.tables.push_back(std::move(table));
	}
	return capture;
}

} // namespace Libs::Graphics::ShaderRecompiler::Diagnostics
