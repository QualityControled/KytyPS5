#include "graphics/shader/recompiler/PixelSampleSensitivity.h"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace IR = Libs::Graphics::ShaderRecompiler::IR;
namespace D = Libs::Graphics::ShaderRecompiler::Diagnostics;
namespace Dec = Libs::Graphics::ShaderRecompiler::Decoder;
using Libs::Graphics::ShaderType;

void Check(bool condition, const char* text) {
	if (!condition) throw std::runtime_error(text);
}

void DefaultOffAndNonPixel() {
	IR::ShaderInfo info;
	info.inputs.push_back({.kind = static_cast<IR::StageInputKind>(UINT32_MAX)});
	IR::ResourceSpecialization spec;
	std::array<IR::MemoryInfo, 1> memory {{{.kind = IR::ResourceKind::Global}}};
	auto off = D::CollectPixelSampleSensitivity(ShaderType::Pixel, false, info, memory, spec);
	D::ObservePixelSampleOpcode(off, IR::ValueOpcode::ImageRead);
	Check(off == IR::PixelSampleSensitivity {}, "disabled collection retained source facts");
	Check(D::CollectPixelSampleSensitivity(ShaderType::Compute, true, info, memory, spec) ==
	          IR::PixelSampleSensitivity {}, "non-pixel shader was classified as pixel");
}

void InputsAndExports() {
	IR::ShaderInfo info;
	for (const auto kind : {IR::StageInputKind::SampleId, IR::StageInputKind::PackedAncillary,
	                       IR::StageInputKind::BaryCoordSmoothCentroid, IR::StageInputKind::FragCoord})
		info.inputs.push_back({.kind = kind});
	info.outputs.push_back({.kind = IR::StageOutputKind::SampleMask});
	info.outputs.push_back({.kind = IR::StageOutputKind::Depth});
	IR::ResourceSpecialization spec;
	const auto original = info;
	const auto out = D::CollectPixelSampleSensitivity(ShaderType::Pixel, true, info, {}, spec);
	Check(out.captured && out.complete && out.sample_id && out.packed_ancillary &&
	          out.centroid_input && out.sample_mask_export, "sample inputs/exports were lost");
	Check(out.input_kind_mask == ((1u << 9) | (1u << 7) | (1u << 11) | (1u << 5)) &&
	          out.output_kind_mask == ((1u << 3) | (1u << 4)), "input/export bit positions changed");
	Check(info == original, "summary mutated shader source metadata");
	info.inputs.push_back({.kind = static_cast<IR::StageInputKind>(100)});
	info.outputs.push_back({.kind = static_cast<IR::StageOutputKind>(100)});
	const auto bad = D::CollectPixelSampleSensitivity(ShaderType::Pixel, true, info, {}, spec);
	Check(!bad.complete && bad.unknown_inputs == 1 && bad.unknown_outputs == 1 && bad.sample_id,
	      "unrecognized source input/output was silently treated as absent");
}

void AllMaterializedCandidatesAndUnknowns() {
	IR::ShaderInfo info;
	info.images.resize(1);
	IR::ResourceSpecialization spec;
	spec.images.resize(3);
	spec.images[0].dimension = Dec::ImageDimension::Dim2D;
	spec.images[1].dimension = Dec::ImageDimension::Dim2DMsaa;
	spec.images[1].fmask = true;
	spec.images[2].dimension = Dec::ImageDimension::Dim2DMsaaArray;
	spec.images[2].binding_alias = 1;
	const auto before = spec;
	const auto out = D::CollectPixelSampleSensitivity(ShaderType::Pixel, true, info, {}, spec);
	Check(out.complete && out.original_images == 1 && out.materialized_images == 3 &&
	          out.fmask_candidates == 1 && out.msaa_candidates == 2,
	      "generated or aliased FMASK/MSAA candidate escaped original sensitivity summary");
	Check(spec == before, "classifier mutated specialization");
	spec.images[2].dimension = Dec::ImageDimension::Unknown;
	const auto unknown = D::CollectPixelSampleSensitivity(ShaderType::Pixel, true, info, {}, spec);
	Check(!unknown.complete && unknown.unknown_image_candidates == 1 && unknown.fmask_candidates == 1,
	      "unknown dimension or original FMASK was lost");
	spec.images.clear();
	Check(!D::CollectPixelSampleSensitivity(ShaderType::Pixel, true, info, {}, spec).complete,
	      "missing materialized image root incorrectly classified complete");
}

void IndirectAndAddressFacts() {
	IR::ShaderInfo info;
	info.buffers.resize(2);
	info.buffers[1].indirect_root = 0;
	info.images.resize(2);
	info.images[1].indirect_root = 0;
	info.samplers.resize(2);
	info.samplers[1].indirect_root = 0;
	IR::ResourceSpecialization spec;
	spec.images.resize(2);
	for (auto& image : spec.images) image.dimension = Dec::ImageDimension::Dim2D;
	std::array<IR::MemoryInfo, 8> memory {{
	    {.kind = IR::ResourceKind::ScalarAddress}, {.kind = IR::ResourceKind::Flat},
	    {.kind = IR::ResourceKind::FlatLocal}, {.kind = IR::ResourceKind::Global},
	    {.kind = IR::ResourceKind::Scratch}, {.kind = IR::ResourceKind::IndirectBuffer},
	    {.kind = IR::ResourceKind::Image, .planning_only = true, .dynamic_descriptor_source = 0},
	    {.kind = IR::ResourceKind::Buffer}}};
	const auto before = memory;
	const auto out = D::CollectPixelSampleSensitivity(ShaderType::Pixel, true, info, memory, spec);
	Check(out.complete && out.indirect_buffers == 1 && out.indirect_images == 1 &&
	          out.indirect_samplers == 1 && out.address_memory_rows == 5 &&
	          out.indirect_buffer_rows == 1 && out.dynamic_descriptor_rows == 1,
	      "conservative raw/indirect/planning-only provenance facts omitted");
	Check(memory == before, "summary changed memory metadata");
}

void OpcodesAndWarmRetention() {
	IR::ShaderInfo info;
	info.images.resize(1);
	IR::ResourceSpecialization fmask;
	fmask.images.resize(1);
	fmask.images[0].dimension = Dec::ImageDimension::Dim2D;
	fmask.images[0].fmask = true;
	auto summary = D::CollectPixelSampleSensitivity(ShaderType::Pixel, true, info, {}, fmask);
	for (const auto opcode : {IR::ValueOpcode::ImageRead, IR::ValueOpcode::ImageRead,
	                         IR::ValueOpcode::ImageWrite, IR::ValueOpcode::ImageQueryDimensions,
	                         IR::ValueOpcode::ImageQueryLod, IR::ValueOpcode::ImageSampleRaw,
	                         IR::ValueOpcode::ImageGatherRaw, IR::ValueOpcode::Identity})
		D::ObservePixelSampleOpcode(summary, opcode);
	Check(summary.original_instruction_count == 8 && summary.image_read_count == 2 &&
	          summary.image_write_count == 1 && summary.image_query_dimensions_count == 1 &&
	          summary.image_query_lod_count == 1 && summary.image_sample_count == 1 &&
	          summary.image_gather_count == 1, "pre-specialization operation counts lost");
	IR::CompiledShaderInfo first;
	first.pixel_sample_sensitivity = summary;
	first.info.images.clear(); // FMASK compaction may leave no final image binding.
	const auto warm_copy = first;
	Check(warm_copy.info.images.empty() && warm_copy.pixel_sample_sensitivity.fmask_candidates == 1,
	      "compiled permutation copy lost pre-compaction FMASK evidence");
	fmask.images[0].fmask = false;
	IR::CompiledShaderInfo second;
	second.pixel_sample_sensitivity = D::CollectPixelSampleSensitivity(ShaderType::Pixel, true, info, {}, fmask);
	Check(second.pixel_sample_sensitivity.fmask_candidates == 0 &&
	          first.pixel_sample_sensitivity.fmask_candidates == 1,
	      "different materialized permutation overwrote earlier summary");
}

void PreCollectionBuiltinsAndExports() {
	IR::ShaderInfo info; // Actual TranslateProgram has not collected interface lists yet.
	IR::ResourceSpecialization spec;
	auto out = D::CollectPixelSampleSensitivity(ShaderType::Pixel, true, info, {}, spec);
	for (const auto kind : {IR::StageInputKind::SampleId, IR::StageInputKind::PackedAncillary,
	                       IR::StageInputKind::BaryCoordSmoothCentroid})
		D::ObservePixelSampleBuiltin(out, true, static_cast<uint32_t>(kind));
	IR::ExportInfo exp {.kind = IR::ExportTargetKind::MrtZ, .en = 5};
	D::ObservePixelSampleExport(out, &exp);
	Check(out.complete && out.sample_id && out.packed_ancillary && out.centroid_input &&
	          out.sample_mask_export && info.inputs.empty() && info.outputs.empty(),
	      "pre-collection direct builtin/export dependencies escaped summary");
	D::ObservePixelSampleBuiltin(out, false, 0);
	D::ObservePixelSampleExport(out, nullptr);
	Check(!out.complete && out.unknown_inputs == 1 && out.unknown_outputs == 1,
	      "invalid original builtin/export metadata silently treated absent");
}

int main(int argc, char** argv) {
	try {
		if (argc == 3 && std::strcmp(argv[1], "--environment-proof") == 0) {
			const auto expected = std::strcmp(argv[2], "1") == 0;
			Check(D::EqaaShaderCaptureEnabled() == expected, "exact env=1 gate changed");
			_putenv(expected ? "KYTY_CAPTURE_EQAA_SHADER_STATE=0" : "KYTY_CAPTURE_EQAA_SHADER_STATE=1");
			Check(D::EqaaShaderCaptureEnabled() == expected, "diagnostic process mode was not frozen");
			std::puts("PASS exact env gate and frozen process mode");
			return 0;
		}
		DefaultOffAndNonPixel();
		InputsAndExports();
		AllMaterializedCandidatesAndUnknowns();
		IndirectAndAddressFacts();
		OpcodesAndWarmRetention();
		PreCollectionBuiltinsAndExports();
		std::puts("PASS 6 pure pixel-sensitivity summary groups (no guest reader/runtime/GPU)");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
