#pragma once

#include "graphics/shader/recompiler/EqaaReduced2xPolicy.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Diagnostics {

inline bool EqaaReduced2xResolveRequested() {
	static const bool requested = [] {
		const auto* value = std::getenv("KYTY_EXPERIMENTAL_EQAA_2X_RESOLVE");
		return value != nullptr && std::strcmp(value, "1") == 0 && EqaaReduced2xRequested();
	}();
	return requested;
}

struct ReducedEqaa2xResolveInputs {
	bool requested = false;
	bool existing_resolve_source_path = false;
	uint32_t color_mode = 0;
	uint32_t color_rop = 0;
	uint32_t source_encoded_coverage = UINT32_MAX;
	uint32_t source_encoded_fragments = UINT32_MAX;
	uint32_t destination_encoded_coverage = UINT32_MAX;
	uint32_t destination_encoded_fragments = UINT32_MAX;
	uint64_t source_address = 0;
	uint64_t destination_address = 0;
	std::array<uint32_t, 3> source_format {};
	std::array<uint32_t, 3> destination_format {};
	bool source_2d_single_mip_tiled = false;
	bool destination_2d_base_mip = false;
	uint32_t destination_levels = 0;
	bool view_bounds_known = false;
	uint32_t source_layers = 0;
	uint32_t destination_layers = 0;
	uint32_t source_width = 0;
	uint32_t source_height = 0;
	uint32_t destination_width = 0;
	uint32_t destination_height = 0;
	bool raw_sample_state_complete = false;
	uint32_t aa_mask_low = 0;
	uint32_t aa_mask_high = 0;
	uint32_t sample_exclusion = UINT32_MAX;
	bool alpha_to_mask_enabled = true;
};

enum class ReducedEqaa2xResolveDecision {
	Disabled, NotExistingResolveSource, WrongCounts, InvalidOrAliasedAddress,
	DifferentFormats, UnsupportedLayoutOrExtent, MissingRawState, NonFullMask,
	AlphaToCoverage, AdmitResolveApproximation
};

inline const char* ReducedEqaa2xResolveDecisionName(ReducedEqaa2xResolveDecision value) {
	switch (value) {
	case ReducedEqaa2xResolveDecision::Disabled: return "resolve_extension_disabled";
	case ReducedEqaa2xResolveDecision::NotExistingResolveSource: return "requires_existing_mode3_copy_source0";
	case ReducedEqaa2xResolveDecision::WrongCounts: return "requires_4coverage_2stored_to_1coverage_1stored";
	case ReducedEqaa2xResolveDecision::InvalidOrAliasedAddress: return "source_destination_address_invalid_or_identical";
	case ReducedEqaa2xResolveDecision::DifferentFormats: return "source_destination_raw_color_format_differs";
	case ReducedEqaa2xResolveDecision::UnsupportedLayoutOrExtent: return "resolve_layout_view_or_extent_outside_profile";
	case ReducedEqaa2xResolveDecision::MissingRawState: return "raw_sample_state_incomplete";
	case ReducedEqaa2xResolveDecision::NonFullMask: return "sample_mask_or_exclusion_sensitive";
	case ReducedEqaa2xResolveDecision::AlphaToCoverage: return "alpha_to_coverage_enabled";
	case ReducedEqaa2xResolveDecision::AdmitResolveApproximation: return "experimental_native_2_to_1_initial_contents_unknown";
	}
	return "unknown";
}

// Fixed-function mode3 has no prepared PS: none is invented or required here.
// This permits only an existing2->1 native resolve. It neither imports/expands
// guest compressed data nor claims equivalence to the native4-coverage EQAA resolve.
inline ReducedEqaa2xResolveDecision ClassifyReducedEqaa2xResolve(
    const ReducedEqaa2xResolveInputs& in) {
	if (!in.requested) return ReducedEqaa2xResolveDecision::Disabled;
	if (!in.existing_resolve_source_path || in.color_mode != 3 || in.color_rop != 0xcc)
		return ReducedEqaa2xResolveDecision::NotExistingResolveSource;
	if (in.source_encoded_coverage != 2 || in.source_encoded_fragments != 1 ||
	    in.destination_encoded_coverage != 0 || in.destination_encoded_fragments != 0)
		return ReducedEqaa2xResolveDecision::WrongCounts;
	if (in.source_address == 0 || in.destination_address == 0 ||
	    in.source_address == in.destination_address)
		return ReducedEqaa2xResolveDecision::InvalidOrAliasedAddress;
	if (in.source_format != in.destination_format)
		return ReducedEqaa2xResolveDecision::DifferentFormats;
	if (!in.source_2d_single_mip_tiled || !in.destination_2d_base_mip || in.destination_levels == 0 || in.destination_levels > 16 ||
	    !in.view_bounds_known || in.source_layers != 1 || in.destination_layers != 1 || in.source_width == 0 || in.source_height == 0 ||
	    in.destination_width == 0 || in.destination_height == 0 ||
	    in.source_width > 16384 || in.source_height > 16384 ||
	    in.destination_width != in.source_width || in.destination_height != in.source_height)
		return ReducedEqaa2xResolveDecision::UnsupportedLayoutOrExtent;
	if (!in.raw_sample_state_complete) return ReducedEqaa2xResolveDecision::MissingRawState;
	if (in.aa_mask_low != UINT32_MAX || in.aa_mask_high != UINT32_MAX || in.sample_exclusion != 0)
		return ReducedEqaa2xResolveDecision::NonFullMask;
	if (in.alpha_to_mask_enabled) return ReducedEqaa2xResolveDecision::AlphaToCoverage;
	return ReducedEqaa2xResolveDecision::AdmitResolveApproximation;
}

} // namespace Libs::Graphics::ShaderRecompiler::Diagnostics
