#ifndef EMULATOR_IMAGE_OWNER_DIAGNOSTIC_H_
#define EMULATOR_IMAGE_OWNER_DIAGNOSTIC_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics {

// CPU facts only. Dirty, usage, and metadata flags are not an initialization proof.
struct ImageOwnerDiagnosticRow {
	uint32_t index = 0, generation = 0;
	bool     registered = false, backing_present = false;
	uint64_t data_address = 0, data_size = 0, stencil_address = 0, stencil_size = 0;
	uint64_t metadata_address = 0, metadata_size = 0;
	uint32_t metadata_kind = 0, htile_clear_mask = 0;
	uint32_t guest_samples = 0, backing_samples = 0;
	int32_t  guest_format = 0, backing_format = 0, image_type = 0;
	uint32_t width = 0, height = 0, depth = 0, layers = 0, levels = 0;
	bool     cpu_dirty = false, definitely_cpu_dirty = false, maybe_cpu_dirty = false;
	bool     gpu_modified = false, buffer_modified = false, tracked = false;
	bool     texture = false, storage = false, render_target = false, depth_target = false;
	bool     video_out = false, bound = false, target = false, needs_rebind = false;
	bool     force_general = false, shader_write = false;
	int32_t  global_layout = 0, attachment_layout = 0;
	uint64_t global_access = 0, global_stage = 0, attachment_access = 0;
	size_t   subresource_state_count = 0;
};

struct ImageOwnerDiagnosticSnapshot {
	static constexpr size_t kOwnerScanLimit = 64;
	static constexpr size_t kRowLimit       = 16;
	uint64_t                address         = 0;
	bool                    valid_address = false, truncated = false;
	size_t                  page_owner_count = 0, inspected = 0, matched = 0;
	size_t                  stale = 0, unregistered = 0, outside_range = 0, row_count = 0;
	std::array<ImageOwnerDiagnosticRow, kRowLimit> rows {};
};

[[nodiscard]] constexpr bool DiagnosticRangeContains(uint64_t base, uint64_t size,
                                                     uint64_t address) noexcept {
	constexpr uint64_t limit = uint64_t {1} << 44;
	return base != 0 && base < limit && size != 0 && size <= limit - base && address >= base &&
	       address - base < size;
}

// Caller supplies the existing base-page owner list and a read-only slot lookup.
// No query epoch, LRU, register, guest memory, or Vulkan object is changed.
template <typename Owners, typename Lookup>
[[nodiscard]] ImageOwnerDiagnosticSnapshot
CollectImageOwnerDiagnostic(uint64_t address, const Owners* owners, Lookup&& lookup) {
	ImageOwnerDiagnosticSnapshot result;
	result.address       = address;
	result.valid_address = address != 0 && address < (uint64_t {1} << 44);
	if (!result.valid_address || owners == nullptr) return result;
	result.page_owner_count = owners->size();
	const size_t count      = std::min(owners->size(), result.kOwnerScanLimit);
	result.truncated        = count != owners->size();
	for (size_t i = 0; i < count; ++i) {
		++result.inspected;
		auto row = lookup((*owners)[i]);
		if (!row) {
			++result.stale;
			continue;
		}
		if (!row->registered) {
			++result.unregistered;
			continue;
		}
		if (!DiagnosticRangeContains(row->data_address, row->data_size, address) &&
		    !DiagnosticRangeContains(row->stencil_address, row->stencil_size, address)) {
			++result.outside_range;
			continue;
		}
		++result.matched;
		if (result.row_count == result.rows.size()) {
			result.truncated = true;
			continue;
		}
		result.rows[result.row_count++] = *row;
	}
	return result;
}

} // namespace Libs::Graphics
#endif
