#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Canonical module-affecting resource state. Runtime addresses and descriptor payloads remain in
// ResourceSnapshot and therefore do not create shader permutations.
struct ResourceSpecialization {
	struct Buffer {
		uint32_t               packed_stride                   = 0;
		Prospero::BufferFormat descriptor_format               = Prospero::BufferFormat::kInvalid;
		uint32_t               descriptor_swizzle              = DstSel(4, 5, 6, 7);
		bool                   zero_stride_oob                 = false;
		uint32_t               indirect_root                   = BufferResource::NoIndirectBuffer;
		uint32_t               indirect_mapping_offset         = 0;
		uint32_t               indirect_search_iterations      = 0;
		bool                   operator==(const Buffer&) const = default;
	};

	struct Image {
		Prospero::TextureNumericClass numeric_class = Prospero::TextureNumericClass::Unsupported;
		Decoder::ImageDimension       dimension     = Decoder::ImageDimension::Unknown;
		uint32_t                      mip_count     = 1;
		Prospero::BufferFormat        conversion_format          = Prospero::BufferFormat::kInvalid;
		uint32_t                      shader_swizzle             = ShaderImageIdentitySwizzle;
		ImageComparisonMode           comparison_mode            = ImageComparisonMode::Native;
		uint32_t                      indirect_root              = ImageResource::NoIndirectImage;
		uint32_t                      indirect_mapping_offset    = 0;
		uint32_t                      indirect_search_iterations = 0;
		bool                          cube                       = false;
		bool                          fmask                      = false;
		uint32_t                      binding_alias              = UINT32_MAX;
		bool                          operator==(const Image&) const = default;
	};

	struct Sampler {
		uint32_t indirect_root                    = SamplerResource::NoIndirectSampler;
		uint32_t indirect_mapping_offset          = 0;
		uint32_t indirect_search_iterations       = 0;
		bool     operator==(const Sampler&) const = default;
	};

	// Enable separate native-comparison/noncomparison sampler variants only for this mode.
	bool software_color_dref_enabled = false;
	std::vector<Buffer> buffers;
	std::vector<Image>  images;
	std::vector<Sampler> samplers;
	// Indexed by the final sampler filtering/border variants, not raw snapshots.
	std::vector<uint32_t>               sampler_binding_aliases;
	std::vector<DynamicBufferFormatSet> dynamic_buffer_formats;

	bool operator==(const ResourceSpecialization&) const = default;
};

// Extracts the descriptor/SRT value graph before resource specialization. The returned plan owns
// its values and is independent of the translated shader CFG.
ResourcePlan ExtractResourcePlan(const Program& program);

// Refreshes cached resources and specialization in place. A failed refresh must not be used.
bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization);

// Pure post-materialization configuration. No additional guest reads or descriptor mutation.
// Capability must come from the physical device optimal-tiling FormatProperties3 query.
bool ConfigureSoftwareColorComparison(const ResourcePlan& plan, const ResourceSnapshot& snapshot,
                                      ResourceSpecialization& specialization, bool enabled,
                                      bool r8_native_comparison_supported);

// Applies an already-derived specialization to native IR before layout and emission.
void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
