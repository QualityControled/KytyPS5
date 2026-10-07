#include "graphics/host_gpu/renderer/cache/shaderCallFaultManager.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace Libs::Graphics {

ShaderCallFaultManager::ShaderCallFaultManager(GraphicContext& graphics,
                                               CommandScheduler& scheduler)
    : m_scheduler(scheduler),
      m_fault_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, RecordSize),
	  m_area_stride(std::max<uint64_t>(RecordSize,
	      graphics.physical_device_properties.limits.nonCoherentAtomSize)),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
	                        MaxPending * m_area_stride) {
	m_fault_buffer.Fill(0, RecordSize, 0);
}

Buffer* ShaderCallFaultManager::GetBuffer() noexcept {
	m_used = true;
	return &m_fault_buffer;
}

void ShaderCallFaultManager::Process(bool wait_for_completion) {
	if (!m_used) return;
	m_used = false;
	if (const auto tick = m_ticks[m_area]; tick != 0u) {
		m_scheduler.Wait(tick);
		m_scheduler.PopPendingOperations();
	}
	const auto area = m_area;
	// Each pending read occupies its own noncoherent atom. Invalidation must not overlap a
	// different in-flight slot even when the device's atom is larger than the fault record.
	const uint64_t offset = area * m_area_stride;
	m_download_buffer.CopyFrom(m_scheduler.Current(), m_fault_buffer, 0, offset, RecordSize,
	                           vk::AccessFlagBits::eShaderWrite,
	                           vk::AccessFlagBits::eHostRead,
	                           vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
	                           vk::AccessFlagBits::eHostRead);
	m_scheduler.DeferOperation([this, offset, area] {
		m_download_buffer.Invalidate(offset, RecordSize);
		std::array<uint32_t, 16> record {};
		std::memcpy(record.data(), m_download_buffer.Mapped().data() + offset, RecordSize);
		m_ticks[area] = 0;
		if (record[0] == 0u) return;
		const uint64_t target = uint64_t {record[2]} | (uint64_t {record[3]} << 32u);
		const uint64_t pc = uint64_t {record[4]} | (uint64_t {record[5]} << 32u);
		const uint64_t hash = uint64_t {record[6]} | (uint64_t {record[7]} << 32u);
		if (record[1] == 2u) {
			EXIT("Shader external-material context fault: ordinal=%u domain=%u guest_pc=0x%016"
			     PRIx64 " shader=0x%016" PRIx64 "\n", record[2], record[3], pc, hash);
		}
		if (record[1] == 3u) {
			const uint64_t auxiliary = uint64_t {record[12]} | (uint64_t {record[13]} << 32u);
			EXIT("Shader selected-call diagnostic stop: target=0x%016" PRIx64
			     " guest_pc=0x%016" PRIx64 " shader=0x%016" PRIx64
			     " ordinal=%u domain=%u exec=0x%08x%08x auxiliary=0x%016" PRIx64
			     " input_mismatch=%u host_subgroup=%u; no callee or caller continuation executed\n",
			     target, pc, hash, record[8], record[9], record[11], record[10], auxiliary,
			     record[14], record[15]);
		}
		if (record[1] == 5u) {
			EXIT("Shader checked external-call layout rejection: detail0=%u detail1=%u "
			     "guest_pc=0x%016" PRIx64 " shader=0x%016" PRIx64
			     " physical_local=%u subgroup_id=%u subgroup_lane=%u subgroup_size=%u "
			     "physical_workgroup=%u expected_subgroup=%u guest_wave=%u host_width=%u"
			     "; stopped before guest shader work\n", record[2], record[3], pc, hash,
			     record[8], record[9], record[10], record[11], record[12], record[13],
			     record[14], record[15]);
		}
		if (record[1] == 6u) {
			EXIT("Shader external scalar call-input disagreement: target=0x%016" PRIx64
			     " guest_pc=0x%016" PRIx64 " shader=0x%016" PRIx64
			     "; stopped before selected callee or caller continuation\n", target, pc, hash);
		}
		if (record[1] == 7u) {
			EXIT("Shader first-active BVH diagnostic stop: derived_node_address=0x%016" PRIx64
			     " guest_pc=0x%016" PRIx64 " shader=0x%016" PRIx64
			     " descriptor=[%08x,%08x,%08x,%08x] raw_node=0x%08x%08x exec=0x%08x%08x"
			     "; stopped before intersection, BDA lookup, or following consumers\n",
			     target, pc, hash, record[8], record[9], record[10], record[11],
			     record[13], record[12], record[15], record[14]);
		}
		if (record[1] == 1u && record[15] == 32u && record[14] == 0u) {
			const uint64_t auxiliary = uint64_t {record[12]} | (uint64_t {record[13]} << 32u);
			EXIT("Shader checked external-call coverage stop: target=0x%016" PRIx64
			     " guest_pc=0x%016" PRIx64 " shader=0x%016" PRIx64
			     " ordinal=%u domain=%u exec=0x%08x%08x auxiliary=0x%016" PRIx64
			     " host_subgroup=%u; selected function is outside verified coverage\n",
			     target, pc, hash, record[8], record[9], record[11], record[10], auxiliary,
			     record[15]);
		}
		EXIT("Shader external-call runtime fault: kind=%u target=0x%016" PRIx64
		     " guest_pc=0x%016" PRIx64 " shader=0x%016" PRIx64 "\n",
		     record[1], target, pc, hash);
	});
	m_ticks[m_area] = m_scheduler.CurrentTick();
	const auto scheduled_tick = m_ticks[m_area];
	m_area = (m_area + 1u) % MaxPending;
	if (wait_for_completion) {
		// Checked external calls may return early at a diagnostic fault. Finish their dispatch
		// and inspect the record before following PM4 consumers can use incomplete results.
		m_scheduler.Wait(scheduled_tick);
		m_scheduler.PopPendingOperations();
	}
}

} // namespace Libs::Graphics
