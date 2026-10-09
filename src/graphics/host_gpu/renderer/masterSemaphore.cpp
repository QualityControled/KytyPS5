#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/MenuPerformanceDiagnostic.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cinttypes>

namespace Libs::Graphics {

namespace {

void ReportFirstDeviceLoss(GraphicContext& graphics, vk::Result result, const char* site,
                           bool tick_valid, uint64_t tick) {
	auto& state = graphics.device_fault_diagnostic;
	if (!DeviceFaultDiagnostic::TryBegin(state, static_cast<VkResult>(result))) return;
	Log::WriteToConsoleAndLog(fmt::format(
	    "GpuFaultDiagnostic loss site={} result={} tick_valid={} tick={} enabled={} "
	    "entrypoint_available={} first_loss=1 fatal_preserved=1\n",
	    site, static_cast<int>(result), tick_valid, tick, state.enabled, state.query != nullptr));
	if (!state.enabled || state.query == nullptr) {
		Log::WriteToConsoleAndLog("GpuFaultDiagnostic unavailable no_query=1 fault_reason_unproved=1\n");
		return;
	}
	const auto report = DeviceFaultDiagnostic::Collect(
	    [&](VkDeviceFaultCountsEXT* counts, VkDeviceFaultInfoEXT* info) {
		    return state.query(static_cast<VkDevice>(graphics.device), counts, info);
	    });
	for (uint32_t i = 0; i < report.attempts; ++i) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "GpuFaultDiagnostic query attempt={} count_result={} info_queried={} info_result={}\n",
		    i + 1, static_cast<int>(report.count_results[i]), report.info_queried[i],
		    report.info_queried[i] ? std::to_string(static_cast<int>(report.info_results[i]))
		                           : "not_queried"));
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "GpuFaultDiagnostic report complete={} capped={} invalid_return_counts={} "
	    "unexpected_binary={} count_values_changed={} advertised_addresses={} "
	    "advertised_vendors={} advertised_binary_bytes={} written_addresses={} "
	    "written_vendors={} calls={} vendor_binary_captured=0 description={}\n",
	    report.complete, report.capped, report.invalid_return_counts, report.unexpected_binary,
	    report.count_values_changed, report.advertised_addresses, report.advertised_vendors,
	    report.advertised_binary_bytes, report.written_addresses, report.written_vendors,
	    report.calls, report.description.data()));
	for (uint32_t i = 0; i < report.written_addresses; ++i) {
		const auto& address = report.addresses[i];
		Log::WriteToConsoleAndLog(fmt::format(
		    "GpuFaultDiagnostic address index={} type={} reported=0x{:016x} precision={}\n",
		    i, static_cast<int>(address.addressType), address.reportedAddress, address.addressPrecision));
	}
	for (uint32_t i = 0; i < report.written_vendors; ++i) {
		const auto& vendor = report.vendors[i];
		const auto text = DeviceFaultDiagnostic::SafeDescription(vendor.description);
		Log::WriteToConsoleAndLog(fmt::format(
		    "GpuFaultDiagnostic vendor index={} code=0x{:016x} data=0x{:016x} description={}\n",
		    i, vendor.vendorFaultCode, vendor.vendorFaultData, text.data()));
	}
}

} // namespace

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh(TimelineRefreshRole role) {
	MenuPerformanceDiagnostic::TimelineRefreshScope diagnostic_scope(static_cast<uint32_t>(role));
	uint64_t                                        counter = 0;
	const auto result = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result != vk::Result::eSuccess) {
		ReportFirstDeviceLoss(m_graphics, result, "timeline_counter", false, 0);
		EXIT("GPU timeline counter failed: Vulkan result=%d device_lost=%d\n",
		     static_cast<int>(result), result == vk::Result::eErrorDeviceLost);
	}

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh(TimelineRefreshRole::Wait);
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	// Only the original potentially blocking Vulkan call, excluding both Refresh calls.
	const auto result = [&] {
		MenuPerformanceDiagnostic::TimedScope diagnostic_scope(
		    MenuPerformanceDiagnostic::TimedOperation::SemaphoreWaitBlocking);
		return m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	}();
	if (result != vk::Result::eSuccess) {
		ReportFirstDeviceLoss(m_graphics, result, "timeline_wait", true, tick);
		EXIT("GPU timeline wait failed: Vulkan result=%d device_lost=%d tick=%" PRIu64 "\n",
		     static_cast<int>(result), result == vk::Result::eErrorDeviceLost, tick);
	}
	Refresh(TimelineRefreshRole::Wait);
}

} // namespace Libs::Graphics
