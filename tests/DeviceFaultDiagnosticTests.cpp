#include "graphics/host_gpu/deviceFaultDiagnostic.h"

#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Libs::Graphics::DeviceFaultDiagnostic;

void Require(bool value, const char* message) {
	if (!value) throw std::runtime_error(message);
}

struct Step {
	bool info;
	VkResult result;
	uint32_t addresses;
	uint32_t vendors;
	VkDeviceSize binary = 0;
};

struct Spy {
	std::vector<Step> steps;
	size_t calls = 0;
	std::vector<std::array<uint32_t, 2>> capacities;
	VkResult operator()(VkDeviceFaultCountsEXT* counts, VkDeviceFaultInfoEXT* info) {
		Require(calls < steps.size(), "unexpected refetch");
		const auto step = steps[calls++];
		Require(counts->sType == VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT &&
		        counts->pNext == nullptr, "actual counts ABI");
		Require(step.info == (info != nullptr), "two-pass ordering");
		if (info != nullptr) {
			Require(info->sType == VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT &&
			        info->pNext == nullptr, "actual info ABI");
			Require(counts->addressInfoCount <= MaxEntries &&
			        counts->vendorInfoCount <= MaxEntries, "bounded allocated capacity");
			Require(counts->vendorBinarySize == 0 && info->pVendorBinaryData == nullptr,
			        "binary must never be requested");
			Require((counts->addressInfoCount == 0) == (info->pAddressInfos == nullptr),
			        "zero-address pointer contract");
			Require((counts->vendorInfoCount == 0) == (info->pVendorInfos == nullptr),
			        "zero-vendor pointer contract");
			capacities.push_back({counts->addressInfoCount, counts->vendorInfoCount});
			for (uint32_t i = 0; i < std::min(counts->addressInfoCount, step.addresses); ++i) {
				info->pAddressInfos[i] = {VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT,
				                         0x12340000ull + i, 4096};
			}
			for (uint32_t i = 0; i < std::min(counts->vendorInfoCount, step.vendors); ++i) {
				std::memcpy(info->pVendorInfos[i].description, "vendor\nline", 12);
				info->pVendorInfos[i].vendorFaultCode = 0xab00 + i;
				info->pVendorInfos[i].vendorFaultData = 0xcd00 + i;
			}
			std::memcpy(info->description, "fault\nline", 11);
		}
		counts->addressInfoCount = step.addresses;
		counts->vendorInfoCount = step.vendors;
		counts->vendorBinarySize = step.binary;
		return step.result;
	}
};

int main(int argc, char** argv) {
	try {
		if (argc == 3 && std::strcmp(argv[1], "--gate-child") == 0) {
			const bool expected = std::strcmp(argv[2], "1") == 0;
			Require(Requested() == expected, "exact environment selection");
#ifdef _WIN32
			_putenv_s("KYTY_GPU_FAULT_DIAGNOSTIC", expected ? "0" : "1");
#else
			setenv("KYTY_GPU_FAULT_DIAGNOSTIC", expected ? "0" : "1", 1);
#endif
			Require(Requested() == expected, "process-frozen selection");
			std::puts("PASS exact frozen gate child");
			return 0;
		}
		Require(argc == 1, "strict CLI");
		unsigned groups = 0;
		for (unsigned requested = 0; requested < 2; ++requested)
			for (unsigned extension = 0; extension < 2; ++extension)
				for (unsigned feature = 0; feature < 2; ++feature)
					Require(EnableFeature(requested, extension, feature) ==
					            (requested && extension && feature), "supported opt-in feature");
		++groups;
		{
			State state;
			Require(!TryBegin(state, VK_ERROR_DEVICE_LOST) && !state.first_loss_claimed.load(),
			        "OFF loss untouched");
			state.requested = true;
			for (auto result : {VK_SUCCESS, VK_INCOMPLETE, VK_TIMEOUT, VK_ERROR_UNKNOWN})
				Require(!TryBegin(state, result) && !state.first_loss_claimed.load(),
				        "genuine device loss only");
			Require(TryBegin(state, VK_ERROR_DEVICE_LOST) && !TryBegin(state, VK_ERROR_DEVICE_LOST),
			        "first genuine loss once");
		}
		++groups;
		{
			State state; state.requested = true;
			std::atomic<unsigned> winners {0};
			std::vector<std::thread> threads;
			for (unsigned i = 0; i < 16; ++i) threads.emplace_back([&] {
				if (TryBegin(state, VK_ERROR_DEVICE_LOST)) ++winners;
			});
			for (auto& thread : threads) thread.join();
			Require(winners == 1, "simultaneous loss atomic once");
		}
		++groups;
		{
			Spy spy {{{false, VK_SUCCESS, 2, 1}, {true, VK_SUCCESS, 2, 1}}};
			auto report = Collect(spy);
			Require(report.complete && report.calls == 2 && report.attempts == 1 &&
			        report.written_addresses == 2 && report.written_vendors == 1 &&
			        report.addresses[1].reportedAddress == 0x12340001 &&
			        report.vendors[0].vendorFaultCode == 0xab00 &&
			        std::strcmp(report.description.data(), "fault?line") == 0,
			        "complete bounded payload identity");
		}
		++groups;
		for (auto size : {0u, MaxEntries}) {
			Spy spy {{{false, VK_SUCCESS, size, size}, {true, VK_SUCCESS, size, size}}};
			auto report = Collect(spy);
			Require(report.complete && report.written_addresses == size &&
			        report.written_vendors == size && report.calls == 2, "zero/64 boundary");
			++groups;
		}
		for (auto size : {MaxEntries + 1, UINT32_MAX}) {
			Spy spy {{{false, VK_SUCCESS, size, size}, {true, VK_INCOMPLETE, MaxEntries, MaxEntries}}};
			auto report = Collect(spy);
			Require(!report.complete && report.capped && report.calls == 2 &&
			        report.written_addresses == MaxEntries && report.written_vendors == MaxEntries &&
			        spy.capacities[0] == std::array<uint32_t, 2>{MaxEntries, MaxEntries},
			        "65/UINT_MAX truncation does not allocate or refetch beyond cap");
			++groups;
		}
		{
			Spy spy {{{false, VK_SUCCESS, 2, 1}, {true, VK_INCOMPLETE, 1, 1},
			          {false, VK_SUCCESS, 2, 1}, {true, VK_SUCCESS, 2, 1}}};
			auto report = Collect(spy);
			Require(report.complete && report.calls == 4 && report.attempts == 2 &&
			        report.written_addresses == 2 && spy.capacities.size() == 2,
			        "INCOMPLETE bounded two-pass refetch");
		}
		++groups;
		{
			Spy spy {{{false, VK_SUCCESS, 1, 1}, {true, VK_INCOMPLETE, 1, 1},
			          {false, VK_SUCCESS, 1, 1}, {true, VK_INCOMPLETE, 1, 1}}};
			auto report = Collect(spy);
			Require(!report.complete && report.calls == 4 && report.attempts == 2,
			        "persistent INCOMPLETE strict stop");
		}
		++groups;
		{
			Spy spy {{{false, VK_INCOMPLETE, 1, 0}, {true, VK_SUCCESS, 1, 0},
			          {false, VK_SUCCESS, 1, 0}, {true, VK_SUCCESS, 1, 0}}};
			auto report = Collect(spy);
			Require(report.complete && report.calls == 4, "count-pass INCOMPLETE refetch");
		}
		++groups;
		{
			Spy spy {{{false, VK_ERROR_OUT_OF_HOST_MEMORY, UINT32_MAX, UINT32_MAX}}};
			auto report = Collect(spy);
			Require(!report.complete && report.calls == 1 && !report.info_queried[0] &&
			        report.written_addresses == 0, "count error no data query");
		}
		++groups;
		{
			Spy spy {{{false, VK_SUCCESS, 1, 1}, {true, VK_ERROR_UNKNOWN, 1, 1}}};
			auto report = Collect(spy);
			Require(!report.complete && report.calls == 2 && report.written_addresses == 0 &&
			        report.written_vendors == 0 && report.description[0] == 0,
			        "data error does not advertise undefined outputs");
		}
		++groups;
		{
			Spy spy {{{false, VK_SUCCESS, 1, 1}, {true, VK_INCOMPLETE, 1, 1},
			          {false, VK_ERROR_UNKNOWN, 0, 0}}};
			auto report = Collect(spy);
			Require(!report.complete && report.calls == 3 && !report.info_queried[1] &&
			        report.written_addresses == 0, "refetch error discards stale payload");
		}
		++groups;
		{
			Spy spy {{{false, VK_SUCCESS, 1, 1}, {true, VK_SUCCESS, 2, 2}}};
			auto report = Collect(spy);
			Require(!report.complete && report.invalid_return_counts && report.calls == 2 &&
			        report.written_addresses == 1, "invalid returned counts remain bounded");
		}
		++groups;
		{
			Spy spy {{{false, VK_SUCCESS, 1, 1, 4096}, {true, VK_INCOMPLETE, 1, 1, 0}}};
			auto report = Collect(spy);
			Require(!report.complete && report.unexpected_binary && report.calls == 2,
			        "unexpected advertised binary never read");
		}
		++groups;
		{
			Spy spy {{{false, VK_SUCCESS, 1, 0}, {true, VK_INCOMPLETE, 1, 0},
			          {false, VK_SUCCESS, 2, 0}, {true, VK_SUCCESS, 2, 0}}};
			auto report = Collect(spy);
			Require(!report.complete && report.count_values_changed && report.calls == 4,
			        "stable Vulkan count contract violation labeled");
		}
		++groups;
		{
			Spy spy {{{false, VK_SUCCESS, 2, 1}, {true, VK_SUCCESS, 1, 1}}};
			auto report = Collect(spy);
			Require(!report.complete && report.written_addresses == 1 && report.calls == 2,
			        "short SUCCESS cannot claim complete advertised payload");
		}
		++groups;
		{
			std::array<char, VK_MAX_DESCRIPTION_SIZE> source;
			source.fill('x'); source[0] = '\n'; source[1] = '\r'; source[2] = '\t'; source[3] = 127;
			auto text = SafeDescription(source.data());
			Require(text[0] == '?' && text[1] == '?' && text[2] == '?' && text[3] == '?' &&
			        text.back() == 0 && std::strlen(text.data()) == text.size() - 1,
			        "bounded non-NUL description and one-line logs");
		}
		++groups;
		std::printf("PASS device fault CPU spy groups=%u no_vulkan_calls=1\n", groups);
		return 0;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "FAIL %s\n", error.what());
		return 1;
	}
}
