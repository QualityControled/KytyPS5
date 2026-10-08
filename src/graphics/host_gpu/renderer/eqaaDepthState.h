#pragma once

#include "graphics/guest_gpu/hardwareContext.h"

namespace Libs::Graphics::ShaderRecompiler::Diagnostics {

// Narrower than ResolveRenderDepthTarget's inactive fast path: no dormant bound
// attachment, stale addresses, active stencil clear/copy, metadata or mip state.
// This proves absence only; it never creates or certifies initialized depth data.
inline bool IsReducedEqaaDepthAbsent(const HW::Context& hw) {
	const auto& z = hw.GetDepthRenderTarget();
	const auto& rc = hw.GetRenderControl();
	const auto& dc = hw.GetDepthControl();
	return !dc.z_enable && !dc.depth_bounds_enable && !dc.stencil_enable &&
	       !rc.depth_clear_enable && !rc.stencil_clear_enable &&
	       !rc.copy_depth_to_color && !rc.copy_stencil_to_color &&
	       !rc.copy_centroid && rc.copy_sample == 0 &&
	       z.z_info.format == Prospero::DepthFormat::kInvalid &&
	       z.stencil_info.format == Prospero::StencilFormat::kInvalid &&
	       z.z_info.num_samples == 0 &&
	       z.z_info.texture_compatibility == Prospero::TextureCompatiblePlaneCompression::kDisable &&
	       !z.z_info.expclear_enabled && !z.z_info.partially_resident && z.z_info.max_mip_level == 0 &&
	       z.stencil_info.texture_compatibility == Prospero::TextureCompatibleStencil::kDisable &&
	       !z.stencil_info.expclear_enabled && !z.stencil_info.partially_resident &&
	       z.depth_view.slice_start == 0 && z.depth_view.slice_max == 0 &&
	       z.depth_view.current_mip_level == 0 && !z.depth_view.depth_write_disable &&
	       !z.depth_view.stencil_write_disable && z.z_read_base_addr == 0 &&
	       z.z_write_base_addr == 0 && z.stencil_read_base_addr == 0 &&
	       z.stencil_write_base_addr == 0 && z.htile_data_base_addr == 0 &&
	       !z.z_info.htile_acceleration && z.shading_rate_encoding == 0 &&
	       z.size.x_max == 0 && z.size.y_max == 0;
}

} // namespace Libs::Graphics::ShaderRecompiler::Diagnostics
