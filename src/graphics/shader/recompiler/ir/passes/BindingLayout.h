#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

void AllocateBindings(Program& program, uint32_t push_data_start_dword = 0,
                      bool lds_storage = false);

struct SharedMemoryResources {
	bool lds = false;
	bool gds = false;
};

SharedMemoryResources CollectMemoryResources(const Program& program, std::vector<uint32_t>& buffers);
bool UsesFlattenedSrt(const Program& program);

struct DescriptorBindingLimits;
// Counts actual allocated descriptors, including mip views and helper buffers.
// The renderer remains responsible for descriptor-set totals across stages.
bool ValidateDescriptorBindingLimits(const Program& program, const DescriptorBindingLimits& limits,
                                     std::string& failure);

const DescriptorBinding* FindBinding(const BindingLayout& layout, DescriptorBindingKind kind);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_ */
