#include "graphics/shader/recompiler/ExternalLibrary.h"
#include "graphics/shader/recompiler/frontend/decode/ScalarWriteWidth.h"

#include "graphics/shader/recompiler/ShaderCallDiagnostics.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler {
namespace {

uint64_t Pair(uint32_t low, uint32_t high) {
	return uint64_t {low} | (uint64_t {high} << 32u);
}

bool DescriptorAddress(uint64_t base, int32_t offset, uint64_t& address) {
	// Match the native scalar load and Diagnostics::AddOffset before comparing a cache origin.
	constexpr uint64_t address_mask = 0x0000ffffffffffffull;
	base &= address_mask & ~uint64_t {3};
	const auto signed_offset = static_cast<int64_t>(offset) & ~int64_t {3};
	if (signed_offset < 0) {
		const auto magnitude = static_cast<uint64_t>(-signed_offset);
		if (magnitude > base) return false;
		address = base - magnitude;
	} else {
		const auto magnitude = static_cast<uint64_t>(signed_offset);
		if (magnitude > address_mask - base) return false;
		address = base + magnitude;
	}
	return sizeof(uint32_t) * 4u - 1u <= address_mask - address;
}


bool AddCodeDependency(ExternalLibraryPlan& plan, uint64_t address,
                       const std::vector<uint32_t>& words) {
	if (plan.dependencies.empty()) {
		plan.dependencies.push_back({address, words});
		return true;
	}
	auto& previous = plan.dependencies.back();
	const auto end = previous.address + previous.words.size() * sizeof(uint32_t);
	if (address > end) {
		plan.dependencies.push_back({address, words});
		return true;
	}
	const auto offset = static_cast<size_t>((address - previous.address) / sizeof(uint32_t));
	const auto overlap = std::min(words.size(), previous.words.size() - offset);
	if (!std::equal(words.begin(), words.begin() + overlap, previous.words.begin() + offset))
		return false;
	// Keep the complete byte union, but read shared tails only once on a warm cache hit.
	previous.words.insert(previous.words.end(), words.begin() + overlap, words.end());
	return true;
}

uint64_t IdentityHash(const ExternalLibraryPlan& plan) {
	// This is a bucket hash. Cache equality must also compare exact dependencies.
	uint64_t hash = 14695981039346656037ull;
	const auto mix = [&](uint64_t value) {
		for (uint32_t byte = 0; byte < 8u; ++byte) {
			hash ^= (value >> (byte * 8u)) & 0xffu;
			hash *= 1099511628211ull;
		}
	};
	mix(plan.caller_address);
	for (const auto& read: plan.dependencies) {
		mix(read.address);
		mix(read.words.size());
		for (const auto word: read.words) mix(word);
	}
	return hash;
}

uint32_t AuxiliaryPair(const Decoder::Program& caller, uint32_t record_pc,
                       uint32_t call_pc, uint32_t record_sgpr) {
	std::array<int32_t, 108> origins;
	origins.fill(-1);
	if (record_sgpr > 104u) return UINT32_MAX;
	for (uint32_t word = 0; word < 4u; ++word) origins[record_sgpr + word] = word;
	for (const auto& inst: caller.instructions) {
		if (inst.pc <= record_pc) continue;
		if (inst.pc >= call_pc) break;
		if (Decoder::IsDirectBranch(inst.opcode) || inst.opcode == Decoder::Opcode::S_SETPC_B64 ||
		    inst.opcode == Decoder::Opcode::S_SWAPPC_B64 || inst.opcode == Decoder::Opcode::S_ENDPGM ||
		    inst.opcode == Decoder::Opcode::UNKNOWN || inst.opcode == Decoder::Opcode::UNSUPPORTED)
			return UINT32_MAX;
		const bool mov64 = inst.opcode == Decoder::Opcode::S_MOV_B64;
		const bool copy = mov64 || inst.opcode == Decoder::Opcode::S_MOV_B32;
		const auto old = origins;
		if (inst.dst.kind == Decoder::OperandKind::Sgpr) {
			const auto width = Decoder::ScalarDestinationDwords(inst);
			if (inst.dst.reg >= origins.size() || width > origins.size() - inst.dst.reg)
				return UINT32_MAX;
			for (uint32_t word = 0; word < width; ++word) {
				origins[inst.dst.reg + word] = copy && inst.src0.kind == Decoder::OperandKind::Sgpr &&
				                                      inst.src0.reg < old.size() &&
				                                      word < old.size() - inst.src0.reg
				                                  ? old[inst.src0.reg + word] : -1;
			}
		}
		// Vector carry instructions can also overwrite a two-word scalar destination.
		if (inst.dst2.kind == Decoder::OperandKind::Sgpr) {
			if (inst.dst2.reg >= origins.size() || 2u > origins.size() - inst.dst2.reg)
				return UINT32_MAX;
			origins[inst.dst2.reg] = -1;
			origins[inst.dst2.reg + 1u] = -1;
		}
	}
	uint32_t result = UINT32_MAX;
	for (uint32_t reg = 0; reg + 1u < origins.size(); ++reg) {
		// Ignore the original payload anchor, but allow a transported auxiliary pair
		// to overlap the original four-word load destination. Origins prove the MOVs.
		if (reg == record_sgpr + 2u) continue;
		if (origins[reg] == 2 && origins[reg + 1u] == 3) {
			if (result != UINT32_MAX) return UINT32_MAX;
			result = reg;
		}
	}
	return result;
}

} // namespace

ExternalLibraryLoadResult LoadExternalLibrary(
    const Decoder::Program& caller, uint64_t caller_address,
    std::span<const uint32_t> user_data, uint32_t user_data_base,
    IR::SrtMemoryReader reader, void* reader_context) {
	ExternalLibraryLoadResult result;
	result.plan.caller_address = caller_address;
	const auto traces = Diagnostics::TraceCallTables(caller, user_data, user_data_base);
	result.has_calls = !traces.calls.empty() || traces.call_sites_truncated;
	if (!result.has_calls) return result;
	const auto fail = [&](const char* reason) {
		result.failure = reason;
		result.plan.complete = false;
	};
	if (caller_address == 0u || caller_address > 0x0000ffffffffffffull ||
	    (caller_address & 3u) != 0u) {
		fail("external library requires a valid aligned guest caller address");
		return result;
	}
	if (traces.call_sites_truncated) {
		fail("external call-site limit reached");
		return result;
	}
	for (const auto& trace: traces.calls) {
		if (!trace.rejection.empty()) {
			result.failure = "external function-table origin unproven: " + trace.rejection;
			return result;
		}
	}
	auto capture = Diagnostics::CaptureCallTables(
	    true, caller, user_data, reader, reader_context, user_data_base);
	if (capture.tables.size() != traces.calls.size() || capture.call_sites_truncated ||
	    capture.target_limit_reached || capture.zero_targets != 0u ||
	    capture.misaligned_targets != 0u || capture.outside_address_space_targets != 0u) {
		fail("external library candidate set is incomplete or contains invalid code pointers");
		return result;
	}
	std::map<uint64_t, Diagnostics::TargetSnapshot*> targets;
	for (auto& target: capture.targets) {
		if (target.read_failed || target.words.size() * sizeof(uint32_t) != Diagnostics::MaxTargetBytes) {
			fail("external function prefix is unavailable or incomplete");
			return result;
		}
		targets.emplace(target.raw_address, &target);
	}
	std::map<uint64_t, uint32_t> function_ids;
	for (const auto& [address, target]: targets) {
		const auto id = static_cast<uint32_t>(result.plan.functions.size());
		function_ids.emplace(address, id);
		if (!AddCodeDependency(result.plan, address, target->words)) {
			fail("external function snapshots conflict in overlapping code bytes");
			return result;
		}
		result.plan.functions.push_back({id, address, std::move(target->words)});
	}
	for (const auto& table: capture.tables) {
		ShaderBufferResource descriptor;
		std::copy(table.descriptor.begin(), table.descriptor.end(), descriptor.fields);
		if (!table.descriptor_read || table.read_failed || table.table_truncated ||
		    table.table_size == 0u || table.table_size % 16u != 0u ||
		    table.words.size() * sizeof(uint32_t) != table.table_size ||
		    descriptor.Type() != 0u || (descriptor.PackedStride() & 0x3fffu) != 16u ||
		    (descriptor.PackedStride() & (1u << 14u)) != 0u) {
			fail("external function table requires a complete linear 16-byte record buffer");
			return result;
		}
		ExternalCallSite site;
		site.caller_pc = table.trace.call_pc;
		site.target_sgpr = table.trace.target_sgpr;
		site.return_sgpr = table.trace.return_sgpr;
		site.context_domain = static_cast<uint32_t>(result.plan.call_sites.size());
		site.record_load_pc = table.trace.record_load_pc;
		site.descriptor_user_sgpr = table.trace.descriptor_user_sgpr;
		site.descriptor_offset = table.trace.descriptor_offset;
		site.descriptor_address = table.trace.descriptor_address;
		site.table_base = table.table_base;
		site.table_bytes = table.table_size;
		const auto load = std::ranges::find(caller.instructions, site.record_load_pc,
		                                   &Decoder::Instruction::pc);
		if (load == caller.instructions.end() || load->dst.kind != Decoder::OperandKind::Sgpr) {
			fail("external record load has no proven scalar destination");
			return result;
		}
		site.record_sgpr = load->dst.reg;
		site.auxiliary_sgpr = AuxiliaryPair(caller, site.record_load_pc, site.caller_pc, site.record_sgpr);
		std::set<uint64_t> candidates;
		for (uint32_t ordinal = 0; ordinal < table.words.size() / 4u; ++ordinal) {
			const auto first = ordinal * 4u;
			const auto address = Pair(table.words[first], table.words[first + 1u]);
			const auto function = function_ids.find(address);
			if (function == function_ids.end()) {
				fail("external table record has no complete candidate prefix");
				return result;
			}
			const auto auxiliary = Pair(table.words[first + 2u], table.words[first + 3u]);
			site.records.push_back({ordinal, function->second, address, auxiliary});
			site.context_records.push_back({ordinal, function->second,
			    {table.words[first], table.words[first + 1u], table.words[first + 2u], table.words[first + 3u]}});
			candidates.insert(address);
		}
		site.candidate_addresses.assign(candidates.begin(), candidates.end());
		result.plan.dependencies.push_back({table.trace.descriptor_address,
		                                   {table.descriptor.begin(), table.descriptor.end()}});
		result.plan.dependencies.push_back({table.table_base, table.words});
		result.plan.call_sites.push_back(std::move(site));
	}
	result.plan.dependency_hash = IdentityHash(result.plan);
	result.plan.complete = true;
	return result;
}

bool ValidateExternalLibraryDependencies(const ExternalLibraryPlan& plan,
                                         IR::SrtMemoryReader reader, void* reader_context) {
	if (!plan.complete || reader == nullptr) return false;
	std::array<uint32_t, Diagnostics::ReadChunkBytes / sizeof(uint32_t)> words;
	for (const auto& dependency: plan.dependencies) {
		for (size_t offset = 0; offset < dependency.words.size(); offset += words.size()) {
			const auto count = std::min(words.size(), dependency.words.size() - offset);
			if (!reader(reader_context, dependency.address + offset * sizeof(uint32_t),
			            std::span(words).first(count)) ||
			    !std::equal(words.begin(), words.begin() + count, dependency.words.begin() + offset))
				return false;
		}
	}
	return true;
}

bool ExternalLibraryInputsMatch(const ExternalLibraryPlan& plan,
                                std::span<const uint32_t> user_data, uint32_t user_data_base) {
	if (!plan.complete || plan.call_sites.empty()) return false;
	for (const auto& site: plan.call_sites) {
		if (site.descriptor_user_sgpr < user_data_base) return false;
		const auto index = site.descriptor_user_sgpr - user_data_base;
		if (index >= user_data.size() || user_data.size() - index < 2u) return false;
		uint64_t address;
		if (!DescriptorAddress(Pair(user_data[index], user_data[index + 1u]), site.descriptor_offset, address))
			return false;
		if (address != site.descriptor_address) return false;
	}
	return true;
}

std::vector<IR::ExternalCallContextDomain> ExternalContextDomains(const ExternalLibraryPlan& plan) {
	std::vector<IR::ExternalCallContextDomain> domains;
	domains.reserve(plan.call_sites.size());
	for (const auto& site: plan.call_sites)
		domains.push_back({site.context_domain, plan.complete, site.context_records});
	return domains;
}

} // namespace Libs::Graphics::ShaderRecompiler
