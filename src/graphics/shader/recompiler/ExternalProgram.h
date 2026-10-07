#pragma once

#include "graphics/shader/recompiler/ExternalLibrary.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"

namespace Libs::Graphics::ShaderRecompiler {

struct ExternalFunctionEntry {
	uint32_t pc             = 0;
	uint32_t record_load_pc = 0;
	uint32_t auxiliary_sgpr = UINT32_MAX;
	uint32_t domain_id      = 0;
	uint32_t function_id    = 0;
};

struct LinkedExternalProgram {
	std::vector<uint32_t>              code;
	Decoder::Program                   program;
	std::vector<CFG::ExternalTransfer> transfers;
	std::vector<ExternalFunctionEntry> entries;
	std::vector<uint64_t>              excluded_addresses;
	uint32_t                           coverage_vgpr = UINT32_MAX;
	bool                               success = false;
	std::string                        failure;
};

// Decodes only reachable leaf bodies from a byte-consistent prefix union. Native
// addresses and saved links remain 64-bit; only internal block labels are relocated.
[[nodiscard]] LinkedExternalProgram LinkExternalProgram(const Decoder::Program&    caller,
                                                        const ExternalLibraryPlan& library,
                                                        uint32_t unwritten_vgpr = UINT32_MAX);

// Diagnostic-only caller graph. Each proved external call records its inputs
// and returns; no library instruction or caller continuation is substituted.
[[nodiscard]] LinkedExternalProgram BuildExternalCallProbe(const Decoder::Program& caller,
                                                          const ExternalLibraryPlan& library);

} // namespace Libs::Graphics::ShaderRecompiler
