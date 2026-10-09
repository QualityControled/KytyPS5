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
		bool     force_unnormalized_coordinates   = false;
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

// Optional failure-only metadata. Indices/PC identify plan associations, not executed operations.
enum class SoftwareColorComparisonFailure : uint8_t {
    None, ImageCountMismatch, InvalidImageSnapshot, UnsupportedTextureFields,
    UnsupportedLiveConsumer, ReservedSwizzle, InvalidSamplerSnapshot, RejectedScope, ConflictingSamplerModes,
    MissingSampledPair, BindingAliases
};
struct SoftwareColorComparisonFailureReport {
    SoftwareColorComparisonFailure reason = SoftwareColorComparisonFailure::None;
    uint32_t image = UINT32_MAX;
    uint32_t sampler = UINT32_MAX;
    uint32_t pair_first_use_pc = 0;
    bool pair_first_use_pc_present = false;
    uint32_t scope_reason = UINT32_MAX;
};

constexpr const char* SoftwareColorComparisonFailureName(SoftwareColorComparisonFailure reason) {
    switch (reason) {
        case SoftwareColorComparisonFailure::None: return "none";
        case SoftwareColorComparisonFailure::ImageCountMismatch: return "image_count_mismatch";
        case SoftwareColorComparisonFailure::InvalidImageSnapshot: return "invalid_image_snapshot";
        case SoftwareColorComparisonFailure::UnsupportedTextureFields: return "unsupported_texture_fields";
        case SoftwareColorComparisonFailure::UnsupportedLiveConsumer: return "unsupported_live_consumer";
        case SoftwareColorComparisonFailure::ReservedSwizzle: return "reserved_swizzle";
        case SoftwareColorComparisonFailure::InvalidSamplerSnapshot: return "invalid_sampler_snapshot";
        case SoftwareColorComparisonFailure::RejectedScope: return "rejected_scope";
        case SoftwareColorComparisonFailure::ConflictingSamplerModes: return "conflicting_sampler_modes";
        case SoftwareColorComparisonFailure::MissingSampledPair: return "missing_sampled_pair";
        case SoftwareColorComparisonFailure::BindingAliases: return "binding_aliases";
    }
    return "unknown";
}

// Pure post-materialization configuration. No additional guest reads or descriptor mutation.
// Capability must come from the physical device optimal-tiling FormatProperties3 query.
bool ConfigureSoftwareColorComparison(const ResourcePlan& plan, const ResourceSnapshot& snapshot,
                                      ResourceSpecialization& specialization, bool enabled,
                                      bool r8_native_comparison_supported,
                                      SoftwareColorComparisonFailureReport* failure_report = nullptr);

// Applies an already-derived specialization to native IR before layout and emission.
void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
