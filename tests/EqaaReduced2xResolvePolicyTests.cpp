#include "graphics/shader/recompiler/EqaaReduced2xResolvePolicy.h"

#include <cstdio>
#include <stdexcept>

namespace D = Libs::Graphics::ShaderRecompiler::Diagnostics;

static unsigned checks = 0;
void Check(bool value, const char* text) {
	++checks;
	if (!value) throw std::runtime_error(text);
}

int main(int argc, char** argv) {
	try {
		if (argc == 3 && std::strcmp(argv[1], "--environment-proof") == 0) {
			const bool expected = std::strcmp(argv[2], "1") == 0;
			Check(D::EqaaReduced2xResolveRequested() == expected, "resolve mode did not require both exact flags=1");
			_putenv(expected ? "KYTY_EXPERIMENTAL_EQAA_2X_RESOLVE=0" : "KYTY_EXPERIMENTAL_EQAA_2X_RESOLVE=1");
			_putenv(expected ? "KYTY_EXPERIMENTAL_EQAA_2X=0" : "KYTY_EXPERIMENTAL_EQAA_2X=1");
			Check(D::EqaaReduced2xResolveRequested() == expected, "resolve process mode was not frozen");
			std::puts("PASS exact resolve+base flags and frozen mode");
			return 0;
		}
		using I = D::ReducedEqaa2xResolveInputs;
		using E = D::ReducedEqaa2xResolveDecision;
		const I base {
		    .requested = true, .existing_resolve_source_path = true, .color_mode = 3, .color_rop = 0xcc,
		    .source_encoded_coverage = 2, .source_encoded_fragments = 1,
		    .destination_encoded_coverage = 0, .destination_encoded_fragments = 0,
		    .source_address = 0x100000, .destination_address = 0x200000,
		    .source_format = {0xa, 0, 0}, .destination_format = {0xa, 0, 0},
		    .source_2d_single_mip_tiled = true, .destination_2d_base_mip = true, .destination_levels = 4, .view_bounds_known = true,
		    .source_layers = 1, .destination_layers = 1,
		    .source_width = 512, .source_height = 512, .destination_width = 512, .destination_height = 512,
		    .raw_sample_state_complete = true, .aa_mask_low = UINT32_MAX, .aa_mask_high = UINT32_MAX,
		    .sample_exclusion = 0, .alpha_to_mask_enabled = false};
		Check(D::ClassifyReducedEqaa2xResolve(base) == E::AdmitResolveApproximation, "valid owned2->1 profile rejected without PS");
		auto in = base; in.destination_width = 400; in.destination_height = 300;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::UnsupportedLayoutOrExtent, "smaller destination broadened fixed base-mip profile");
		for(uint32_t levels : {0u,17u}) {
			in = base; in.destination_levels = levels;
			Check(D::ClassifyReducedEqaa2xResolve(in) == E::UnsupportedLayoutOrExtent, "invalid destination mip chain admitted");
		}
		in = base; in.requested = false;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::Disabled, "defaultoff resolve admitted");
		in = base; in.existing_resolve_source_path = false;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::NotExistingResolveSource, "ordinary/source1/untyped path admitted");
		in = base; in.color_mode = 1;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::NotExistingResolveSource, "ordinary draw bypassed PS policy");
		in = base; in.color_rop = 0;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::NotExistingResolveSource, "noncopy resolve operation admitted");
		for (uint32_t I::* field : {&I::source_encoded_coverage, &I::source_encoded_fragments,
		    &I::destination_encoded_coverage, &I::destination_encoded_fragments}) {
			in = base; in.*field = 3;
			Check(D::ClassifyReducedEqaa2xResolve(in) == E::WrongCounts, "other sample/storage counts admitted");
		}
		for (uint64_t I::* field : {&I::source_address, &I::destination_address}) {
			in = base; in.*field = 0;
			Check(D::ClassifyReducedEqaa2xResolve(in) == E::InvalidOrAliasedAddress, "null source/destination admitted");
		}
		in = base; in.destination_address = in.source_address;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::InvalidOrAliasedAddress, "same-address sample reinterpretation admitted");
		for (unsigned component = 0; component < 3; ++component) {
			in = base; in.destination_format[component] ^= 1u;
			Check(D::ClassifyReducedEqaa2xResolve(in) == E::DifferentFormats, "format/numeric/order reinterpretation admitted");
		}
		for (bool I::* field : {&I::source_2d_single_mip_tiled, &I::destination_2d_base_mip, &I::view_bounds_known}) {
			in = base; in.*field = false;
			Check(D::ClassifyReducedEqaa2xResolve(in) == E::UnsupportedLayoutOrExtent, "unsupported layout/view admitted");
		}
		for (uint32_t I::* field : {&I::source_width, &I::source_height, &I::destination_width, &I::destination_height}) {
			in = base; in.*field = 0;
			Check(D::ClassifyReducedEqaa2xResolve(in) == E::UnsupportedLayoutOrExtent, "zero/overflowed extent admitted");
		}
		for (uint32_t I::* field : {&I::source_layers, &I::destination_layers}) {
			in = base; in.*field = 0;
			Check(D::ClassifyReducedEqaa2xResolve(in) == E::UnsupportedLayoutOrExtent, "empty layer view admitted");
		}
		in = base; in.destination_layers = 2;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::UnsupportedLayoutOrExtent, "resolve would truncate requested destination layers");
		in.source_layers = 2;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::UnsupportedLayoutOrExtent, "multiple-layer view admitted despite existing one-layer resolve");
		for (uint32_t I::* field : {&I::source_width, &I::source_height}) {
			in = base; in.*field = 16385;
			Check(D::ClassifyReducedEqaa2xResolve(in) == E::UnsupportedLayoutOrExtent, "oversized source extent admitted");
		}
		in = base; in.destination_width = in.source_width + 1;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::UnsupportedLayoutOrExtent, "destination width outside source admitted");
		in = base; in.destination_height = in.source_height + 1;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::UnsupportedLayoutOrExtent, "destination height outside source admitted");
		in = base; in.raw_sample_state_complete = false;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::MissingRawState, "missing retained raw state admitted");
		for (uint32_t I::* field : {&I::aa_mask_low, &I::aa_mask_high, &I::sample_exclusion}) {
			in = base; in.*field ^= 1u;
			Check(D::ClassifyReducedEqaa2xResolve(in) == E::NonFullMask, "partial mask/exclusion resolve admitted");
		}
		in = base; in.alpha_to_mask_enabled = true;
		Check(D::ClassifyReducedEqaa2xResolve(in) == E::AlphaToCoverage, "A2C-dependent state admitted");
		Check(base.source_address == 0x100000 && base.source_format[0] == 0xa,
		      "policy changed resolve input association");
		std::printf("PASS %u pure resolve admission checks; no guest memory, Vulkan, GPU or PS substitution\n", checks);
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
