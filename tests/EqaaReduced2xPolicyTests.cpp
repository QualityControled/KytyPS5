#include "graphics/shader/recompiler/EqaaReduced2xPolicy.h"

#include <cstdio>
#include <stdexcept>

namespace IR = Libs::Graphics::ShaderRecompiler::IR;
namespace D = Libs::Graphics::ShaderRecompiler::Diagnostics;

void Check(bool condition, const char* text) {
	if (!condition) throw std::runtime_error(text);
}

int main() {
	try {
		using Decision = D::ReducedEqaa2xDecision;
		Check(!D::RawAlphaToMaskEnabled(0x00016c00u) &&
		          D::RawAlphaToMaskEnabled(0x00016c01u) &&
		          !D::RawAlphaToMaskEnabled(0xfffffffeu), "PAL alpha-to-mask enable bit misdecoded");
		IR::PixelSampleSensitivity plain {.captured = true, .complete = true};
		const D::ReducedEqaa2xInputs base {
		    .requested = true, .raw_sample_state_complete = true,
		    .encoded_coverage_samples = 2, .encoded_color_fragments = 1, .encoded_depth_samples = 1,
		    .aa_mask_low = UINT32_MAX, .aa_mask_high = UINT32_MAX, .sample_exclusion = 0,
		    .alpha_to_mask_enabled = false, .ps_sample_shading = false,
		    .ps_sample_mask_export = false, .ps_ancillary = false, .pixel = &plain};
		Check(D::ClassifyReducedEqaa2x(base) == Decision::AdmitApproximation,
		      "narrow native-two-sample profile refused");
		auto in = base; in.requested = false;
		Check(D::ClassifyReducedEqaa2x(in) == Decision::Disabled, "default profile admitted");
		for (uint32_t D::ReducedEqaa2xInputs::* field : {&D::ReducedEqaa2xInputs::encoded_coverage_samples,
		     &D::ReducedEqaa2xInputs::encoded_color_fragments, &D::ReducedEqaa2xInputs::encoded_depth_samples}) {
			in = base; in.*field = 3;
			Check(D::ClassifyReducedEqaa2x(in) == Decision::WrongCounts, "other sample profile admitted");
		}
		in = base; in.raw_sample_state_complete = false;
		Check(D::ClassifyReducedEqaa2x(in) == Decision::MissingRawState, "missing raw state admitted");
		for (uint32_t D::ReducedEqaa2xInputs::* field : {&D::ReducedEqaa2xInputs::aa_mask_low,
		     &D::ReducedEqaa2xInputs::aa_mask_high, &D::ReducedEqaa2xInputs::sample_exclusion}) {
			in = base; in.*field ^= 1u;
			Check(D::ClassifyReducedEqaa2x(in) == Decision::NonFullMask, "partial sample mask/exclusion admitted");
		}
		in = base; in.alpha_to_mask_enabled = true;
		Check(D::ClassifyReducedEqaa2x(in) == Decision::AlphaToCoverage, "alpha coverage admitted");
		in = base; in.pixel = nullptr;
		Check(D::ClassifyReducedEqaa2x(in) == Decision::MissingPixelSummary, "missing PS admitted");
		for (bool IR::PixelSampleSensitivity::* field : {&IR::PixelSampleSensitivity::captured,
		     &IR::PixelSampleSensitivity::complete}) {
			auto sensitive = plain; sensitive.*field = false; in = base; in.pixel = &sensitive;
			Check(D::ClassifyReducedEqaa2x(in) == Decision::MissingPixelSummary, "unknown original summary admitted");
		}
		for (bool D::ReducedEqaa2xInputs::* field : {&D::ReducedEqaa2xInputs::ps_sample_shading,
		     &D::ReducedEqaa2xInputs::ps_sample_mask_export, &D::ReducedEqaa2xInputs::ps_ancillary}) {
			in = base; in.*field = true;
			Check(D::ClassifyReducedEqaa2x(in) == Decision::PixelSampleDependent, "prepared sample-sensitive PS admitted");
		}
		for (bool IR::PixelSampleSensitivity::* field : {&IR::PixelSampleSensitivity::sample_id,
		     &IR::PixelSampleSensitivity::packed_ancillary,
		     &IR::PixelSampleSensitivity::sample_mask_export}) {
			auto sensitive = plain; sensitive.*field = true; in = base; in.pixel = &sensitive;
			Check(D::ClassifyReducedEqaa2x(in) == Decision::PixelSampleDependent, "original sample input/export admitted");
		}
		auto centroid = plain; centroid.centroid_input = true; in = base; in.pixel = &centroid;
		Check(D::ClassifyReducedEqaa2x(in) == Decision::AdmitApproximation,
		      "declared centroid coverage-quality approximation was refused");
		for (uint32_t IR::PixelSampleSensitivity::* field : {&IR::PixelSampleSensitivity::fmask_candidates,
		     &IR::PixelSampleSensitivity::msaa_candidates, &IR::PixelSampleSensitivity::unknown_image_candidates}) {
			auto sensitive = plain; sensitive.*field = 1; in = base; in.pixel = &sensitive;
			Check(D::ClassifyReducedEqaa2x(in) == Decision::PixelSampleDependent, "FMASK/MSAA/unknown candidate admitted");
		}
		for (uint32_t IR::PixelSampleSensitivity::* field : {&IR::PixelSampleSensitivity::indirect_images,
		     &IR::PixelSampleSensitivity::indirect_buffers, &IR::PixelSampleSensitivity::indirect_samplers,
		     &IR::PixelSampleSensitivity::address_memory_rows, &IR::PixelSampleSensitivity::indirect_buffer_rows,
		     &IR::PixelSampleSensitivity::dynamic_descriptor_rows}) {
			auto sensitive = plain; sensitive.*field = 1; in = base; in.pixel = &sensitive;
			Check(D::ClassifyReducedEqaa2x(in) == Decision::IndirectAddressDependency, "unbounded raw/descriptor alias admitted");
		}
		Check(plain == IR::PixelSampleSensitivity {.captured = true, .complete = true},
		      "admission modified original sensitivity metadata");
		std::puts("PASS 31 reduced-2x classifier checks (declared approximation, no Vulkan/GPU)");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
