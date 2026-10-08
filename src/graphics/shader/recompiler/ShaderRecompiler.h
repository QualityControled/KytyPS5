#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {

struct ExternalLibraryPlan;

struct CompileOptions {
	ShaderType                  stage           = ShaderType::Compute;
	uint32_t                    wave_size       = 64;
	uint32_t                    user_data_base  = 0;
	uint64_t                    shader_hash     = 0;
	bool                        dump_ir                    = true;
	bool                        early_dump                 = false;
	const char*                 dump_label                 = nullptr;
	std::span<const uint32_t>   user_data;
	std::span<const uint32_t>   back_code;
	ShaderStageInputInfo        input_info;
	const ExternalLibraryPlan*  external_library = nullptr;
	// Diagnostic variant: record an actual external target, then end its wave.
	// The host must wait and inspect the record before submitting consumers.
	bool                        external_call_probe = false;
	// Diagnostic probe: capture the first active native BVH input and return
	// before the intersection helper accesses guest memory.
	bool                        external_probe_before_bvh = false;
	// Opt-in caller-only probe lowering; retain dispatcher fallback on any CFG
	// or exact call-metadata validation failure. Ordinary shaders are unchanged.
	bool                        external_probe_structured = false;
	// Execute each active BVH normally, then capture one completed tuple/result
	// and end the native wave. The host must stop even if no event is recorded.
	bool                        external_probe_after_bvh = false;
	// Strict coverage variant: only link leaf bodies proved not to write this
	// VGPR. Every other target faults and ends its wave; the host must inspect
	// the fault channel before any dispatch consumer is submitted.
	uint32_t                    external_unwritten_vgpr = UINT32_MAX;
};

struct TranslateResult {
	IR::Program program;
	std::string decoded_dump;
	std::string cfg_dump;
};

struct CompileResult {
	std::vector<uint32_t>  spirv;
	std::string            decoded_dump;
	std::string            ir_dump;
	IR::Program            program;
};

[[nodiscard]] TranslateResult TranslateProgram(std::span<const uint32_t> code,
                                               const CompileOptions& options);
[[nodiscard]] CompileResult CompileProgram(TranslateResult translated,
                                           const CompileOptions& options,
                                           const IR::ResourceSpecialization& specialization,
	                                       uint32_t push_data_start_dword = 0);

} // namespace Libs::Graphics::ShaderRecompiler

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_ */
