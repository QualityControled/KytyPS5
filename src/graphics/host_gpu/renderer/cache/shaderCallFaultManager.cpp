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

void ShaderCallFaultManager::Process() {
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
		std::array<uint32_t, 8> record {};
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
		EXIT("Shader external-call runtime fault: kind=%u target=0x%016" PRIx64
		     " guest_pc=0x%016" PRIx64 " shader=0x%016" PRIx64 "\n",
		     record[1], target, pc, hash);
	});
	m_ticks[m_area] = m_scheduler.CurrentTick();
	m_area = (m_area + 1u) % MaxPending;
}

} // namespace Libs::Graphics
