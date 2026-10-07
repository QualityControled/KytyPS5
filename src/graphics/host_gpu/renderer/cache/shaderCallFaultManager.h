#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_SHADERCALLFAULTMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_SHADERCALLFAULTMANAGER_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>

namespace Libs::Graphics {

// Dedicated external-call fault records must not be mixed with the BDA page bitmap.
class ShaderCallFaultManager {
public:
	ShaderCallFaultManager(GraphicContext& graphics, CommandScheduler& scheduler);
	KYTY_CLASS_NO_COPY(ShaderCallFaultManager);
	[[nodiscard]] Buffer* GetBuffer() noexcept;
	void Process(bool wait_for_completion = false);

private:
	// First eight DWORDs retain the ordinary fault ABI; the probe adds ordinal,
	// domain, full EXEC, auxiliary pointer, half agreement and subgroup width.
	static constexpr uint64_t RecordSize = 16u * sizeof(uint32_t);
	static constexpr size_t MaxPending = 8;
	CommandScheduler& m_scheduler;
	Buffer m_fault_buffer;
	uint64_t m_area_stride;
	Buffer m_download_buffer;
	std::array<uint64_t, MaxPending> m_ticks {};
	uint32_t m_area = 0;
	bool m_used = false;
};

} // namespace Libs::Graphics
#endif
