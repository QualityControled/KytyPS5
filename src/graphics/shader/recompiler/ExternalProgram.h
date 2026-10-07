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
	bool                               success = false;
	std::string                        failure;
};

// Decodes only reachable leaf bodies from a byte-consistent prefix union. Native
// addresses and saved links remain 64-bit; only internal block labels are relocated.
[[nodiscard]] LinkedExternalProgram LinkExternalProgram(const Decoder::Program&    caller,
                                                        const ExternalLibraryPlan& library);

} // namespace Libs::Graphics::ShaderRecompiler
