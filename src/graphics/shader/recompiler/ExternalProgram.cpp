#include "graphics/shader/recompiler/ExternalProgram.h"
#include "graphics/shader/recompiler/frontend/decode/ScalarWriteWidth.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace Libs::Graphics::ShaderRecompiler {
namespace {
using Decoder::Instruction;
using Decoder::Opcode;
constexpr size_t MaxLinkedInstructions = 4u * 1024u * 1024u;
struct Range {
	uint64_t              address;
	std::vector<uint32_t> words;
};
struct NativeInstruction {
	Instruction           inst;
	std::vector<uint64_t> successors;
};
using Body = std::map<uint64_t, NativeInstruction>;

[[noreturn]] void Fail(std::string message) {
	throw std::runtime_error(std::move(message));
}
uint64_t Add(uint64_t address, uint64_t bytes) {
	if (bytes > UINT64_MAX - address) Fail("external shader address overflow");
	return address + bytes;
}
bool Pair(const Decoder::Operand& operand) {
	return operand.kind == Decoder::OperandKind::Sgpr && operand.reg <= 104u;
}
std::vector<Range> MakeRanges(const ExternalLibraryPlan& library) {
	std::vector<const ExternalFunction*> ordered;
	for (const auto& function: library.functions) {
		if (function.code_prefix.empty() || (function.guest_address & 3u) != 0)
			Fail("empty or unaligned external function snapshot");
		ordered.push_back(&function);
	}
	std::ranges::sort(ordered, {}, &ExternalFunction::guest_address);
	std::vector<Range> ranges;
	for (const auto* function: ordered) {
		const auto end = Add(function->guest_address, function->code_prefix.size() * 4ull);
		if (ranges.empty() || function->guest_address >
		                          Add(ranges.back().address, ranges.back().words.size() * 4ull)) {
			ranges.push_back({function->guest_address, function->code_prefix});
			continue;
		}
		auto&        range   = ranges.back();
		const size_t offset  = static_cast<size_t>((function->guest_address - range.address) / 4u);
		const size_t overlap = std::min(function->code_prefix.size(), range.words.size() - offset);
		if (!std::equal(function->code_prefix.begin(), function->code_prefix.begin() + overlap,
		                range.words.begin() + offset))
			Fail("conflicting overlapping external code snapshots");
		if (end > Add(range.address, range.words.size() * 4ull))
			range.words.insert(range.words.end(), function->code_prefix.begin() + overlap,
			                   function->code_prefix.end());
	}
	return ranges;
}
std::span<const uint32_t> At(const std::vector<Range>& ranges, uint64_t address) {
	if ((address & 3u) != 0) Fail("unaligned reachable external instruction address");
	const auto found =
	    std::upper_bound(ranges.begin(), ranges.end(), address,
		                 [](uint64_t value, const Range& range) { return value < range.address; });
	if (found == ranges.begin()) return {};
	const auto& range  = *std::prev(found);
	const auto  offset = (address - range.address) / 4u;
	if (offset >= range.words.size()) return {};
	return std::span<const uint32_t>(range.words).subspan(static_cast<size_t>(offset));
}
uint64_t BranchAddress(uint64_t pc, uint32_t relative) {
	const int64_t delta = static_cast<int32_t>(relative);
	if (delta < 0) {
		const auto magnitude = static_cast<uint64_t>(-delta);
		if (magnitude > pc) Fail("external direct branch underflows guest address");
		return pc - magnitude;
	}
	return Add(pc, static_cast<uint64_t>(delta));
}
Body DecodeBody(const std::vector<Range>& ranges, uint64_t entry, size_t& instruction_count) {
	Body                         body;
	std::deque<uint64_t>         pending {entry};
	std::map<uint64_t, uint64_t> occupied;
	while (!pending.empty()) {
		const auto address = pending.front();
		pending.pop_front();
		if (body.contains(address)) continue;
		const auto prior = occupied.upper_bound(address);
		if (prior != occupied.begin() && std::prev(prior)->second > address)
			Fail("external branch enters the middle of an instruction");
		if (++instruction_count > MaxLinkedInstructions)
			Fail("external linked instruction budget exceeded");
		const auto words = At(ranges, address);
		if (words.empty())
			Fail(fmt::format("uncovered reachable external code at 0x{:016x}", address));
		if (Decoder::GetInstructionFamily(words.front()) == Decoder::Family::Unknown)
			Fail(fmt::format("unknown external instruction family at 0x{:016x}", address));
		std::array<uint32_t, 32> padded {};
		std::copy_n(words.begin(), std::min(words.size(), padded.size()), padded.begin());
		Instruction inst;
		Decoder::DecodeInstruction(padded, 0, inst);
		if (inst.word_count == 0 || inst.word_count > words.size() ||
		    inst.word_count > padded.size())
			Fail(fmt::format("incomplete external instruction at 0x{:016x}", address));
		if (inst.opcode == Opcode::UNSUPPORTED || inst.opcode == Opcode::UNKNOWN)
			Fail(fmt::format("unsupported external instruction at 0x{:016x}: {}", address,
			                 Decoder::InstructionToString(inst)));
		if (inst.opcode == Opcode::S_SWAPPC_B64 || inst.opcode == Opcode::S_GETPC_B64)
			Fail("external leaf bodies with nested calls or GETPC require additional linking "
			     "support");
		if (inst.opcode == Opcode::S_ENDPGM)
			Fail("external leaf body terminates without returning its saved link");
		const auto end       = Add(address, inst.word_count * 4ull);
		const auto following = occupied.lower_bound(address);
		if ((following != occupied.end() && following->first < end) ||
		    (following != occupied.begin() && std::prev(following)->second > address))
			Fail("external branch enters the middle of an instruction");
		occupied.emplace(address, end);
		NativeInstruction native {inst, {}};
		if (Decoder::IsDirectBranch(inst.opcode)) {
			native.successors.push_back(BranchAddress(address, inst.branch_target));
			if (Decoder::IsConditionalBranch(inst.opcode)) native.successors.push_back(end);
		} else if (inst.opcode != Opcode::S_SETPC_B64)
			native.successors.push_back(end);
		for (const auto successor: native.successors)
			pending.push_back(successor);
		body.emplace(address, std::move(native));
	}
	if (std::ranges::none_of(
	        body, [](const auto& item) { return item.second.inst.opcode == Opcode::S_SETPC_B64; }))
		Fail("external leaf body has no reachable return");
	return body;
}

// The link may be copied to another pair, but every returning path must retain
// both original halves. Numeric/dynamic tail jumps are not mistaken for returns.
void ValidateLinks(const Body& body, uint64_t entry, uint32_t link_reg) {
	using State = std::array<uint8_t, 106>;
	std::map<uint64_t, State> states;
	State                     initial {};
	initial.at(link_reg)      = 1;
	initial.at(link_reg + 1u) = 2;
	states.emplace(entry, initial);
	std::deque<uint64_t> pending {entry};
	while (!pending.empty()) {
		const auto address = pending.front();
		pending.pop_front();
		auto        state  = states.at(address);
		const auto& native = body.at(address);
		const auto& inst   = native.inst;
		if (inst.opcode == Opcode::S_SETPC_B64) {
			if (!Pair(inst.src0) || state[inst.src0.reg] != 1 || state[inst.src0.reg + 1u] != 2)
				Fail(fmt::format("external SETPC at 0x{:016x} is not a proven saved-link return",
				                 address));
			continue;
		}
		const auto old   = state;
		const auto write = [&](const Decoder::Operand& dst, uint32_t count) {
			if (dst.kind != Decoder::OperandKind::Sgpr) return;
			for (uint32_t i = 0; i < count && dst.reg + i < state.size(); ++i)
				state[dst.reg + i] = 0;
		};
		write(inst.dst, Decoder::ScalarDestinationDwords(inst));
		write(inst.dst2, 2u);
		if ((inst.opcode == Opcode::S_MOV_B32 || inst.opcode == Opcode::S_MOV_B64) &&
		    inst.dst.kind == Decoder::OperandKind::Sgpr &&
		    inst.src0.kind == Decoder::OperandKind::Sgpr) {
			const uint32_t n = inst.opcode == Opcode::S_MOV_B64 ? 2u : 1u;
			for (uint32_t i = 0;
			     i < n && inst.dst.reg + i < state.size() && inst.src0.reg + i < old.size(); ++i)
				state[inst.dst.reg + i] = old[inst.src0.reg + i];
		}
		for (const auto successor: native.successors) {
			auto [found, inserted] = states.emplace(successor, state);
			bool changed           = inserted;
			if (!inserted)
				for (size_t i = 0; i < state.size(); ++i) {
					const auto joined = found->second[i] == state[i] ? state[i] : 0u;
					if (found->second[i] != joined) {
						found->second[i] = static_cast<uint8_t>(joined);
						changed          = true;
					}
				}
			if (changed) pending.push_back(successor);
		}
	}
}
uint32_t AppendPc(LinkedExternalProgram& result, const Instruction& inst) {
	if (result.program.instructions.size() >= MaxLinkedInstructions)
		Fail("external combined instruction budget exceeded");
	if (result.code.size() > (UINT32_MAX / 4u) - inst.word_count)
		Fail("external combined shader exceeds 32-bit internal PC range");
	const auto pc = static_cast<uint32_t>(result.code.size() * 4u);
	result.code.insert(result.code.end(), inst.raw, inst.raw + inst.word_count);
	return pc;
}
} // namespace

LinkedExternalProgram LinkExternalProgram(const Decoder::Program&    caller,
                                          const ExternalLibraryPlan& library,
                                          uint32_t unwritten_vgpr) {
	LinkedExternalProgram result;
	try {
		const bool limited = unwritten_vgpr != UINT32_MAX;
		if (limited && unwritten_vgpr >= 256u) Fail("external coverage VGPR is out of range");
		result.coverage_vgpr = unwritten_vgpr;
		if (!library.complete || library.call_sites.empty())
			Fail("external library plan is incomplete or has no call sites");
		if (limited && std::ranges::any_of(caller.instructions, [](const Instruction& inst) {
			    return inst.opcode == Opcode::S_BARRIER;
		    }))
			Fail("checked external coverage cannot end waves in a caller with workgroup barriers");
		if (limited && std::ranges::any_of(caller.instructions, [](const Instruction& inst) {
			    const auto name = magic_enum::enum_name(inst.opcode);
			    if (inst.opcode == Opcode::S_SETREG_B32 || inst.opcode == Opcode::V_MOVRELD_B32 ||
			        inst.opcode == Opcode::V_MOVRELS_B32 || name.find("GPR_IDX") != std::string_view::npos)
				    return true;
			    for (const auto* operand: {&inst.dst, &inst.dst2, &inst.src0, &inst.src1, &inst.src2, &inst.src3})
				    if (operand->kind == Decoder::OperandKind::M0) return true;
			    return false;
		    }))
			Fail("checked external coverage cannot prove the caller's direct VGPR bank/index state");
		if (caller.code.size() > UINT32_MAX / 4u)
			Fail("caller shader exceeds 32-bit internal PC range");
		const auto ranges = MakeRanges(library);
		result.code.assign(caller.code.begin(), caller.code.end());
		result.program.instructions = caller.instructions;
		std::map<uint64_t, const ExternalFunction*> functions;
		std::set<uint32_t>                          function_ids, call_pcs;
		for (const auto& function: library.functions) {
			if (!functions.emplace(function.guest_address, &function).second ||
			    !function_ids.insert(function.function_id).second)
				Fail("duplicate external function address or identity");
		}
		std::map<uint64_t, Body> bodies;
		std::set<uint64_t>       denied_bodies, excluded;
		size_t                   instruction_count = 0;
		for (const auto& site: library.call_sites) {
			if (!call_pcs.insert(site.caller_pc).second) Fail("duplicate external call site");
			const auto call =
			    std::ranges::find(caller.instructions, site.caller_pc, &Instruction::pc);
			if (call == caller.instructions.end() || call->opcode != Opcode::S_SWAPPC_B64 ||
			    !Pair(call->src0) || !Pair(call->dst) || call->src0.reg != site.target_sgpr ||
			    call->dst.reg != site.return_sgpr)
				Fail("external call site does not match decoded SWAPPC register pairs");
			if (site.auxiliary_sgpr != UINT32_MAX &&
			    (site.auxiliary_sgpr > 104u ||
			     (site.auxiliary_sgpr <= site.return_sgpr + 1u && site.return_sgpr <= site.auxiliary_sgpr + 1u)))
				Fail("external auxiliary pair overlaps the overwritten return link or is invalid");
			if (limited) {
				const auto load = std::ranges::find(caller.instructions, site.record_load_pc, &Instruction::pc);
				if (load == caller.instructions.end() || load->opcode != Opcode::S_BUFFER_LOAD_DWORDX4 ||
				    load->dst.kind != Decoder::OperandKind::Sgpr || load->dst.reg != site.record_sgpr ||
				    site.auxiliary_sgpr > 104u || site.context_domain == UINT32_MAX || site.records.empty())
					Fail("checked external coverage has no proved record load, auxiliary pair, or complete domain");
			}
			const auto continuation = Add(site.caller_pc, call->word_count * 4ull);
			if (continuation > UINT32_MAX ||
			    std::ranges::find(caller.instructions, static_cast<uint32_t>(continuation),
			                      &Instruction::pc) == caller.instructions.end())
				Fail("external call has no decoded caller continuation");
			CFG::ExternalTransfer transfer {.pc          = site.caller_pc,
			                                .target_sgpr = site.target_sgpr,
			                                .return_sgpr = site.return_sgpr,
			                                .guest_pc = Add(library.caller_address, site.caller_pc),
			                                .link_address =
			                                    Add(library.caller_address, continuation),
			                                .call = true,
			                                .checked = limited,
			                                .record_load_pc = limited ? site.record_load_pc : UINT32_MAX,
			                                .auxiliary_sgpr = limited ? site.auxiliary_sgpr : UINT32_MAX,
			                                .context_domain = limited ? site.context_domain : UINT32_MAX};
			std::set<uint64_t>    targets;
			for (const auto target: site.candidate_addresses) {
				if (!targets.insert(target).second) continue;
				const auto function = functions.find(target);
				if (function == functions.end())
					Fail("call domain references an absent external function snapshot");
				if (denied_bodies.contains(target)) { excluded.insert(target); continue; }
				auto body = bodies.find(target);
				if (body == bodies.end()) {
					try {
						auto decoded_body = DecodeBody(ranges, target, instruction_count);
						if (limited && std::ranges::any_of(decoded_body, [&](const auto& item) {
							    return item.second.inst.opcode == Opcode::S_BARRIER ||
							           !Decoder::ProvesVgprUnwritten(item.second.inst, unwritten_vgpr);
						    })) {
							denied_bodies.insert(target);
							excluded.insert(target);
							continue;
						}
						body = bodies.emplace(target, std::move(decoded_body)).first;
					} catch (const std::exception&) {
						if (!limited) throw;
						// Incomplete/unsupported bodies are outside this coverage variant.
						// Their real addresses stay absent from the dispatcher mapping.
						denied_bodies.insert(target);
						excluded.insert(target);
						continue;
					}
				}
				try { ValidateLinks(body->second, target, site.return_sgpr); }
				catch (const std::exception&) {
					if (!limited) throw;
					// Saved-link validity is specific to this call site's return pair.
					excluded.insert(target);
					continue;
				}
				std::map<uint64_t, uint32_t>             relocated;
				std::vector<std::pair<uint64_t, size_t>> appended;
				for (const auto& [address, native]: body->second) {
					auto inst = native.inst;
					inst.pc   = AppendPc(result, inst);
					relocated.emplace(address, inst.pc);
					appended.emplace_back(address, result.program.instructions.size());
					result.program.instructions.push_back(inst);
				}
				for (const auto& [address, index]: appended) {
					auto& inst = result.program.instructions[index];
					if (Decoder::IsDirectBranch(inst.opcode))
						inst.branch_target =
						    relocated.at(body->second.at(address).successors.front());
					if (inst.opcode == Opcode::S_SETPC_B64)
						result.transfers.push_back(
						    {.pc              = inst.pc,
							 .target_sgpr     = inst.src0.reg,
							 .guest_pc        = address,
							 .guest_addresses = {transfer.link_address},
							 .target_pcs      = {static_cast<uint32_t>(continuation)}});
				}
				transfer.guest_addresses.push_back(target);
				transfer.target_pcs.push_back(relocated.at(target));
				result.entries.push_back({.pc             = relocated.at(target),
				                          .record_load_pc = site.record_load_pc,
				                          .auxiliary_sgpr = site.auxiliary_sgpr,
				                          .domain_id      = site.context_domain,
				                          .function_id    = function->second->function_id});
			}
			if (transfer.target_pcs.empty())
				Fail(limited ? "external call domain has no bodies satisfying strict VGPR coverage"
				             : "external call domain has no candidates");
			result.transfers.push_back(std::move(transfer));
		}
		for (const auto& inst: caller.instructions)
			if (inst.opcode == Opcode::S_SWAPPC_B64 && !call_pcs.contains(inst.pc))
				Fail("decoded SWAPPC has no complete external call domain");
		result.program.code = result.code;
		result.excluded_addresses.assign(excluded.begin(), excluded.end());
		result.success      = true;
	} catch (const std::exception& exception) {
		result.failure = exception.what();
	}
	return result;
}
LinkedExternalProgram BuildExternalCallProbe(const Decoder::Program& caller,
                                             const ExternalLibraryPlan& library) {
	LinkedExternalProgram result;
	try {
		if (!library.complete || library.call_sites.empty())
			Fail("external probe requires a complete call-table plan");
		if (caller.code.size() > UINT32_MAX / 4u)
			Fail("probe caller exceeds the internal PC range");
		result.code.assign(caller.code.begin(), caller.code.end());
		result.program.instructions = caller.instructions;
		std::set<uint32_t> call_pcs;
		for (const auto& site: library.call_sites) {
			if (!call_pcs.insert(site.caller_pc).second)
				Fail("duplicate external probe call site");
			const auto call = std::ranges::find(caller.instructions, site.caller_pc, &Instruction::pc);
			if (call == caller.instructions.end() || call->opcode != Opcode::S_SWAPPC_B64 ||
			    !Pair(call->src0) || !Pair(call->dst) || call->src0.reg != site.target_sgpr ||
			    call->dst.reg != site.return_sgpr)
				Fail("external probe does not match the actual SWAPPC register pairs");
			const auto load = std::ranges::find(caller.instructions, site.record_load_pc, &Instruction::pc);
			if (load == caller.instructions.end() || load->opcode != Opcode::S_BUFFER_LOAD_DWORDX4 ||
			    load->dst.kind != Decoder::OperandKind::Sgpr || load->dst.reg != site.record_sgpr ||
			    site.auxiliary_sgpr > 104u || site.context_domain == UINT32_MAX ||
			    site.candidate_addresses.empty() || site.records.empty())
				Fail("external probe has no proved record load, auxiliary pair, or complete domain");
			result.transfers.push_back({
			    .pc = site.caller_pc, .target_sgpr = site.target_sgpr,
			    .return_sgpr = site.return_sgpr,
			    .guest_pc = Add(library.caller_address, site.caller_pc), .call = true,
			    .probe = true, .record_load_pc = site.record_load_pc,
			    .auxiliary_sgpr = site.auxiliary_sgpr, .context_domain = site.context_domain});
		}
		for (const auto& inst: caller.instructions) {
			if (inst.opcode == Opcode::S_SWAPPC_B64 && !call_pcs.contains(inst.pc))
				Fail("probe caller contains a SWAPPC without proved call-table provenance");
			if (inst.opcode == Opcode::S_BARRIER)
				Fail("external probe cannot end waves in a caller with workgroup barriers");
		}
		result.program.code = result.code;
		result.success = true;
	} catch (const std::exception& exception) {
		result.failure = exception.what();
	}
	return result;
}
} // namespace Libs::Graphics::ShaderRecompiler
