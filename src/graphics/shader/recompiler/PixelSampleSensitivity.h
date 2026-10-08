#pragma once

#include "graphics/shader/recompiler/EqaaFactCollection.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <span>

namespace Libs::Graphics::ShaderRecompiler::Diagnostics {

// No guest memory, resource-plan evaluation, callbacks or executable IR mutation.
// All materialized candidates are included conservatively, including generated
// indirect children and unused metadata rows. Unknowns remain explicit.
inline IR::PixelSampleSensitivity CollectPixelSampleSensitivity(
    ShaderType stage, bool enabled, const IR::ShaderInfo& original,
    std::span<const IR::MemoryInfo> memory,
    const IR::ResourceSpecialization& specialization) {
	IR::PixelSampleSensitivity out;
	if (!enabled || stage != ShaderType::Pixel) return out;
	out.captured = true;
	out.complete = true;
	for (const auto& input : original.inputs) {
		const auto kind = static_cast<uint32_t>(input.kind);
		if (kind > static_cast<uint32_t>(IR::StageInputKind::Parameter)) {
			++out.unknown_inputs;
			out.complete = false;
			continue;
		}
		out.input_kind_mask |= 1u << kind;
		out.sample_id |= input.kind == IR::StageInputKind::SampleId;
		out.packed_ancillary |= input.kind == IR::StageInputKind::PackedAncillary;
		out.centroid_input |= input.kind == IR::StageInputKind::BaryCoordSmoothCentroid;
	}
	for (const auto& output : original.outputs) {
		const auto kind = static_cast<uint32_t>(output.kind);
		if (kind > static_cast<uint32_t>(IR::StageOutputKind::ViewportIndex)) {
			++out.unknown_outputs;
			out.complete = false;
			continue;
		}
		out.output_kind_mask |= 1u << kind;
		out.sample_mask_export |= output.kind == IR::StageOutputKind::SampleMask;
	}
	out.original_images = static_cast<uint32_t>(original.images.size());
	out.materialized_images = static_cast<uint32_t>(specialization.images.size());
	if (specialization.images.size() < original.images.size()) out.complete = false;
	for (const auto& image : specialization.images) {
		out.fmask_candidates += image.fmask ? 1u : 0u;
		out.msaa_candidates += image.dimension == Decoder::ImageDimension::Dim2DMsaa ||
		                       image.dimension == Decoder::ImageDimension::Dim2DMsaaArray;
		const auto dimension = static_cast<uint32_t>(image.dimension);
		if (dimension == static_cast<uint32_t>(Decoder::ImageDimension::Unknown) ||
		    dimension > static_cast<uint32_t>(Decoder::ImageDimension::Dim2DMsaaArray)) {
			++out.unknown_image_candidates;
			out.complete = false;
		}
	}
	for (const auto& image : original.images)
		out.indirect_images += image.indirect_root != IR::ImageResource::NoIndirectImage;
	for (const auto& buffer : original.buffers)
		out.indirect_buffers += buffer.indirect_root != IR::BufferResource::NoIndirectBuffer;
	for (const auto& sampler : original.samplers)
		out.indirect_samplers += sampler.indirect_root != IR::SamplerResource::NoIndirectSampler;
	for (const auto& row : memory) {
		out.address_memory_rows += IR::IsAddressResourceKind(row.kind);
		out.indirect_buffer_rows += row.kind == IR::ResourceKind::IndirectBuffer;
		out.dynamic_descriptor_rows += row.dynamic_descriptor_source != UINT32_MAX;
	}
	return out;
}

// Called while the original optimized/tracked IR still exists, before the
// specialization replaces FMASK reads and compacts resource metadata.
inline void ObservePixelSampleOpcode(IR::PixelSampleSensitivity& out, IR::ValueOpcode opcode) {
	if (!out.captured) return;
	++out.original_instruction_count;
	switch (opcode) {
	case IR::ValueOpcode::ImageRead: ++out.image_read_count; break;
	case IR::ValueOpcode::ImageWrite: ++out.image_write_count; break;
	case IR::ValueOpcode::ImageQueryDimensions: ++out.image_query_dimensions_count; break;
	case IR::ValueOpcode::ImageQueryLod: ++out.image_query_lod_count; break;
	case IR::ValueOpcode::ImageSampleRaw: ++out.image_sample_count; break;
	case IR::ValueOpcode::ImageGatherRaw: ++out.image_gather_count; break;
	default: break;
	}
}

// info.inputs/outputs are normally populated after specialization. Read the
// original builtin/export metadata too so an empty interface list is not an
// absence proof and a pre-specialization builtin cannot silently disappear.
inline void ObservePixelSampleBuiltin(IR::PixelSampleSensitivity& out, bool known,
                                      uint32_t kind) {
	if (!out.captured) return;
	if (!known || kind > static_cast<uint32_t>(IR::StageInputKind::Parameter)) {
		++out.unknown_inputs;
		out.complete = false;
		return;
	}
	out.input_kind_mask |= 1u << kind;
	out.sample_id |= kind == static_cast<uint32_t>(IR::StageInputKind::SampleId);
	out.packed_ancillary |= kind == static_cast<uint32_t>(IR::StageInputKind::PackedAncillary);
	out.centroid_input |= kind == static_cast<uint32_t>(IR::StageInputKind::BaryCoordSmoothCentroid);
}

inline void ObservePixelSampleExport(IR::PixelSampleSensitivity& out,
                                     const IR::ExportInfo* exp) {
	if (!out.captured) return;
	if (exp == nullptr) {
		++out.unknown_outputs;
		out.complete = false;
		return;
	}
	if (exp->kind == IR::ExportTargetKind::MrtZ) {
		if ((exp->en & 1u) != 0)
			out.output_kind_mask |= 1u << static_cast<uint32_t>(IR::StageOutputKind::Depth);
		if ((exp->en & 4u) != 0) {
			out.output_kind_mask |= 1u << static_cast<uint32_t>(IR::StageOutputKind::SampleMask);
			out.sample_mask_export = true;
		}
	} else if (exp->kind == IR::ExportTargetKind::Mrt && exp->en != 0) {
		out.output_kind_mask |= 1u << static_cast<uint32_t>(IR::StageOutputKind::Mrt);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Diagnostics
