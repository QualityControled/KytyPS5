#ifndef KYTY_GRAPHICS_DEVICE_FAULT_DIAGNOSTIC_H_
#define KYTY_GRAPHICS_DEVICE_FAULT_DIAGNOSTIC_H_

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::DeviceFaultDiagnostic {

inline constexpr uint32_t MaxEntries = 64;
inline constexpr uint32_t MaxAttempts = 2;

inline bool Requested() {
	static const bool requested = [] {
		const auto* value = std::getenv("KYTY_GPU_FAULT_DIAGNOSTIC");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

inline bool EnableFeature(bool requested, bool extension_supported, VkBool32 feature_supported) {
	return requested && extension_supported && feature_supported == VK_TRUE;
}

struct State {
	bool requested           = false;
	bool extension_supported = false;
	bool feature_supported   = false;
	bool enabled             = false;
	PFN_vkGetDeviceFaultInfoEXT query = nullptr;
	std::atomic_bool first_loss_claimed {false};
};

inline bool TryBegin(State& state, VkResult observed_result) {
	// Disabled and non-device-loss paths do not touch the atomic or query Vulkan.
	return state.requested && observed_result == VK_ERROR_DEVICE_LOST &&
	       !state.first_loss_claimed.exchange(true, std::memory_order_relaxed);
}

struct Report {
	std::array<VkDeviceFaultAddressInfoEXT, MaxEntries> addresses {};
	std::array<VkDeviceFaultVendorInfoEXT, MaxEntries> vendors {};
	std::array<char, VK_MAX_DESCRIPTION_SIZE> description {};
	std::array<VkResult, MaxAttempts> count_results {};
	std::array<VkResult, MaxAttempts> info_results {};
	std::array<bool, MaxAttempts> info_queried {};
	uint32_t attempts = 0;
	uint32_t calls = 0;
	uint32_t advertised_addresses = 0;
	uint32_t advertised_vendors = 0;
	VkDeviceSize advertised_binary_bytes = 0;
	uint32_t written_addresses = 0;
	uint32_t written_vendors = 0;
	bool capped = false;
	bool invalid_return_counts = false;
	bool unexpected_binary = false;
	bool count_values_changed = false;
	bool complete = false;
};

inline std::array<char, VK_MAX_DESCRIPTION_SIZE> SafeDescription(const char* source) {
	std::array<char, VK_MAX_DESCRIPTION_SIZE> text {};
	for (size_t i = 0; i + 1 < text.size() && source[i] != '\0'; ++i) {
		const auto byte = static_cast<unsigned char>(source[i]);
		text[i] = byte < 0x20 || byte == 0x7f ? '?' : source[i];
	}
	return text;
}

// Query is injectable only for CPU tests; production passes the actual extension entry point.
// VK_EXT_device_fault promises stable count/data queries after loss. A bounded refetch
// handles INCOMPLETE without an unbounded enumerate loop. Binary capture is never requested.
template <typename Query>
Report Collect(Query&& query) {
	Report report;
	uint32_t first_addresses = 0;
	uint32_t first_vendors = 0;
	VkDeviceSize first_binary = 0;
	for (uint32_t attempt = 0; attempt < MaxAttempts; ++attempt) {
		report.attempts = attempt + 1;
		report.written_addresses = 0;
		report.written_vendors = 0;
		report.description = {};
		VkDeviceFaultCountsEXT counts {};
		counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
		const auto count_result = query(&counts, nullptr);
		++report.calls;
		report.count_results[attempt] = count_result;
		if (count_result != VK_SUCCESS && count_result != VK_INCOMPLETE) break;
		if (attempt == 0) {
			first_addresses = counts.addressInfoCount;
			first_vendors = counts.vendorInfoCount;
			first_binary = counts.vendorBinarySize;
		} else {
			report.count_values_changed |= counts.addressInfoCount != first_addresses ||
			    counts.vendorInfoCount != first_vendors || counts.vendorBinarySize != first_binary;
		}
		report.advertised_addresses = counts.addressInfoCount;
		report.advertised_vendors = counts.vendorInfoCount;
		report.advertised_binary_bytes = counts.vendorBinarySize;
		report.capped |= counts.addressInfoCount > MaxEntries || counts.vendorInfoCount > MaxEntries;
		report.unexpected_binary |= counts.vendorBinarySize != 0;
		const auto address_capacity = std::min(counts.addressInfoCount, MaxEntries);
		const auto vendor_capacity = std::min(counts.vendorInfoCount, MaxEntries);
		counts.addressInfoCount = address_capacity;
		counts.vendorInfoCount = vendor_capacity;
		counts.vendorBinarySize = 0;
		VkDeviceFaultInfoEXT info {};
		info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
		info.pAddressInfos = address_capacity != 0 ? report.addresses.data() : nullptr;
		info.pVendorInfos = vendor_capacity != 0 ? report.vendors.data() : nullptr;
		info.pVendorBinaryData = nullptr;
		const auto info_result = query(&counts, &info);
		++report.calls;
		report.info_queried[attempt] = true;
		report.info_results[attempt] = info_result;
		// Output payload is defined only on SUCCESS/INCOMPLETE. Preserve the error code,
		// but do not advertise a prior attempt's rows as a complete final result.
		report.written_addresses = 0;
		report.written_vendors = 0;
		report.description = {};
		if (info_result != VK_SUCCESS && info_result != VK_INCOMPLETE) break;
		report.invalid_return_counts |= counts.addressInfoCount > address_capacity ||
		    counts.vendorInfoCount > vendor_capacity;
		report.unexpected_binary |= counts.vendorBinarySize != 0;
		report.written_addresses = std::min(counts.addressInfoCount, address_capacity);
		report.written_vendors = std::min(counts.vendorInfoCount, vendor_capacity);
		report.description = SafeDescription(info.description);
		report.complete = count_result == VK_SUCCESS && info_result == VK_SUCCESS &&
		    !report.capped && !report.invalid_return_counts && !report.unexpected_binary &&
		    !report.count_values_changed &&
		    report.written_addresses == report.advertised_addresses &&
		    report.written_vendors == report.advertised_vendors;
		if ((info_result != VK_INCOMPLETE && count_result != VK_INCOMPLETE) ||
		    report.capped || report.unexpected_binary ||
		    report.invalid_return_counts) break;
	}
	return report;
}

} // namespace Libs::Graphics::DeviceFaultDiagnostic

#endif
