#include "graphics/host_gpu/renderer/cache/shaderCallFaultManager.h"

#include "common/assert.h"
#include "common/atomicFileReplace.h"
#include "graphics/shader/recompiler/BvhResultHostPolicy.h"
#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/BvhNodeCapture.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <fmt/format.h>

namespace Libs::Graphics {

namespace {

uint64_t CheckedFaultRecordSize() {
    const auto& mode = ShaderRecompiler::Diagnostics::FrozenAfterBvhMode();
    if (mode.error != ShaderRecompiler::Diagnostics::AfterBvhModeError::None)
        EXIT("Invalid after-BVH mode: %s\n", ShaderRecompiler::Diagnostics::AfterBvhModeErrorName(mode.error));
    return ShaderRecompiler::Diagnostics::FaultRecordBytes(mode.enabled);
}
uint64_t CheckedFaultAreaStride(uint64_t bytes,uint64_t atom,uint64_t areas) {
    uint64_t stride = 0;
    if (!ShaderRecompiler::Diagnostics::FaultAreaStride(bytes,atom,areas,stride))
        EXIT("Invalid fault readback stride: record=%" PRIu64 " atom=%" PRIu64 " areas=%" PRIu64 "\n",bytes,atom,areas);
    return stride;
}

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
	const auto completed_dispatch_tick = std::exchange(m_pending_bvh_tick, 0);
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
	bool resident_attempt_written = false, resident_manifest_written = false;
	bool table_written = false, owner_written = false;
	std::optional<BdaResidentCapture> resident_capture;
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
			if (sidecar_valid && event.may_read && record_written && ray_written) {
				const auto attempt = fmt::format(
				    "scope=post_dispatch_resident_view_before_cpu_backing_read\n"
				    "event_time_mapping_proven=false no_physical_dereference=true\n"
				    "guest_address=0x{:016x} node_bytes={} dispatch_completed_tick={}\n"
				    "maximum_source_copy_bytes=136 maximum_private_download_bytes=144\n"
				    "capture_not_yet_completed=true\n",
				    event.address, event.block_count * BvhNodeCapture::NodeBlockBytes, completed_dispatch_tick);
				resident_attempt_written = WriteBvhCaptureFile(folder / "resident-attempt.txt",
				    attempt.data(), attempt.size());
				if (resident_attempt_written) {
					// Query/copy existing GPU resources before TryReadBufferBacking can discover
					// or merge an owner. The capture API never resolves physical BDA pointers.
					resident_capture = m_scheduler.Context().GetBufferCache().CaptureResidentBdaView(
					    event.address, event.block_count * BvhNodeCapture::NodeBlockBytes, completed_dispatch_tick);
					const auto& resident = *resident_capture;
					if (resident.table_copied)
						table_written = WriteBvhCaptureFile(folder / "gpu-table-entry.bin",
						    resident.table_bytes.data(), resident.table_bytes.size());
					if (resident.owner_copied && resident.plan.node_bytes > 0 &&
					    resident.plan.node_bytes <= resident.owner_bytes.size())
						owner_written = WriteBvhCaptureFile(folder / "node-existing-owner.bin",
						    resident.owner_bytes.data(), static_cast<size_t>(resident.plan.node_bytes));
					auto resident_manifest = Libs::Graphics::Describe(resident);
					resident_manifest += fmt::format("table_payload_written={} owner_payload_written={}\n",
					    table_written, owner_written);
					resident_manifest_written = WriteBvhCaptureFile(folder / "resident-manifest.txt",
					    resident_manifest.data(), resident_manifest.size());
					std::printf("BVH resident BDA capture: plan=%s assessment=%s table_copied=%u "
					            "owner_copied=%u owner_stable=%u mapping_matches=%u "
					            "dispatch_tick=%" PRIu64 " copy_tick=%" PRIu64 "\n",
					            BdaResidentPlanStatusName(resident.plan.status),
					            BdaResidentAssessmentName(AssessBdaResidentCapture(resident)),
					            resident.table_copied, resident.owner_copied, resident.owner_stable_after_wait,
					            resident.page_mapping_matches, resident.dispatch_completed_tick, resident.copy_wait_tick);
					std::fflush(stdout);
				}
				const bool persisted = resident_capture && resident_manifest_written &&
				    (!resident_capture->table_copied || table_written) &&
				    (!resident_capture->owner_copied || owner_written);
				if (persisted && resident_capture->dispatch_tick_confirmed &&
				    resident_capture->plan.table_copy_allowed && resident_capture->copy_wait_completed) {
					snapshot = BvhNodeCapture::CaptureView(event, BvhNodeCapture::View::SynchronizedBuffer,
					                                    {ReadBvhBufferView, nullptr});
				} else {
					snapshot.error = "resident query/wait/evidence incomplete; no CPU backing node read";
				}
			} else {
				snapshot.error = !sidecar_valid || !event.may_read ? "invalid ray/event; no node read"
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
			manifest += fmt::format("resident_attempt_written={} resident_manifest_written={} "
			    "table_payload_written={} owner_payload_written={}\n",
			    resident_attempt_written, resident_manifest_written, table_written, owner_written);
			bool node_files_complete = snapshot.complete;
			for (size_t block = 0; block < snapshot.blocks.size(); ++block) {
				const auto& outcome = snapshot.blocks[block];
				bool written = false;
				if (outcome.succeeded && outcome.payload.size() == BvhNodeCapture::NodeBlockBytes)
					written = WriteBvhCaptureFile(folder / fmt::format("node{}-buffer-view.bin", block),
					                             outcome.payload.data(), outcome.payload.size());
				manifest += fmt::format("node_payload[{}]_written={}\n", block, written);
				node_files_complete = node_files_complete && written;
			}
			bool views_comparable = resident_capture && owner_written && node_files_complete &&
			    resident_capture->owner_copied && snapshot.complete &&
			    resident_capture->plan.node_bytes == snapshot.succeeded_bytes;
			bool views_equal = views_comparable;
			size_t compared_bytes = 0;
			if (views_comparable) {
				for (const auto& block: snapshot.blocks) {
					if (!block.succeeded || block.payload.size() != BvhNodeCapture::NodeBlockBytes ||
					    compared_bytes > resident_capture->plan.node_bytes ||
					    block.payload.size() > resident_capture->plan.node_bytes - compared_bytes) {
						views_comparable = views_equal = false;
						break;
					}
					views_equal = views_equal && std::equal(block.payload.begin(), block.payload.end(),
					    resident_capture->owner_bytes.begin() + compared_bytes);
					compared_bytes += block.payload.size();
				}
				views_comparable = views_comparable && compared_bytes == resident_capture->plan.node_bytes;
				views_equal = views_equal && views_comparable;
			}
			manifest += fmt::format("existing_owner_vs_backing_comparable={} "
			    "existing_owner_vs_backing_bytes_equal={} event_time_atomicity_proven=false\n",
			    views_comparable, views_equal);
			if (resident_capture) manifest += Libs::Graphics::Describe(*resident_capture);
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

void ShaderCallFaultManager::FinalizeBvhResultCapture() {
    using namespace ShaderRecompiler;
    if (!m_pending_bvh_result) EXIT("No pending after-BVH raw record; terminal stop\n");
    const auto record = *m_pending_bvh_result;
    m_pending_bvh_result.reset();
    const auto tick = std::exchange(m_pending_bvh_result_tick,0);
    const bool tick_complete = tick != 0 && m_scheduler.IsFree(tick);
    const bool on_gpu_thread = GuestGpu::IsGpuThread();
    const bool sidecar_valid = Diagnostics::ValidBvhResultSidecar(record);
    const auto pc = uint64_t{record[4]} | (uint64_t{record[5]} << 32);
    // This first result capture performs no node/BDA/guest-memory reads. Native
    // opcode/address cross-checking is an explicit later decoder extension.
    const bool result_valid = sidecar_valid && tick_complete && on_gpu_thread && pc != 0;
    const char* status = record[0] == 0 ? "empty_no_bvh_result" :
        record[0] != 1 ? "invalid_claim_no_bvh_result" :
        record[1] != 8 ? "other_terminal_record_no_bvh_result" :
        !result_valid ? "invalid_kind8_no_validated_result" : "completed_structural_kind8_result";
    bool raw_written=false,ray_written=false,result_written=false,manifest_written=false;
    std::filesystem::path folder;
    std::string io_error;
    if (Config::GraphicsDebugDumpEnabled() && on_gpu_thread && tick_complete) {
        const auto base=Config::GetShaderLogFolder()/"bvh_results";
        std::error_code error;std::filesystem::create_directories(base,error);
        static std::atomic_uint64_t id=0;bool created=false;
        for (unsigned attempt=0;!error&&attempt<64&&!created;++attempt) {
            folder=base/fmt::format("{:04d}_{:016x}_{:016x}",id++,
                uint64_t{record[6]}|(uint64_t{record[7]}<<32),pc);
            created=std::filesystem::create_directory(folder,error);
        }
        if (!created || error) io_error=error?error.message():"unique directory attempts exhausted";
        else {
            const auto raw=Diagnostics::EncodeBvhResultLittleEndian(record);
            raw_written=WriteBvhCaptureFile(folder/"event34.bin",raw.data(),raw.size());
            // Never interpret a call/layout/prior fault or invalid sidecar as a BVH ray/result.
            if (result_valid && raw_written) {
                ray_written=WriteBvhCaptureFile(folder/"ray.bin",raw.data()+Diagnostics::BvhRayWord*4,
                                               Diagnostics::BvhRayWords*4);
                result_written=WriteBvhCaptureFile(folder/"result4.bin",raw.data()+Diagnostics::BvhResultWord*4,
                                                  Diagnostics::BvhResultWords*4);
            }
            if (!raw_written) io_error="event34 write/flush/close failed";
            else if (result_valid && !ray_written) io_error="ray40 write/flush/close failed";
            else if (result_valid && !result_written) io_error="result16 write/flush/close failed";
            const bool complete=result_valid && raw_written && ray_written && result_written;
            const auto manifest=fmt::format(
                "capture_schema=after_bvh_host_v1 record_schema={} version={} raw_record_bytes=136 expected_ray_raw_bytes=40 expected_result_raw_bytes=16\n"
                "status={} complete={} result_structurally_validated={} sidecar_valid={}\n"
                "record_kind={} claimed={} after_dispatch_wait={} dispatch_tick={}\n"
                "outside_deferred_callback=true capture_thread=guest_gpu\n"
                "intersection_helper_executed_from_emitter_record={} result_same_CAS_winner_by_emitter_protocol={} nativePC_nonzero={}\n"
                "raw_payload_written={} ray_payload_written={} result_payload_written={}\n"
                "node_reads_attempted=0 node_or_BDA_view_captured=false native_opcode_decoded_by_host=false\n"
                "event_time_memory_atomicity_proven=false recorded_active_wave_returned_after_result={} following_PM4_consumers_permitted=false\n"
                "other_record_kind_preserved=true global_CAS_winner_order_unproved=true\n",
                result_valid ? "BVHR" : "raw_fault_storage", result_valid ? 2u : 0u,
                status,complete,result_valid,sidecar_valid,record[1],record[0],tick_complete,tick,
                result_valid,result_valid,pc!=0,raw_written,ray_written,result_written,result_valid);
            const auto temporary=folder/"manifest-final.tmp";
            if (WriteBvhCaptureFile(temporary,manifest.data(),manifest.size())) {
                manifest_written=Common::AtomicReplaceFile(temporary,folder/"manifest.txt");
                if (!manifest_written && io_error.empty()) io_error="manifest atomic replacement failed";
            } else if (io_error.empty()) io_error="manifest temporary write/flush/close failed";
        }
    } else io_error=!on_gpu_thread?"not GPU thread":!tick_complete?"dispatch not complete":"debug dumping disabled";
    std::printf("After-BVH diagnostic: status=%s kind=%u claimed=%u sidecar_valid=%u "
                "dispatch_complete=%u tick=%" PRIu64 " event_written=%u ray_written=%u result_written=%u "
                "manifest_written=%u folder=%s error=%s\n",status,record[1],record[0],sidecar_valid,
                tick_complete,tick,raw_written,ray_written,result_written,manifest_written,folder.string().c_str(),io_error.c_str());
    // Preserve every known prior claim's raw metadata without decoding it as results.
    std::printf("After-BVH raw header: detail=0x%08x%08x pc=0x%08x%08x hash=0x%08x%08x "
                "words8_15=[%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x]\n",
                record[3],record[2],record[5],record[4],record[7],record[6],
                record[8],record[9],record[10],record[11],record[12],record[13],record[14],record[15]);
    if (result_valid)
        std::printf("After-BVH result bits: [%08x,%08x,%08x,%08x] selected_lane=%u\n",
                    record[30],record[31],record[32],record[33],record[27]);
    std::fflush(stdout);
    EXIT("After-BVH diagnostic terminal stop: status=%s files_complete=%u; "
         "no following shader/PM4 consumers permitted\n",status,
         result_valid&&raw_written&&ray_written&&result_written&&manifest_written);
}

ShaderCallFaultManager::ShaderCallFaultManager(GraphicContext& graphics,
                                               CommandScheduler& scheduler)
    : m_scheduler(scheduler),
      m_record_size(CheckedFaultRecordSize()),
      m_fault_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, m_record_size),
	  m_area_stride(CheckedFaultAreaStride(m_record_size,
          graphics.physical_device_properties.limits.nonCoherentAtomSize,MaxPending)),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
	                        MaxPending * m_area_stride) {
	m_fault_buffer.Fill(0, m_record_size, 0);
}

Buffer* ShaderCallFaultManager::GetBuffer(uint64_t required_record_bytes) noexcept {
	if ((required_record_bytes != 128u && required_record_bytes != 136u) || required_record_bytes > m_record_size)
		EXIT("Shader fault record exceeds fixed process capacity: required=%" PRIu64 " capacity=%" PRIu64 "\n",required_record_bytes,m_record_size);
	m_used = true;
	return &m_fault_buffer;
}

void ShaderCallFaultManager::Process(bool wait_for_completion,bool after_bvh_capture) {
	if (after_bvh_capture && (!wait_for_completion || m_record_size != 136u || !m_used))
		EXIT("After-BVH inspection requires used136B channel and synchronous wait; terminal stop\n");
	if (!m_used) return;
	m_used = false;
	if (const auto tick = m_ticks[m_area]; tick != 0u) {
		m_scheduler.Wait(tick);
		m_scheduler.PopPendingOperations();
		if (m_pending_bvh) FinalizeBvhCapture();
		if (m_pending_bvh_result) FinalizeBvhResultCapture();
	}
	const auto area = m_area;
	// Each pending read occupies its own noncoherent atom. Invalidation must not overlap a
	// different in-flight slot even when the device's atom is larger than the fault record.
	const uint64_t offset = area * m_area_stride;
	m_download_buffer.CopyFrom(m_scheduler.Current(), m_fault_buffer, 0, offset, m_record_size,
	                           vk::AccessFlagBits::eShaderWrite,
	                           vk::AccessFlagBits::eHostRead,
	                           vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
	                           vk::AccessFlagBits::eHostRead);
	const auto scheduled_tick = m_scheduler.CurrentTick();
	m_scheduler.DeferOperation([this, offset, area, wait_for_completion, after_bvh_capture, scheduled_tick] {
		m_download_buffer.Invalidate(offset, m_record_size);
		std::array<uint32_t, ShaderRecompiler::Diagnostics::BvhMaximumDiagnosticWords> record {};
		std::memcpy(record.data(), m_download_buffer.Mapped().data() + offset, m_record_size);
		m_ticks[area] = 0;
		if (after_bvh_capture) {
			m_pending_bvh_result = record;
			m_pending_bvh_result_tick = scheduled_tick;
			return; // Empty/other CAS claims are also finalized outside callbacks.
		}
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
			std::array<uint32_t, ShaderRecompiler::Diagnostics::BvhDiagnosticWords> before_record {};
			std::copy_n(record.begin(),before_record.size(),before_record.begin());
			m_pending_bvh = before_record;
			m_pending_bvh_tick = scheduled_tick;
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
	m_ticks[m_area] = scheduled_tick;
	m_area = (m_area + 1u) % MaxPending;
	if (wait_for_completion) {
		// Checked external calls may return early at a diagnostic fault. Finish their dispatch
		// and inspect the record before following PM4 consumers can use incomplete results.
		m_scheduler.Wait(scheduled_tick);
		m_scheduler.PopPendingOperations();
		if (m_pending_bvh) FinalizeBvhCapture();
		if (m_pending_bvh_result) FinalizeBvhResultCapture();
	}
}

} // namespace Libs::Graphics
