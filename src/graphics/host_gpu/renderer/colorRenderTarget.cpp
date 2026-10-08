#include "graphics/host_gpu/renderer/colorRenderTarget.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>

namespace Libs::Graphics {

static bool DccAlphaOnMsb(const HW::ColorInfo& info) {
	switch (info.format) {
		case Prospero::ChannelLayout::k10_10_10_2:
		case Prospero::ChannelLayout::k10_10_10_2Float:
		case Prospero::ChannelLayout::k5_5_5_1: return true;
		case Prospero::ChannelLayout::k2_10_10_10:
		case Prospero::ChannelLayout::k1_5_5_5: return false;
		default: break;
	}
	const auto components =
	    Prospero::ResolveRenderTargetFormat(info.format, info.channel_type).components;
	if (components == 1) {
		return info.channel_order != Prospero::ChannelOrder::kStandard;
	}
	return components == 3 || info.channel_order == Prospero::ChannelOrder::kStandard ||
	       info.channel_order == Prospero::ChannelOrder::kAlt;
}

// Diagnostic only: called from the existing unsupported-sample branch.
// No guest memory, image/cache lookup, GPU operation, or register writes.
static void PrintUnsupportedColorSamples(const CommandBuffer& buffer, uint32_t slot,
                                         uint32_t effective_mask, uint32_t slice_offset,
                                         bool ignore_target_mask, bool exact_format) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	const auto& hw = buffer.GetRegisters();
	const auto& rt = hw.GetRenderTarget(slot);
	const auto& cc = hw.GetColorControl();
	std::printf("ColorSampleDiagnostic schema=1 slot=%u mode=%u rop=0x%02x "
	            "ignore_target_mask=%u exact_format=%u slice_offset=%u "
	            "target_mask=0x%08x effective_slot_mask=0x%x "
	            "samples_encoded=%u fragments_encoded=%u coverage_count=%u fragment_count=%u\n",
	            slot, static_cast<unsigned>(cc.mode), static_cast<unsigned>(cc.op),
	            static_cast<unsigned>(ignore_target_mask), static_cast<unsigned>(exact_format),
	            slice_offset, hw.GetRenderTargetMask(), effective_mask, rt.attrib.num_samples,
	            rt.attrib.num_fragments,
	            rt.attrib.num_samples <= 7 ? (1u << rt.attrib.num_samples) : 0u,
	            render_sample_count(rt.attrib.num_fragments));
	std::printf("ColorSampleDiagnostic original_cb_raw_registers=not_retained "
	            "aa_mask_registers=not_retained sample_exclusion_mask=not_retained "
	            "draw_debug_tuple=private_unavailable\n");
	for (uint32_t i = 0; i < 8; ++i) {
		std::printf("ColorSampleDiagnostic target_slot=%u slot_mask=0x%x failing=%u\n", i,
		            render_target_mask_slot(hw.GetRenderTargetMask(), i),
		            static_cast<unsigned>(i == slot));
		std::printf("%s", rt_print("DecodedRenderTarget:", hw.GetRenderTarget(i)).c_str());
	}
	const auto& eq = hw.GetEqaaControl();
	const auto& ac = hw.GetAaConfig();
	const auto& aa = hw.GetAaSampleControl();
	const auto& sc = hw.GetScanModeControl();
	std::printf("EQAA max_anchor_samples=%u ps_iter_samples=%u mask_export_num_samples=%u "
	            "alpha_to_mask_num_samples=%u high_quality_intersections=%u "
	            "incoherent_eqaa_reads=%u interpolate_comp_z=%u static_anchor_associations=%u\n",
	            static_cast<unsigned>(eq.max_anchor_samples),
	            static_cast<unsigned>(eq.ps_iter_samples),
	            static_cast<unsigned>(eq.mask_export_num_samples),
	            static_cast<unsigned>(eq.alpha_to_mask_num_samples),
	            static_cast<unsigned>(eq.high_quality_intersections),
	            static_cast<unsigned>(eq.incoherent_eqaa_reads),
	            static_cast<unsigned>(eq.interpolate_comp_z),
	            static_cast<unsigned>(eq.static_anchor_associations));
	std::printf(
	    "AA msaa_num_samples=%u exposed_samples=%u mask_centroid_dtmn=%u max_sample_dist=%u "
	    "scan_msaa_enable=%u vport_scissor_enable=%u line_stipple_enable=%u "
		"centroid_priority=0x%016" PRIx64 "\n",
	    static_cast<unsigned>(ac.msaa_num_samples), static_cast<unsigned>(ac.msaa_exposed_samples),
	    static_cast<unsigned>(ac.aa_mask_centroid_dtmn), static_cast<unsigned>(ac.max_sample_dist),
	    static_cast<unsigned>(sc.msaa_enable), static_cast<unsigned>(sc.vport_scissor_enable),
	    static_cast<unsigned>(sc.line_stipple_enable), aa.centroid_priority);
	for (uint32_t i = 0; i < 16; ++i) {
		std::printf("AA sample_location[%u]=0x%08x\n", i, aa.locations[i]);
	}
	const auto& z = hw.GetDepthRenderTarget();
	std::printf("Depth format=%u samples_encoded=%u texture_compatibility=%u z_compare_base=%u "
	            "htile_acceleration=%u expclear=%u partially_resident=%u max_mip_level=%u "
	            "stencil_format=%u stencil_texture_compatibility=%u stencil_expclear=%u "
	            "htile_stencil_disabled=%u stencil_partially_resident=%u\n",
	            static_cast<unsigned>(z.z_info.format), z.z_info.num_samples,
	            static_cast<unsigned>(z.z_info.texture_compatibility),
	            static_cast<unsigned>(z.z_info.z_compare_base),
	            static_cast<unsigned>(z.z_info.htile_acceleration),
	            static_cast<unsigned>(z.z_info.expclear_enabled),
	            static_cast<unsigned>(z.z_info.partially_resident),
	            static_cast<unsigned>(z.z_info.max_mip_level),
	            static_cast<unsigned>(z.stencil_info.format),
	            static_cast<unsigned>(z.stencil_info.texture_compatibility),
	            static_cast<unsigned>(z.stencil_info.expclear_enabled),
	            static_cast<unsigned>(z.stencil_info.htile_stencil_disabled),
	            static_cast<unsigned>(z.stencil_info.partially_resident));
	std::printf(
	    "Depth z_read_base=0x%016" PRIx64 " z_write_base=0x%016" PRIx64
	    " stencil_read_base=0x%016" PRIx64 " stencil_write_base=0x%016" PRIx64
	    " htile_base=0x%016" PRIx64 " shading_rate_encoding=%u "
	    "slice_start=%u slice_max=%u mip=%u depth_write_disable=%u stencil_write_disable=%u "
	    "size_x_max=%u size_y_max=%u size_valid=%u\n",
	    z.z_read_base_addr, z.z_write_base_addr, z.stencil_read_base_addr,
	    z.stencil_write_base_addr, z.htile_data_base_addr,
	    static_cast<unsigned>(z.shading_rate_encoding), z.depth_view.slice_start,
	    z.depth_view.slice_max, static_cast<unsigned>(z.depth_view.current_mip_level),
	    static_cast<unsigned>(z.depth_view.depth_write_disable),
	    static_cast<unsigned>(z.depth_view.stencil_write_disable),
	    static_cast<unsigned>(z.size.x_max), static_cast<unsigned>(z.size.y_max),
	    static_cast<unsigned>(z.size.valid));
	const auto& dc = hw.GetDepthControl();
	const auto& rc = hw.GetRenderControl();
	const auto& ov = hw.GetDepthRenderOverride();
	std::printf("DepthControl stencil_enable=%u z_enable=%u z_write_enable=%u bounds_enable=%u "
	            "zfunc=%u backface_enable=%u stencilfunc=%u stencilfunc_bf=%u\n",
	            static_cast<unsigned>(dc.stencil_enable), static_cast<unsigned>(dc.z_enable),
	            static_cast<unsigned>(dc.z_write_enable),
	            static_cast<unsigned>(dc.depth_bounds_enable), static_cast<unsigned>(dc.zfunc),
	            static_cast<unsigned>(dc.backface_enable), static_cast<unsigned>(dc.stencilfunc),
	            static_cast<unsigned>(dc.stencilfunc_bf));
	std::printf(
	    "RenderControl depth_clear=%u stencil_clear=%u resummarize=%u stencil_compress_disable=%u "
	    "depth_compress_disable=%u copy_depth_to_color=%u copy_stencil_to_color=%u "
		"copy_centroid=%u copy_sample=%u "
	    "force_z_valid=%u force_z_dirty=%u force_stencil_valid=%u force_stencil_dirty=%u\n",
	    static_cast<unsigned>(rc.depth_clear_enable),
	    static_cast<unsigned>(rc.stencil_clear_enable),
	    static_cast<unsigned>(rc.resummarize_enable),
	    static_cast<unsigned>(rc.stencil_compress_disable),
	    static_cast<unsigned>(rc.depth_compress_disable),
	    static_cast<unsigned>(rc.copy_depth_to_color),
	    static_cast<unsigned>(rc.copy_stencil_to_color), static_cast<unsigned>(rc.copy_centroid),
	    static_cast<unsigned>(rc.copy_sample), static_cast<unsigned>(ov.force_z_valid),
	    static_cast<unsigned>(ov.force_z_dirty), static_cast<unsigned>(ov.force_stencil_valid),
	    static_cast<unsigned>(ov.force_stencil_dirty));
	const auto& sh = hw.GetShaderRegisters();
	const auto& db = sh.db_shader_control;
	std::printf("ShaderContext stages=0x%08x cb_shader_mask=0x%08x pa_sc_shader_control=0x%08x "
	            "shader_z_format=0x%08x ps_input_ena=0x%08x ps_input_addr=0x%08x "
	            "ps_in_control=0x%08x baryc_cntl=0x%08x\n",
	            hw.GetShaderStages(), sh.m_cbShaderMask, sh.m_paScShaderControl, sh.shader_z_format,
	            sh.ps_input_ena, sh.ps_input_addr, sh.ps_in_control, sh.baryc_cntl);
	std::printf(
	    "DBShaderControl other_bits=0x%08x conservative_z_export_value=%u shader_z_behavior=%u "
	    "kill=%u z_export=%u mask_export=%u dual_export=%u execute_on_noop=%u "
		"alpha_to_mask_disable=%u\n",
	    db.other_bits, static_cast<unsigned>(db.conservative_z_export_value),
	    static_cast<unsigned>(db.shader_z_behavior), static_cast<unsigned>(db.shader_kill_enable),
	    static_cast<unsigned>(db.shader_z_export_enable),
	    static_cast<unsigned>(db.shader_mask_export_enable),
	    static_cast<unsigned>(db.shader_dual_export_enable),
	    static_cast<unsigned>(db.shader_execute_on_noop),
	    static_cast<unsigned>(db.alpha_to_mask_disable));
	for (uint32_t i = 0; i < 8; ++i) {
		std::printf("ShaderContext target_output_mode[%u]=%u\n", i,
		            static_cast<unsigned>(sh.target_output_mode[i]));
	}
	const auto& ps = buffer.GetShaders().GetPs().ps_regs;
	const auto& vs = buffer.GetShaders().GetVs();
	const auto& uc = buffer.GetUserConfig();
	std::printf("DrawContext primitive=%u index_offset=%u object_id=%u primitive_reset=0x%08x "
	            "ps_program_address=0x%016" PRIx64 " ps_user_data_address=0x%016" PRIx64
	            " ps_aux_table_address=0x%016" PRIx64 " es_program_address=0x%016" PRIx64
	            " gs_program_address=0x%016" PRIx64 " hs_program_address=0x%016" PRIx64
	            " ls_program_address=0x%016" PRIx64 "\n",
	            static_cast<unsigned>(uc.GetPrimType()), uc.GetIndexOffset(), uc.GetObjectId(),
	            uc.GetPrimitiveResetControl(), ps.data_addr, ps.user_data_addr,
	            ps.auxiliary_table_addr, vs.es_regs.data_addr, vs.gs_regs.data_addr,
	            vs.hs_regs.data_addr, vs.ls_regs.data_addr);
	std::printf("ColorSampleDiagnostic end unsupported_configuration_preserved=1\n");
	std::fflush(stdout);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& r,
                                              uint32_t render_target_slice_offset, uint32_t rt_slot,
                                              bool ignore_target_mask, bool exact_format) {
	KYTY_PROFILER_FUNCTION();
	const auto& hw = buffer.GetRegisters();

	const auto& rt   = hw.GetRenderTarget(rt_slot);
	auto        mask = render_target_mask_slot(hw.GetRenderTargetMask(), rt_slot);
	if (ignore_target_mask && rt.base.addr != 0 && mask == 0) {
		mask = 0x0f;
	}

	r             = {};
	r.target_slot = rt_slot;

	if (rt.base.addr == 0 || mask == 0) {
		if (graphics_debug_dump_enabled()) {
			static std::atomic_uint log_count = 0;
			const auto              log_id    = log_count.fetch_add(1, std::memory_order_relaxed);
			if (log_id < 128) {
				LOGF("RenderColorTarget: no color output slot=%" PRIu32 " base=0x%010" PRIx64
				     " slot_mask=0x%01" PRIx32 " target_mask=0x%08" PRIx32
				     " rt_slice_offset=%" PRIu32 "\n",
				     rt_slot, rt.base.addr, mask, hw.GetRenderTargetMask(),
				     render_target_slice_offset);
			}
		}

		return;
	}
	const auto samples = render_sample_count(rt.attrib.num_fragments);
	if (samples == 0 || rt.attrib.num_samples != rt.attrib.num_fragments) {
		PrintUnsupportedColorSamples(buffer, rt_slot, mask, render_target_slice_offset,
		                             ignore_target_mask, exact_format);
		EXIT("unsupported render-target sample configuration: samples=%u fragments=%u\n",
		     rt.attrib.num_samples, rt.attrib.num_fragments);
	}
	const uint32_t levels = rt.attrib2.num_mip_levels + 1u;
	if (levels == 0 || levels > 16 || rt.view.current_mip_level >= levels) {
		EXIT("unsupported render-target mip range: current=%u levels=%u\n",
		     rt.view.current_mip_level, levels);
	}
	static constexpr std::array image_types {Prospero::ImageType::kColor1D,
	                                         Prospero::ImageType::kColor2D,
	                                         Prospero::ImageType::kColor3D};
	if (rt.attrib3.dimension >= image_types.size()) {
		EXIT("unsupported render-target dimension: %u\n", rt.attrib3.dimension);
	}
	const auto image_type = image_types[rt.attrib3.dimension];
	const bool is_1d      = image_type == Prospero::ImageType::kColor1D;
	const bool volume     = image_type == Prospero::ImageType::kColor3D;
	if (is_1d && rt.attrib2.height != 0) {
		EXIT("1D render target has nonzero height: %u\n", rt.attrib2.height);
	}
	if (!volume && rt.attrib3.depth != 0) {
		EXIT("non-3D render target has nonzero depth: %u\n", rt.attrib3.depth);
	}
	if (is_1d && samples != 1) {
		EXIT("multisampled 1D render targets are unsupported\n");
	}
	if (volume && samples != 1) {
		EXIT("multisampled 3D render targets are unsupported\n");
	}
	const uint32_t depth = volume ? rt.attrib3.depth + 1u : 1u;
	// For volumes, CB_COLOR_VIEW bounds exported slices; ATTRIB3 defines storage depth.
	// The host attachment contains only the selected slices that exist in this mip.
	const uint32_t last_layer =
	    volume ? std::min(rt.view.last_array_slice_index,
		                  std::max(depth >> rt.view.current_mip_level, 1u) - 1u)
		       : rt.view.last_array_slice_index;
	const auto view = ResolveTargetViewInfo(rt.view.base_array_slice_index, last_layer,
	                                        render_target_slice_offset);
	switch (view.type) {
		case TargetViewType::Image2D:
		case TargetViewType::Image2DArray: break;
		case TargetViewType::Unsupported:
			EXIT("invalid render-target view: base=%u last=%u draw_offset=%u\n",
			     rt.view.base_array_slice_index, rt.view.last_array_slice_index,
			     render_target_slice_offset);
	}
	if (graphics_debug_dump_enabled()) {
		static std::atomic_uint log_count = 0;
		const auto              log_id    = log_count.fetch_add(1, std::memory_order_relaxed);
		if (log_id < 128) {
			LOGF("RenderColorTarget: inspect slot=%" PRIu32 " base=0x%010" PRIx64
			     " mask=0x%01" PRIx32 " attrib2_width=%" PRIu32 " attrib2_height=%" PRIu32
			     " attrib3_tile=0x%08" PRIx32 " attrib3_dim=0x%08" PRIx32 " fmt=0x%08" PRIx32
			     " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32 "\n",
			     rt_slot, rt.base.addr, mask, rt.attrib2.width, rt.attrib2.height,
			     static_cast<uint32_t>(rt.attrib3.tile_mode), rt.attrib3.dimension,
			     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
			     static_cast<uint32_t>(rt.info.channel_order));
		}
	}

	// Color-control state selects the color-buffer operation and logical blend operation.
	// The normal copy operation is a regular color write, not an attachment clear.
	// Nonlinear clear values are still stored as normalized components.
	// Metadata clears are materialized during image discovery; render-pass loads preserve contents.
	uint32_t   width        = 0;
	uint32_t   height       = 0;
	uint32_t   pitch        = 0;
	uint64_t   size         = 0;
	bool       tile         = false;
	const bool standard4    = rt.attrib3.tile_mode == Prospero::TileMode::kStandard4KB;
	const bool standard64   = rt.attrib3.tile_mode == Prospero::TileMode::kStandard64KB;
	const bool depth_tile   = rt.attrib3.tile_mode == Prospero::TileMode::kDepth;
	const bool texture_tile = standard4 || standard64 || depth_tile;

	switch (rt.attrib3.tile_mode) {
		case Prospero::TileMode::kLinear:
		case Prospero::TileMode::kStandard4KB:
		case Prospero::TileMode::kStandard64KB:
		case Prospero::TileMode::kDepth:
		case Prospero::TileMode::kRenderTarget:
			tile = !RenderIsColorTileModeLinear(rt.attrib3.tile_mode);
			break;
		default: EXIT("unknown tile mode: %u\n", static_cast<uint32_t>(rt.attrib3.tile_mode));
	}
	if (samples > 1 && (!tile || levels != 1)) {
		EXIT("multisampled render targets require a single-mip tiled surface\n");
	}
	if (texture_tile && samples != 1) {
		EXIT("texture-tiled color render targets do not support multisampling\n");
	}

	width  = rt.attrib2.width + 1;
	height = rt.attrib2.height + 1;
	const auto target_format =
	    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type, rt.info.channel_order);
	const auto bytes_per_element = target_format.bytes_per_element;
	if (bytes_per_element == 0) {
		EXIT("render-target format has no valid element size\n");
	}
	const auto transfer_format = ImageOps::RenderTargetTransferFormat(bytes_per_element);
	TileTextureBlockLayout texture_tile_layout {};
	if (texture_tile &&
	    (!TileGetTextureBlockLayout(transfer_format, rt.attrib3.tile_mode, volume,
	                                texture_tile_layout) ||
	     (rt.base.addr & (texture_tile_layout.block.block_size - 1u)) != 0 ||
	     rt.info.fmask_compression_enable || rt.info.fmask_data_compression_disable ||
	     rt.info.fmask_one_frag_mode || rt.info.cmask_fast_clear_enable ||
	     rt.info.dcc_compression_enable || rt.cmask.addr != 0 || rt.fmask.addr != 0 ||
	     rt.dcc_addr.addr != 0 || rt.dcc.data_write_on_dcc_clear_to_reg)) {
		EXIT("unsupported texture-tiled render target: addr=0x%016" PRIx64 " tile=%u"
		     " dimension=%u depth=%u levels=%u layer=%u/%u samples=%u fragments=%u bpe=%u"
		     " cmask=0x%016" PRIx64 " fmask=0x%016" PRIx64 " dcc=0x%016" PRIx64 "\n",
		     rt.base.addr, static_cast<uint32_t>(rt.attrib3.tile_mode), rt.attrib3.dimension,
		     rt.attrib3.depth, levels, view.base_layer, view.image_layers, rt.attrib.num_samples,
		     rt.attrib.num_fragments, bytes_per_element, rt.cmask.addr, rt.fmask.addr,
		     rt.dcc_addr.addr);
	}
	if ((standard64 || depth_tile) &&
	    (rt.attrib3.dimension != 1 || rt.attrib3.depth != 0 ||
	     (depth_tile && (view.base_layer != 0 || view.image_layers != 1)))) {
		EXIT("unsupported 64KB texture-tiled render-target view: dimension=%u depth=%u"
		     " layer=%u/%u\n",
		     rt.attrib3.dimension, rt.attrib3.depth, view.base_layer, view.image_layers);
	}
	if (samples == 1) {
		pitch = TileGetTexturePitch(transfer_format, width, rt.attrib3.tile_mode);
	} else {
		pitch = TileGetRenderTargetPitch(width, bytes_per_element, rt.attrib.num_fragments);
	}
	if (pitch == 0) {
		EXIT("unsupported render-target pitch: width=%u bytes=%u\n", width, bytes_per_element);
	}

	TileSizeOffset    mip_sizes[16] {};
	TilePaddedSize    mip_padded[16] {};
	TileSurfaceLayout volume_layout {};
	uint64_t          backing_size = 0;
	if (volume) {
		const TileSurfaceDescription description {transfer_format,
		                                          rt.attrib3.tile_mode,
		                                          TileSurfaceDimension::Dim3D,
		                                          width,
		                                          height,
		                                          depth,
		                                          levels,
		                                          1};
		if (!tile || !TileGetTiledTextureLayout(description, volume_layout)) {
			EXIT("unsupported 3D render-target layout: %ux%ux%u levels=%u tile=%u\n", width, height,
			     depth, levels, static_cast<uint32_t>(rt.attrib3.tile_mode));
		}
		size         = volume_layout.block_slice_size;
		backing_size = volume_layout.total_size;
	} else {
		TileSizeAlign layout {};
		bool          valid_layout = false;
		if (samples == 1) {
			TileGetTextureSize(transfer_format, width, height, levels, rt.attrib3.tile_mode,
			                   &layout, mip_sizes, mip_padded);
			valid_layout = layout.size != 0 && layout.align != 0 &&
			               (rt.attrib3.tile_mode != Prospero::TileMode::kRenderTarget ||
			                levels <= std::bit_width(std::max(width, height)));
		} else {
			valid_layout  = TileGetRenderTargetSize(width, height, pitch, bytes_per_element, layout,
			                                        rt.attrib.num_fragments);
			mip_sizes[0]  = {layout.size, 0, 0, 0, 0, 0};
			mip_padded[0] = {pitch, height};
		}
		if (!valid_layout) {
			EXIT("unsupported render-target layout: %ux%u pitch=%u bytes=%u levels=%u\n", width,
			     height, pitch, bytes_per_element, levels);
		}
		size = layout.size;
	}
	if (size == 0 || (!volume && size > UINT64_MAX / view.image_layers)) {
		EXIT("render-target memory footprint is invalid\n");
	}
	if (!volume) {
		backing_size = size * view.image_layers;
	}
	if (backing_size == 0) {
		EXIT("render-target backing is empty\n");
	}
	if (!GuestRange {rt.base.addr, backing_size}.Valid()) {
		EXIT("render-target backing range is invalid\n");
	}

	auto& desc                = r.desc;
	desc.type                 = TextureCache::BindingType::RenderTarget;
	desc.info.data            = {rt.base.addr, backing_size};
	desc.info.pixel_format    = target_format.format;
	desc.info.guest_format    = target_format.guest_format;
	desc.info.type            = image_type;
	desc.info.extent          = {width, height, depth};
	desc.info.resources       = {levels, volume ? 1u : view.image_layers};
	desc.info.pitch           = pitch;
	desc.info.bytes_per_block = bytes_per_element;
	desc.info.samples         = samples;
	desc.info.tile_mode       = rt.attrib3.tile_mode;
	const bool has_dcc        = rt.info.dcc_compression_enable && rt.dcc_addr.addr != 0;
	const bool has_cmask = !rt.info.dcc_compression_enable && rt.info.cmask_fast_clear_enable &&
	                       rt.cmask.addr != 0 && samples == 1 &&
	                       !rt.info.fmask_compression_enable &&
	                       !rt.attrib3.write_vrs_rate_hint_to_cmask;
	if (has_dcc || has_cmask) {
		TileSizeAlign metadata_size {};
		const auto    layers = volume ? depth : view.image_layers;
		if (has_dcc) {
			(void)TileGetDccSize(width, height, layers, bytes_per_element, levels,
			                     rt.attrib3.tile_mode, metadata_size, rt.attrib.num_fragments);
			desc.info.metadata.dcc_alpha_msb = DccAlphaOnMsb(rt.info);
		} else {
			(void)TileGetCmaskSize(width, height, layers, levels, metadata_size);
		}
		// DCC owns the clear when both planes are enabled; single-sample CMASK stays expanded.
		desc.info.metadata.kind  = has_dcc ? ImageMetadataKind::Dcc : ImageMetadataKind::Cmask;
		desc.info.metadata.range = {has_dcc ? rt.dcc_addr.addr : rt.cmask.addr, metadata_size.size};
		desc.info.metadata.clear_word           = rt.clear_word0.word0;
		desc.info.metadata.clear_register_valid = true;
	}
	for (uint32_t level = 0; level < levels; level++) {
		if (volume) {
			const auto& mip             = volume_layout.mips[level];
			desc.info.mip_layout[level] = {
			    mip.offset,
			    mip.size,
			    mip.padded_width,
			    mip.padded_height,
			};
			continue;
		}
		const auto level_offset =
		    mip_sizes[level].src_size != 0 ? mip_sizes[level].src_offset : mip_sizes[level].offset;
		const auto level_size =
		    static_cast<uint64_t>(mip_sizes[level].src_size != 0 ? mip_sizes[level].src_size
			                                                     : mip_sizes[level].size) *
		    view.image_layers;
		desc.info.mip_layout[level] = {
		    level_offset,
		    level_size,
		    mip_padded[level].width,
		    mip_padded[level].height,
		};
	}
	desc.view_info.format = target_format.format;
	if (is_1d) {
		desc.view_info.type =
		    view.layer_count == 1 ? vk::ImageViewType::e1D : vk::ImageViewType::e1DArray;
	} else {
		desc.view_info.type =
		    view.layer_count == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
	}
	desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
	desc.view_info.base_level  = rt.view.current_mip_level;
	desc.view_info.level_count = 1;
	desc.view_info.base_layer  = view.base_layer;
	desc.view_info.layer_count = view.layer_count;
	desc.view_info.usage       = vk::ImageUsageFlagBits::eColorAttachment;
	auto& texture_cache        = m_context.GetTextureCache();
	r.guest_mip_level          = rt.view.current_mip_level;
	r.guest_array_layer        = view.base_layer;
	r.image_id                 = texture_cache.FindImage(r.desc, exact_format);
	r.export_mapping           = target_format.export_mapping;
	BindRenderTarget(r.image_id);
}

} // namespace Libs::Graphics
