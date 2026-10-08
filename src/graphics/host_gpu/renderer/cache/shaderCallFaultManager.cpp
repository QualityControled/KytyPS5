#include "graphics/host_gpu/renderer/cache/shaderCallFaultManager.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/shader/recompiler/BvhNodeCapture.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fmt/format.h>

namespace Libs::Graphics {

namespace {

bool ReadBvhBufferView(void*, uint64_t address, uint8_t* output, size_t bytes) {
	return GuestGpu::IsGpuThread() && bytes == ShaderRecompiler::BvhNodeCapture::NodeBlockBytes &&
	       LibKernel::Memory::TryReadBufferBacking(address, output, bytes);
}

bool WriteBvhCaptureFile(const std::filesystem::path& path, const void* bytes, size_t size) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) return false;
	output.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
	output.flush();
	output.close();
	return static_cast<bool>(output);
}

} // namespace

void ShaderCallFaultManager::FinalizeBvhCapture() {
	using namespace ShaderRecompiler;
	if (!m_pending_bvh) EXIT("No pending BVH diagnostic event\n");
	const auto record = *m_pending_bvh;
	m_pending_bvh.reset();
	const auto sidecar_valid = Diagnostics::ValidBvhRaySidecar(record);
	const auto width = !sidecar_valid ? BvhNodeCapture::NodeWidth::Unknown
	    : record[Diagnostics::BvhNodeWidthWord] == 1u ? BvhNodeCapture::NodeWidth::Narrow32
	                                               : BvhNodeCapture::NodeWidth::Wide64;
	BvhNodeCapture::EventWords event_words;
	std::copy_n(record.begin(), event_words.size(), event_words.begin());
	const auto event = BvhNodeCapture::DecodeEvent(event_words, width);
	std::printf("BVH snapshot begin: sidecar_valid=%u selected_lane=%u node_width=%u "
	            "address=0x%016" PRIx64 " guest_pc=0x%016" PRIx64 "\n",
	            sidecar_valid, record[Diagnostics::BvhNativeLaneWord], record[Diagnostics::BvhNodeWidthWord],
	            event.reported_address, event.guest_pc);
	std::printf("BVH raw ray bits: extent=%08x origin=[%08x,%08x,%08x] "
	            "direction=[%08x,%08x,%08x] inverse=[%08x,%08x,%08x]\n",
	            record[16], record[17], record[18], record[19], record[20], record[21], record[22],
	            record[23], record[24], record[25]);
	std::fflush(stdout);
	BvhNodeCapture::Snapshot snapshot;
	snapshot.view = BvhNodeCapture::View::SynchronizedBuffer;
	snapshot.raw_event = event_words;
	bool record_written = false, ray_written = false, manifest_written = false;
	std::filesystem::path folder;
	std::string io_error;
	if (Config::GraphicsDebugDumpEnabled() && GuestGpu::IsGpuThread()) {
		const auto base = Config::GetShaderLogFolder() / "bvh_nodes";
		std::error_code error;
		std::filesystem::create_directories(base, error);
		static std::atomic_uint64_t id = 0;
		bool created = false;
		for (unsigned attempt = 0; !error && attempt < 64u && !created; ++attempt) {
			folder = base / fmt::format("{:04d}_{:016x}_{:016x}", id++, event.shader_hash, event.guest_pc);
			created = std::filesystem::create_directory(folder, error);
		}
		if (!created || error) {
			io_error = error ? error.message() : "unique diagnostic directory attempts exhausted";
			snapshot.error = "capture directory unavailable; no node read";
		} else {
			std::array<uint8_t, Diagnostics::BvhDiagnosticWords * sizeof(uint32_t)> raw {};
			for (size_t word = 0; word < record.size(); ++word)
				for (size_t byte = 0; byte < sizeof(uint32_t); ++byte)
					raw[word * 4u + byte] = static_cast<uint8_t>(record[word] >> (byte * 8u));
			record_written = WriteBvhCaptureFile(folder / "event32.bin", raw.data(), raw.size());
			ray_written = sidecar_valid && WriteBvhCaptureFile(folder / "ray.bin",
			    raw.data() + Diagnostics::BvhRayWord * 4u, Diagnostics::BvhRayWords * 4u);
			// Persist raw event/ray words before synchronization, which can itself fail.
			// Process(true) has returned from deferred callbacks before this readback.
			// No following PM4 consumer has executed; unrelated CPU writers remain live.
			if (sidecar_valid && record_written && ray_written) {
				snapshot = BvhNodeCapture::CaptureView(event, BvhNodeCapture::View::SynchronizedBuffer,
				                                    {ReadBvhBufferView, nullptr});
			} else {
				snapshot.error = !sidecar_valid ? "invalid ray sidecar; no node read"
				                              : "raw event/ray could not be persisted; no node read";
			}
			std::string manifest = "First-active BVH diagnostic snapshot\n"
			    "record_schema=BVHR version=1 raw_record_bytes=128 ray_raw_dwords=10\n"
			    "capture_thread=guest_gpu after_dispatch_wait=true outside_deferred_callback=true\n"
			    "view=synchronized_buffer_cache cpu_before_not_captured=true\n"
			    "unrelated_guest_CPU_writers_not_frozen=true event_time_atomicity_unproved=true\n"
			    "maximum_requested_node_bytes=128 child_traversal=false intersection_not_executed=true\n";
			manifest += fmt::format("sidecar_valid={} selected_native_wave_lane={} record_written={} ray_written={}\n",
			    sidecar_valid, record[Diagnostics::BvhNativeLaneWord], record_written, ray_written);
			manifest += BvhNodeCapture::Describe(event) + "\n" + BvhNodeCapture::Describe(snapshot) + "\n";
			for (size_t block = 0; block < snapshot.blocks.size(); ++block) {
				const auto& outcome = snapshot.blocks[block];
				bool written = false;
				if (outcome.succeeded && outcome.payload.size() == BvhNodeCapture::NodeBlockBytes)
					written = WriteBvhCaptureFile(folder / fmt::format("node{}-buffer-view.bin", block),
					                             outcome.payload.data(), outcome.payload.size());
				manifest += fmt::format("node_payload[{}]_written={}\n", block, written);
			}
			manifest_written = WriteBvhCaptureFile(folder / "manifest.txt", manifest.data(), manifest.size());
		}
	} else {
		snapshot.error = !GuestGpu::IsGpuThread() ? "not on GPU command thread; no node read"
		                                       : "graphics diagnostic dump disabled; no node read";
	}
	std::printf("BVH node capture: sidecar_valid=%u selected_lane=%u node_width=%u "
	            "requested=%zu succeeded=%zu complete=%u event_written=%u ray_written=%u "
	            "manifest_written=%u folder=%s io_error=%s read_error=%s\n",
	            sidecar_valid, record[Diagnostics::BvhNativeLaneWord], record[Diagnostics::BvhNodeWidthWord],
	            snapshot.requested_bytes, snapshot.succeeded_bytes, snapshot.complete,
	            record_written, ray_written, manifest_written, folder.string().c_str(),
	            io_error.c_str(), snapshot.error.c_str());
	std::fflush(stdout);
	EXIT("Shader first-active BVH diagnostic stop: derived_node_address=0x%016" PRIx64
	     " guest_pc=0x%016" PRIx64 " shader=0x%016" PRIx64
	     " descriptor=[%08x,%08x,%08x,%08x] raw_node=0x%08x%08x exec=0x%08x%08x"
	     "; bounded snapshot attempted after wait, stopped before intersection or following consumers\n",
	     event.reported_address, event.guest_pc, event.shader_hash, record[8], record[9], record[10], record[11],
	     record[13], record[12], record[15], record[14]);
}

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
		if (m_pending_bvh) FinalizeBvhCapture();
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
	m_scheduler.DeferOperation([this, offset, area, wait_for_completion] {
		m_download_buffer.Invalidate(offset, RecordSize);
		std::array<uint32_t, ShaderRecompiler::Diagnostics::BvhDiagnosticWords> record {};
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
			if (!wait_for_completion) EXIT("BVH snapshot requires synchronous fault inspection; no node read\n");
			m_pending_bvh = record;
			return;
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
		if (m_pending_bvh) FinalizeBvhCapture();
	}
}

} // namespace Libs::Graphics
