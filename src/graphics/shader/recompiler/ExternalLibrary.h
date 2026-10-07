#pragma once

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {

struct ExternalFunction {
	uint32_t function_id = 0;
	uint64_t guest_address = 0;
	std::vector<uint32_t> code_prefix;
};

struct ExternalRecord {
	uint32_t ordinal = 0;
	uint32_t function_id = 0;
	uint64_t function_address = 0;
	uint64_t auxiliary_address = 0;
};

struct ExternalCallSite {
	uint32_t caller_pc = 0;
	uint32_t target_sgpr = UINT32_MAX;
	uint32_t return_sgpr = UINT32_MAX;
	uint32_t context_domain = 0;
	uint32_t record_load_pc = 0;
	uint32_t record_sgpr = UINT32_MAX;
	uint32_t auxiliary_sgpr = UINT32_MAX;
	uint32_t descriptor_user_sgpr = UINT32_MAX;
	int32_t descriptor_offset = 0;
	uint64_t descriptor_address = 0;
	uint64_t table_base = 0;
	uint64_t table_bytes = 0;
	std::vector<uint64_t> candidate_addresses;
	std::vector<ExternalRecord> records;
	std::vector<IR::ExternalCallContextRecord> context_records;
};

struct ExternalDependencyRead {
	uint64_t address = 0;
	std::vector<uint32_t> words;
	bool operator==(const ExternalDependencyRead&) const = default;
};

struct ExternalLibraryPlan {
	uint64_t caller_address = 0;
	std::vector<ExternalFunction> functions;
	std::vector<ExternalCallSite> call_sites;
	std::vector<ExternalDependencyRead> dependencies;
	uint64_t dependency_hash = 0;
	// Complete table/candidate snapshots, not proof of reachable function closure.
	bool complete = false;
};

struct ExternalLibraryLoadResult {
	ExternalLibraryPlan plan;
	bool has_calls = false;
	std::string failure;
};

// Guarded reads only. Partial tables, invalid targets and missing prefixes fail.
[[nodiscard]] ExternalLibraryLoadResult LoadExternalLibrary(
    const Decoder::Program& caller, uint64_t caller_address,
    std::span<const uint32_t> user_data, uint32_t user_data_base,
    IR::SrtMemoryReader reader, void* reader_context = nullptr);

// Re-read every declared dependency before using a previously compiled mapping.
[[nodiscard]] bool ValidateExternalLibraryDependencies(
    const ExternalLibraryPlan& plan, IR::SrtMemoryReader reader,
    void* reader_context = nullptr);

// Other user words can change per dispatch. Only proved table-origin pointer pairs determine
// whether the previous table snapshot belongs to the current shader invocation.
[[nodiscard]] bool ExternalLibraryInputsMatch(
    const ExternalLibraryPlan& plan, std::span<const uint32_t> user_data,
    uint32_t user_data_base = 0);

[[nodiscard]] std::vector<IR::ExternalCallContextDomain> ExternalContextDomains(
    const ExternalLibraryPlan& plan);

} // namespace Libs::Graphics::ShaderRecompiler
