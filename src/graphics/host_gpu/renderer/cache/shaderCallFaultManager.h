#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_SHADERCALLFAULTMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_SHADERCALLFAULTMANAGER_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/shader/recompiler/BvhDiagnosticRecord.h"

#include <array>
#include <optional>

namespace Libs::Graphics {

// Dedicated external-call fault records must not be mixed with the BDA page bitmap.
class ShaderCallFaultManager {
public:
	ShaderCallFaultManager(GraphicContext& graphics, CommandScheduler& scheduler);
	KYTY_CLASS_NO_COPY(ShaderCallFaultManager);
	[[nodiscard]] Buffer* GetBuffer(uint64_t required_record_bytes = 128u) noexcept;
	void Process(bool wait_for_completion = false, bool after_bvh_capture = false);

private:
	// First eight DWORDs retain the ordinary fault ABI; the probe adds ordinal,
	// domain, full EXEC, auxiliary pointer, half agreement and subgroup width.
	// Kind7 also preserves the winning ray tuple, native pointer width and wave lane.
	static constexpr uint64_t DefaultRecordSize = ShaderRecompiler::Diagnostics::BvhDiagnosticWords * sizeof(uint32_t);
	void FinalizeBvhCapture();
	void FinalizeBvhResultCapture();
	static constexpr size_t MaxPending = 8;
	CommandScheduler& m_scheduler;
	const uint64_t m_record_size;
	Buffer m_fault_buffer;
	uint64_t m_area_stride;
	Buffer m_download_buffer;
	std::array<uint64_t, MaxPending> m_ticks {};
	uint32_t m_area = 0;
	bool m_used = false;
	std::optional<std::array<uint32_t, ShaderRecompiler::Diagnostics::BvhDiagnosticWords>> m_pending_bvh;
	uint64_t m_pending_bvh_tick = 0;
	std::optional<std::array<uint32_t, ShaderRecompiler::Diagnostics::BvhResultDiagnosticWords>> m_pending_bvh_result;
	uint64_t m_pending_bvh_result_tick = 0;
};

} // namespace Libs::Graphics
#endif
