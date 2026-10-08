#pragma once

#include "graphics/shader/recompiler/EqaaFactCollection.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"


namespace Libs::Graphics::ShaderRecompiler::Diagnostics {

// AMD PAL gfx9_plus_merged_mask.h defines the enable mask as 0x00000001.
// Preserve the raw DWORD; offset/round fields do not imply coverage enable.
inline constexpr bool RawAlphaToMaskEnabled(uint32_t db_alpha_to_mask) {
	return (db_alpha_to_mask & 0x00000001u) != 0;
}

struct ReducedEqaa2xInputs {
	bool requested = false;
	bool raw_sample_state_complete = false;
	uint32_t encoded_coverage_samples = UINT32_MAX;
	uint32_t encoded_color_fragments = UINT32_MAX;
	uint32_t encoded_depth_samples = UINT32_MAX;
	uint32_t aa_mask_low = 0;
	uint32_t aa_mask_high = 0;
	uint32_t sample_exclusion = UINT32_MAX;
	bool alpha_to_mask_enabled = true;
	bool ps_sample_shading = true;
	bool ps_sample_mask_export = true;
	bool ps_ancillary = true;
	const IR::PixelSampleSensitivity* pixel = nullptr;
};

enum class ReducedEqaa2xDecision {
	Disabled, WrongCounts, MissingRawState, NonFullMask, AlphaToCoverage,
	MissingPixelSummary, PixelSampleDependent, IndirectAddressDependency,
	AdmitApproximation
};

inline const char* ReducedEqaa2xDecisionName(ReducedEqaa2xDecision value) {
	switch (value) {
	case ReducedEqaa2xDecision::Disabled: return "disabled";
	case ReducedEqaa2xDecision::WrongCounts: return "profile_requires_4coverage_2color_2depth";
	case ReducedEqaa2xDecision::MissingRawState: return "raw_sample_state_incomplete";
	case ReducedEqaa2xDecision::NonFullMask: return "sample_mask_or_exclusion_sensitive";
	case ReducedEqaa2xDecision::AlphaToCoverage: return "alpha_to_coverage_enabled";
	case ReducedEqaa2xDecision::MissingPixelSummary: return "original_pixel_summary_unavailable";
	case ReducedEqaa2xDecision::PixelSampleDependent: return "pixel_sample_or_fmask_sensitive";
	case ReducedEqaa2xDecision::IndirectAddressDependency: return "unbounded_descriptor_or_raw_memory_dependency";
	case ReducedEqaa2xDecision::AdmitApproximation: return "experimental_native_2x_initial_contents_unknown";
	}
	return "unknown";
}

// This is a bounded visual-quality approximation, not a native EQAA mapping.
// It deliberately does not infer initialized physical samples from metadata.
// The caller must retain native 2-sample color/depth/stencil layouts and operations,
// guest metadata, all ordinary format/view guards, and separate pipeline/cache identity.
inline ReducedEqaa2xDecision ClassifyReducedEqaa2x(const ReducedEqaa2xInputs& in) {
	if (!in.requested) return ReducedEqaa2xDecision::Disabled;
	if (in.encoded_coverage_samples != 2 || in.encoded_color_fragments != 1 ||
	    in.encoded_depth_samples != 1) return ReducedEqaa2xDecision::WrongCounts;
	if (!in.raw_sample_state_complete) return ReducedEqaa2xDecision::MissingRawState;
	if (in.aa_mask_low != UINT32_MAX || in.aa_mask_high != UINT32_MAX || in.sample_exclusion != 0)
		return ReducedEqaa2xDecision::NonFullMask;
	if (in.alpha_to_mask_enabled) return ReducedEqaa2xDecision::AlphaToCoverage;
	if (in.pixel == nullptr || !in.pixel->captured || !in.pixel->complete)
		return ReducedEqaa2xDecision::MissingPixelSummary;
	const auto& p = *in.pixel;
	if (in.ps_sample_shading || in.ps_sample_mask_export || in.ps_ancillary || p.sample_id ||
	    p.packed_ancillary || p.sample_mask_export || p.fmask_candidates != 0 ||
	    p.msaa_candidates != 0 || p.unknown_image_candidates != 0)
		return ReducedEqaa2xDecision::PixelSampleDependent;
	if (p.indirect_images != 0 || p.indirect_buffers != 0 || p.indirect_samplers != 0 ||
	    p.address_memory_rows != 0 || p.indirect_buffer_rows != 0 || p.dynamic_descriptor_rows != 0)
		return ReducedEqaa2xDecision::IndirectAddressDependency;
	return ReducedEqaa2xDecision::AdmitApproximation;
}

} // namespace Libs::Graphics::ShaderRecompiler::Diagnostics
