#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/MenuPerformanceDiagnostic.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/BvhResultHostPolicy.h"
#include "common/atomicFileReplace.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderCallDiagnostics.h"
#include "graphics/shader/recompiler/ExternalLibrary.h"
#include "graphics/shader/recompiler/SelectedCalleeCapture.h"
#include "graphics/shader/recompiler/PixelSampleSensitivity.h"
#include "graphics/shader/recompiler/EqaaReduced2xPolicy.h"
#include "graphics/host_gpu/renderer/eqaaDepthState.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/shader/recompiler/ir/passes/SrtReadCapture.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <fmt/format.h>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

bool ExternalProbeRequested() {
	const auto* value = std::getenv("KYTY_PROBE_EXTERNAL_CALL_TARGET");
	return value != nullptr && std::strcmp(value, "1") == 0;
}

bool ExternalBvhProbeRequested() {
	const auto* value = std::getenv("KYTY_PROBE_EXTERNAL_BEFORE_BVH");
	if (value == nullptr) return false;
	if (std::strcmp(value, "1") != 0 || !ExternalProbeRequested()) {
		EXIT("KYTY_PROBE_EXTERNAL_BEFORE_BVH requires value 1 and the external-call probe\n");
	}
	return true;
}

bool ExternalAfterBvhProbeRequested() {
    using namespace ShaderRecompiler::Diagnostics;
    const auto& frozen=FrozenAfterBvhMode();
    if (frozen.error!=AfterBvhModeError::None)
        EXIT("Invalid after-BVH mode: %s\n",AfterBvhModeErrorName(frozen.error));
    const auto current=ParseAfterBvhMode({
        std::getenv("KYTY_PROBE_EXTERNAL_AFTER_BVH"),std::getenv("KYTY_PROBE_EXTERNAL_CALL_TARGET"),
        std::getenv("KYTY_PROBE_EXTERNAL_BEFORE_BVH"),std::getenv("KYTY_EXTERNAL_UNWRITTEN_VGPR"),
        std::getenv("KYTY_CAPTURE_EXTERNAL_RESOURCE_READS"),std::getenv("KYTY_CAPTURE_EXTERNAL_INPUTS_ONLY"),
        std::getenv("KYTY_PROBE_EXTERNAL_STRUCTURED")});
    if (current.error!=frozen.error || current.enabled!=frozen.enabled || current.structured!=frozen.structured)
        EXIT("After-BVH mode cannot change after process initialization\n");
    return frozen.enabled;
}

bool ExternalStructuredProbeRequested() {
	const auto* value = std::getenv("KYTY_PROBE_EXTERNAL_STRUCTURED");
	if (value == nullptr) return false;
	if (std::strcmp(value, "1") != 0 || !ExternalProbeRequested()) {
		EXIT("KYTY_PROBE_EXTERNAL_STRUCTURED requires value 1 and the external-call probe\n");
	}
	const auto* capture = std::getenv("KYTY_CAPTURE_EXTERNAL_RESOURCE_READS");
	if (capture != nullptr && std::strcmp(capture, "1") == 0) {
		EXIT("Structured external probe and resource-read-only capture require separate runs\n");
	}
	return true;
}

uint32_t ExternalUnwrittenVgprRequested() {
	const auto* value = std::getenv("KYTY_EXTERNAL_UNWRITTEN_VGPR");
	if (value == nullptr) return std::numeric_limits<uint32_t>::max();
	uint32_t index = 0;
	const auto length = std::strlen(value);
	const auto parsed = std::from_chars(value, value + length, index);
	if (length == 0 || parsed.ec != std::errc {} || parsed.ptr != value + length || index >= 256u) {
		EXIT("Invalid KYTY_EXTERNAL_UNWRITTEN_VGPR: expected an integer from 0 to 255\n");
	}
	if (ExternalProbeRequested()) {
		EXIT("External-call probe and checked unwritten-VGPR mode cannot be combined\n");
	}
	return index;
}

bool HasExternalCall(std::span<const uint32_t> code) {
	if (!std::ranges::any_of(code, [](uint32_t word) {
		return ((word >> 23u) & 0x1ffu) == 0x17du && ((word >> 8u) & 0xffu) == 0x21u &&
		       ((word >> 16u) & 0x7fu) != 125u;
	})) return false;
	ShaderRecompiler::Decoder::Program decoded;
	ShaderRecompiler::Decoder::DecodeProgram(code, decoded);
	return std::ranges::any_of(decoded.instructions, [](const auto& inst) {
		return inst.opcode == ShaderRecompiler::Decoder::Opcode::S_SWAPPC_B64 &&
		       ((inst.raw[0] >> 16u) & 0x7fu) != 125u;
	});
}

uint8_t RemapSourceAlphaFactor(uint8_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Alpha);
		case Prospero::BlendFactor::kOneMinusSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
		default: return factor;
	}
}

vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back) {
	// CxPrimitiveSetup::PolygonMode disables both per-face modes when it is zero.
	if (mode.poly_mode == 0) {
		return vk::PolygonMode::eFill;
	}
	EXIT_NOT_IMPLEMENTED(mode.poly_mode != 1);
	if (cull_front && cull_back) {
		return vk::PolygonMode::eFill;
	}
	if (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype) {
		EXIT("Pipeline: different polygon modes for two visible faces are unsupported\n");
	}
	// Vulkan has one polygon mode. A culled face does not constrain that mode.
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	switch (polygon_mode) {
		case 0: return vk::PolygonMode::ePoint;
		case 1: return vk::PolygonMode::eLine;
		case 2: return vk::PolygonMode::eFill;
		default: EXIT("Pipeline: invalid polygon mode %u\n", polygon_mode);
	}
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return fmt::format("KytyPC1:{}:{:08x}:{:08x}:{:08x}:{}\n", KYTY_GIT_REVISION,
	                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
}

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	// Scalar and unformatted buffer dependencies use the same backing as native raw loads.
	// Image synchronization belongs to formatted buffer bindings, not these reads.
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadBufferBacking(address, values.data(), values.size_bytes());
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}.bin", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(code.data(), code.size_bytes());
}

bool WriteCallCaptureFile(const std::filesystem::path& path, const void* data, size_t bytes) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) return false;
	file.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
	file.close();
	return static_cast<bool>(file);
}

bool SelectedCalleeResourceCaptureRequested();

bool DumpShaderCallInputs(const char* stage_name,
                          const ShaderRecompiler::CompileOptions& options,
                          std::span<const uint32_t> code,
                          std::filesystem::path* output_folder = nullptr,
                          bool selected_capture = false) {
	// Only the validated selected CPU capture may collect required artifacts with
	// broad shader dumps disabled. Ordinary capture callers retain the old gate.
	if (selected_capture &&
	    (!SelectedCalleeResourceCaptureRequested() || options.stage != ShaderType::Compute ||
	     options.shader_hash != 0x3351560625c27256ull || output_folder == nullptr)) return false;
	// Other stages can have fused/front-only code spans with different decoding rules.
	if ((!Config::GraphicsDebugDumpEnabled() && !selected_capture) ||
	    options.stage != ShaderType::Compute) return false;
	const auto could_call = [](uint32_t word) {
		return ((word >> 23u) & 0x1ffu) == 0x17du && ((word >> 8u) & 0xffu) == 0x21u &&
		       ((word >> 16u) & 0x7fu) != 125u;
	};
	if (!std::ranges::any_of(code, could_call)) return false;
	ShaderRecompiler::Decoder::Program decoded;
	ShaderRecompiler::Decoder::DecodeProgram(code, decoded);
	const auto capture = ShaderRecompiler::Diagnostics::CaptureCallTables(
	    true, decoded, options.user_data, ReadShaderGuestMemory, nullptr, options.user_data_base);
	if (capture.tables.empty()) return false;
	const auto direct_loads = ShaderRecompiler::Diagnostics::CaptureDirectUserLoads(
	    true, decoded, options.user_data, ReadShaderGuestMemory, nullptr, options.user_data_base);

	static std::atomic_uint64_t id = 0;
	std::filesystem::path folder;
	std::error_code error;
	do {
		folder = Config::GetShaderLogFolder() / "external_calls" /
		         fmt::format("{:04d}_{}_{:016x}", id++, stage_name, options.shader_hash);
	} while (std::filesystem::exists(folder, error) && !error);
	if (error) {
		PipelineCacheLog("External-call diagnostic directory query failed: {}", error.message());
		return false;
	}
	std::filesystem::create_directories(folder, error);
	if (error) {
		PipelineCacheLog("External-call diagnostic directory creation failed: {}", error.message());
		return false;
	}
	if (output_folder != nullptr) *output_folder = folder;
	std::string manifest = fmt::format(
	    "External shader-call diagnostic capture\n"
	    "stage={} caller_hash=0x{:016x} caller_span_bytes={} decoded_instructions={}\n"
	    "runtime_scope=compute_only; other stages were not examined\n"
	    "selection=unknown; all records are candidates, no function was executed\n"
	    "prefixes are raw mapped-memory snapshots, not proven function boundaries\n"
	    "limits: call_sites={} aggregate_table_bytes={} targets={} target_prefix_bytes={} "
	    "aggregate_target_bytes={} read_chunk_bytes={}\n"
	    "descriptor_read_requests={} descriptor_requested_bytes={} "
	    "table_bytes_reserved={} table_read_bytes_requested={} target_bytes_reserved={} "
	    "target_read_bytes_requested={}\n"
	    "call_sites_truncated={} table_budget_exhausted={} target_limit_reached={} "
	    "target_budget_exhausted={}\n"
	    "zero_targets={} misaligned_targets={} outside_48bit_targets={} duplicate_targets={}\n"
	    "Raw table files preserve every captured 64-bit target and both auxiliary DWORDs, "
	    "including excluded or repeated targets.\n\n",
	    stage_name, options.shader_hash, code.size_bytes(), decoded.instructions.size(),
	    ShaderRecompiler::Diagnostics::MaxCallSites, ShaderRecompiler::Diagnostics::MaxTableBytes,
	    ShaderRecompiler::Diagnostics::MaxTargets, ShaderRecompiler::Diagnostics::MaxTargetBytes,
	    ShaderRecompiler::Diagnostics::MaxAggregateTargetBytes,
	    ShaderRecompiler::Diagnostics::ReadChunkBytes,
	    capture.descriptor_read_requests, capture.descriptor_read_requests * 16u,
	    capture.table_bytes_reserved, capture.table_read_bytes_requested,
	    capture.target_bytes_reserved,
	    capture.target_read_bytes_requested, capture.call_sites_truncated,
	    capture.table_budget_exhausted, capture.target_limit_reached, capture.target_budget_exhausted,
	    capture.zero_targets,
	    capture.misaligned_targets, capture.outside_address_space_targets, capture.duplicate_targets);
	manifest += fmt::format("caller_base=0x{:016x} wave_size={} user_data_base={} user_data_count={}\n",
	                        reinterpret_cast<uint64_t>(code.data()), options.wave_size,
	                        options.user_data_base, options.user_data.size());
	if (const auto* compute = options.input_info.compute; compute != nullptr) {
		manifest += fmt::format(
		    "threads_num={},{},{} host_subgroup_size={} lds_size_dwords={} scratch_size_dwords={}\n",
		    compute->threads_num[0], compute->threads_num[1], compute->threads_num[2],
		    compute->host_subgroup_size, compute->lds_size_dwords, compute->scratch_size_dwords);
		manifest += fmt::format(
		    "dispatch_threads_num={},{},{} workgroup_counts={},{},{} group_id={},{},{} "
		    "thread_ids_num={} workgroup_register={} tg_size_en={} "
		    "dispatch_thread_dimensions={} lds_storage={} float_mode=0x{:02x}\n",
		    compute->dispatch_threads_num[0], compute->dispatch_threads_num[1],
		    compute->dispatch_threads_num[2], compute->workgroup_counts[0],
		    compute->workgroup_counts[1], compute->workgroup_counts[2],
		    compute->group_id[0], compute->group_id[1], compute->group_id[2],
		    compute->thread_ids_num, compute->workgroup_register, compute->tg_size_en,
		    compute->dispatch_thread_dimensions, compute->lds_storage, compute->float_mode);
	}
	for (size_t word = 0; word < options.user_data.size(); ++word)
		manifest += fmt::format("user_sgpr[{}]=0x{:08x}\n", options.user_data_base + word,
		                        options.user_data[word]);
	manifest += "\n";
	bool files_written = WriteCallCaptureFile(folder / "caller.bin", code.data(), code.size_bytes());
	manifest += fmt::format("caller_file=caller.bin caller_file_written={}\n", files_written);
	manifest += fmt::format("direct_user_loads={} direct_user_load_limit={} "
	    "direct_user_load_max_dwords=16 direct_user_load_limit_reached={} "
	    "direct_user_load_requested_bytes={}\n", direct_loads.loads.size(),
	    ShaderRecompiler::Diagnostics::MaxDirectUserLoads, direct_loads.limit_reached,
	    direct_loads.requested_bytes);
	for (size_t i = 0; i < direct_loads.loads.size(); ++i) {
		const auto& load = direct_loads.loads[i];
		std::string file_name = "none";
		if (!load.words.empty()) {
			file_name = fmt::format("direct_user_{:03d}_{:08x}.bin", i, load.pc);
			if (!WriteCallCaptureFile(folder / file_name, load.words.data(),
			                          load.words.size() * sizeof(uint32_t))) {
				file_name += " (write failed)";
				files_written = false;
			}
		}
		manifest += fmt::format("direct_user_load[{}]: pc=0x{:08x} destination_sgpr={} "
		    "user_sgpr={} dword_count={} offset={} address=0x{:016x} "
		    "read_failed={} rejection={} file={}\n", i, load.pc, load.destination_sgpr,
		    load.user_sgpr, load.dword_count, load.offset, load.address, load.read_failed,
		    load.rejection.empty() ? "none" : load.rejection, file_name);
	}
	for (size_t i = 0; i < capture.tables.size(); ++i) {
		const auto& table = capture.tables[i];
		const auto& trace = table.trace;
		std::string table_file = "none";
		if (!table.words.empty()) {
			table_file = fmt::format("table_{:02d}.bin", i);
			if (!WriteCallCaptureFile(folder / table_file, table.words.data(),
			                          table.words.size() * sizeof(uint32_t))) {
				table_file += " (write failed)";
				files_written = false;
			}
		}
		manifest += fmt::format(
		    "call[{}]: pc=0x{:08x} raw=0x{:08x} target_encoded_pair={} return_encoded_pair={} "
		    "aliased={}\n"
		    "  record_load_pc=0x{:08x} record_offset={} descriptor_load_pc=0x{:08x}\n"
		    "  user_sgpr_pair={} user_words=[0x{:08x},0x{:08x}] descriptor_offset={} "
		    "descriptor_address=0x{:016x}\n"
		    "  status={} rejection={} descriptor_read={} "
		    "descriptor_words=[0x{:08x},0x{:08x},0x{:08x},0x{:08x}]\n"
		    "  aligned_table_base=0x{:016x} declared_table_bytes={} captured_bytes={} "
		    "truncated={} read_failed={} file={}\n",
		    i, trace.call_pc, trace.raw_call, trace.target_sgpr, trace.return_sgpr,
		    trace.target_sgpr == trace.return_sgpr, trace.record_load_pc, trace.record_offset,
		    trace.descriptor_load_pc, trace.descriptor_user_sgpr, trace.user_words[0],
		    trace.user_words[1], trace.descriptor_offset, trace.descriptor_address,
		    table.status, trace.rejection.empty() ? "none" : trace.rejection,
		    table.descriptor_read, table.descriptor[0], table.descriptor[1], table.descriptor[2],
		    table.descriptor[3], table.table_base, table.table_size,
		    table.words.size() * sizeof(uint32_t), table.table_truncated, table.read_failed, table_file);
	}
	for (size_t i = 0; i < capture.targets.size(); ++i) {
		const auto& target = capture.targets[i];
		std::string target_file = "none";
		if (!target.words.empty()) {
			target_file = fmt::format("target_{:02d}_{:016x}.bin", i, target.raw_address);
			if (!WriteCallCaptureFile(folder / target_file, target.words.data(),
			                          target.words.size() * sizeof(uint32_t))) {
				target_file += " (write failed)";
				files_written = false;
			}
		}
		manifest += fmt::format(
		    "target[{}]: raw_address=0x{:016x} table={} record={} "
		    "aux=[0x{:08x},0x{:08x}] captured_bytes={} read_failed={} prefix_capped={} "
		    "status={} file={} selected=unknown\n",
		    i, target.raw_address, target.table_index, target.record_index, target.auxiliary_words[0],
		    target.auxiliary_words[1], target.words.size() * sizeof(uint32_t), target.read_failed,
		    target.prefix_capped, target.status, target_file);
	}
	const auto path = folder / "manifest.txt";
	if (!WriteCallCaptureFile(path, manifest.data(), manifest.size())) {
		PipelineCacheLog("External-call diagnostic manifest write failed: {}", Common::PathToString(path));
		return false;
	}
	PipelineCacheLog("External-call diagnostic capture (selected function unknown): {}",
	                 Common::PathToString(path));
	return files_written;
}

bool SelectedCalleeResourceCaptureRequested() {
    const auto* requested=std::getenv("KYTY_CAPTURE_SELECTED_CALLEE_RESOURCES");
    if(requested==nullptr)return false;
    const auto present=[](const char* name){return std::getenv(name)!=nullptr;};
    const auto* reference=std::getenv("KYTY_SELECTED_CALLEE_REFERENCE_FILE");
    if(std::strcmp(requested,"1")!=0 || !ExternalProbeRequested() ||
       reference==nullptr || *reference=='\0' ||
       present("KYTY_CAPTURE_EXTERNAL_RESOURCE_READS") ||
       present("KYTY_CAPTURE_EXTERNAL_INPUTS_ONLY") ||
       present("KYTY_EXTERNAL_UNWRITTEN_VGPR") ||
       present("KYTY_PROBE_EXTERNAL_BEFORE_BVH") ||
       present("KYTY_PROBE_EXTERNAL_AFTER_BVH"))
        EXIT("Selected-callee resource capture requires exact value1, target probe, reference file, and no other capture/checked/BVH mode\n");
    return true;
}

ShaderRecompiler::Diagnostics::SelectedNativeReference ReadSelectedCalleeReference() {
    using namespace ShaderRecompiler::Diagnostics;
    const std::filesystem::path path(std::getenv("KYTY_SELECTED_CALLEE_REFERENCE_FILE"));
    if(!path.is_absolute())EXIT("Selected-callee native reference must be an absolute owned file path\n");
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)EXIT("Selected-callee native reference open failed\n");
    const auto bytes=file.tellg();
    if(bytes<=0 || bytes>65536 || bytes%4!=0)EXIT("Selected-callee native reference size rejected\n");
    std::vector<uint32_t> words(static_cast<size_t>(bytes)/4u);
    file.seekg(0);file.read(reinterpret_cast<char*>(words.data()),bytes);
    if(!file)EXIT("Selected-callee native reference read failed\n");
    SelectedNativeReference result;std::string failure;
    if(!ParseSelectedNativeReference(words,SelectedReferencePolicy{},result,failure))
        EXIT("Selected-callee native reference rejected: %s\n",failure.c_str());
    return result;
}

bool ExternalResourceReadCaptureRequested() {
	const auto* value = std::getenv("KYTY_CAPTURE_EXTERNAL_RESOURCE_READS");
	if (value == nullptr) return false;
	if (std::strcmp(value, "1") != 0 || !ExternalProbeRequested() ||
	    !Config::GraphicsDebugDumpEnabled()) {
		EXIT("KYTY_CAPTURE_EXTERNAL_RESOURCE_READS requires value 1, the external-call "
		     "probe and graphics debug dumping\n");
	}
	return true;
}

// Canonical topology is retained separately from the bucket hash. Prefix words
// are retained in the exact dependency files, not reread from guest memory.
std::vector<uint64_t> ResourceCaptureLibraryTopology(
    const ShaderRecompiler::ExternalLibraryPlan& library) {
	std::vector<uint64_t> words {1, library.complete, library.caller_address,
	                           library.functions.size(), library.call_sites.size(),
	                           library.dependencies.size()};
	for (const auto& function: library.functions) {
		words.insert(words.end(), {function.function_id, function.guest_address,
		                          function.code_prefix.size()});
	}
	for (const auto& site: library.call_sites) {
		words.insert(words.end(), {site.caller_pc, site.target_sgpr, site.return_sgpr,
		    site.context_domain, site.record_load_pc, site.record_sgpr, site.auxiliary_sgpr,
		    site.descriptor_user_sgpr, static_cast<uint64_t>(static_cast<int64_t>(site.descriptor_offset)),
		    site.descriptor_address, site.table_base, site.table_bytes,
		    site.candidate_addresses.size(), site.records.size(), site.context_records.size()});
		words.insert(words.end(), site.candidate_addresses.begin(), site.candidate_addresses.end());
		for (const auto& record: site.records) {
			words.insert(words.end(), {record.ordinal, record.function_id,
			                          record.function_address, record.auxiliary_address});
		}
		for (const auto& context: site.context_records) {
			words.insert(words.end(), {context.ordinal, context.function_id});
			words.insert(words.end(), context.words.begin(), context.words.end());
		}
	}
	for (const auto& dependency: library.dependencies) {
		words.insert(words.end(), {dependency.address, dependency.words.size()});
	}
	return words;
}

bool DumpResourceReadCapture(const std::filesystem::path& input_folder,
                             const ShaderRecompiler::IR::SrtReadCapture& capture,
                             const ShaderRecompiler::ExternalLibraryPlan& library,
                             std::span<const uint32_t> caller_code,
                             bool input_files_written, bool materialization_succeeded) {
	using Capture = ShaderRecompiler::IR::SrtReadCapture;
	const auto folder = input_folder / "resource_reads";
	std::error_code error;
	if (input_folder.empty() || !std::filesystem::create_directory(folder, error) || error) {
		PipelineCacheLog("Resource-read capture directory unavailable: {}", error.message());
		return false;
	}
	const std::string attempt = "schema=1 complete=false state=writing\n"
	                            "scope=existing_host_materialization_reads_only\n"
	                            "guest_memory_atomicity_proven=false native_pc=unknown\n";
	if (!WriteCallCaptureFile(folder / "manifest.txt", attempt.data(), attempt.size())) return false;
	const auto& identity = capture.Identity();
	const auto& counters = capture.Counters();
	const auto& limits = capture.Limits();
	const auto& failure = capture.Failure();
	bool files_written = input_files_written;
	const auto write = [&](const char* name, const void* data, size_t bytes) {
		const bool written = WriteCallCaptureFile(folder / name, data, bytes);
		files_written &= written;
		return written;
	};
	write("actual-caller.bin", caller_code.data(), caller_code.size_bytes());
	write("user-data.bin", identity.user_data.data(), identity.user_data.size() * sizeof(uint32_t));
	write("control-words.bin", identity.control_words.data(), identity.control_words.size() * sizeof(uint32_t));
	const auto topology = ResourceCaptureLibraryTopology(library);
	write("library-topology-u64le.bin", topology.data(), topology.size() * sizeof(uint64_t));
	std::string dependency_index;
	for (size_t i = 0; i < library.dependencies.size(); ++i) {
		const auto& dependency = library.dependencies[i];
		const auto name = fmt::format("library-dependency-{:04d}.bin", i);
		const bool written = write(name.c_str(), dependency.words.data(), dependency.words.size() * sizeof(uint32_t));
		dependency_index += fmt::format("dependency[{}]: address=0x{:016x} bytes={} file={} written={}\n",
		    i, dependency.address, dependency.words.size() * sizeof(uint32_t), name, written);
	}
	// Explicit packed records avoid structure padding: address low/high then raw DWORD.
	std::vector<uint32_t> packed_words;
	packed_words.reserve(capture.Words().size() * 3u);
	for (const auto& [address, word]: capture.Words()) {
		packed_words.insert(packed_words.end(), {static_cast<uint32_t>(address),
		    static_cast<uint32_t>(address >> 32u), word});
	}
	write("observed-words-u32le.bin", packed_words.data(), packed_words.size() * sizeof(uint32_t));
	std::string observations;
	for (size_t i = 0; i < capture.Observations().size(); ++i) {
		const auto& observation = capture.Observations()[i];
		observations += fmt::format("read[{}]: kind={} address=0x{:016x} bytes={} succeeded={} "
		    "specialization={} context_present={} native_pc={}", i,
		    static_cast<uint32_t>(observation.kind), observation.address, observation.requested_bytes,
		    observation.succeeded, observation.specialization_read, observation.context.has_value(),
		    observation.native_pc ? fmt::format("0x{:08x}", *observation.native_pc) : "unknown");
		if (observation.context) {
			const auto& context = *observation.context;
			observations += fmt::format(" domain={} function={} ordinal={} words={:08x},{:08x},{:08x},{:08x}",
			    context.domain_id, context.function_id, context.record_ordinal, context.record_words[0],
			    context.record_words[1], context.record_words[2], context.record_words[3]);
		}
		observations += '\n';
	}
	write("observations.txt", observations.data(), observations.size());
	const bool complete = materialization_succeeded && capture.Complete() && library.complete && files_written;
	std::string manifest = fmt::format(
	    "schema=1 complete={} collector_complete={} finalized={} status={}\n"
	    "scope=existing_host_materialization_reads_only guest_memory_atomicity_proven=false native_pc_encoding=optional_relative_caller_pc\n"
	    "stage={} shader_hash=0x{:016x} shader_base=0x{:016x} pass_id={} user_data_base={}\n"
	    "caller_identity={} library_identity={} input_identity={}\n"
	    "control_schema={}\n"
	    "materialization_succeeded={} input_files_written={} payload_files_written={} library_complete={} dependency_bucket_hash=0x{:016x}\n"
	    "library_functions={} library_call_sites={} library_dependencies={} topology_u64_words={}\n"
	    "word_record_schema=u32le_address_low,address_high,word word_records={}\n"
	    "hooks={} accepted_observations={} ignored_after_invalidation={} observed_bytes={} unique_words={}\n"
	    "limit_unique_bytes={} limit_observation_bytes={} limit_observations={}\n"
	    "failure_address=0x{:016x} failure_bytes={} previous_word=0x{:08x} conflicting_word=0x{:08x} failure_native_pc={} failure_context_present={}\n",
	    complete, capture.Complete(), capture.Finalized(), Capture::StatusName(capture.Status()),
	    static_cast<uint32_t>(identity.stage), identity.shader_hash, identity.shader_base, identity.pass_id,
	    identity.user_data_base, identity.caller_identity, identity.library_identity, identity.input_identity,
	    identity.control_schema, materialization_succeeded, input_files_written, files_written, library.complete, library.dependency_hash,
	    library.functions.size(), library.call_sites.size(), library.dependencies.size(), topology.size(),
	    capture.Words().size(), counters.hooks, counters.accepted_observations,
	    counters.ignored_after_invalidation, counters.observed_bytes, counters.unique_words,
	    limits.unique_bytes, limits.observation_bytes, limits.observations, failure.address,
	    failure.requested_bytes, failure.previous_word, failure.conflicting_word,
	    failure.native_pc ? fmt::format("0x{:08x}", *failure.native_pc) : "unknown", failure.context.has_value());
	if (failure.context) {
		const auto& context = *failure.context;
		manifest += fmt::format("failure_context: domain={} function={} ordinal={} words={:08x},{:08x},{:08x},{:08x}\n",
		    context.domain_id, context.function_id, context.record_ordinal, context.record_words[0],
		    context.record_words[1], context.record_words[2], context.record_words[3]);
	}
	manifest += dependency_index;
	// A partial final write cannot publish a leading complete=true. On either
	// failure the initial incomplete manifest remains the only published status.
	const auto final_temporary = folder / "manifest-final.tmp";
	if (!WriteCallCaptureFile(final_temporary, manifest.data(), manifest.size()) ||
	    !Common::AtomicReplaceFile(final_temporary, folder / "manifest.txt")) return false;
	PipelineCacheLog("Resource-read diagnostic capture: complete={} status={} words={} observations={} folder={}",
	    complete, Capture::StatusName(capture.Status()), capture.Words().size(),
	    capture.Observations().size(), Common::PathToString(folder));
	return files_written;
}


bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv, bool required = false) {
	if (!required && !Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	if (tools.Validate(spirv)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

} // namespace

std::size_t PipelineCache::GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const {
	std::size_t hash = 0;
	PipelineKeyHash::Mix(hash, key.rendering.color_count);
	for (uint32_t i = 0; i < key.rendering.color_count; i++) {
		PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.color_formats[i]));
	}
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.depth_format));
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.stencil_format));
	for (const auto id: key.vertex_shader_ids) {
		PipelineKeyHash::Mix(hash, id);
	}
	PipelineKeyHash::Mix(hash, key.ps_shader_id);
	PipelineKeyHash::Mix(hash, key.experimental_eqaa_policy);
	PipelineKeyHash::Mix(hash, key.vertex_input.binding_count);
	for (uint32_t i = 0; i < key.vertex_input.binding_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].stride);
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].instance);
	}
	PipelineKeyHash::Mix(hash, key.vertex_input.attribute_count);
	for (uint32_t i = 0; i < key.vertex_input.attribute_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].offset);
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].binding);
	}
	PipelineKeyHash::Mix(hash, XXH3_64bits(&key.static_params, sizeof(key.static_params)));
	return hash;
}

struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		bool                  external_call_probe = false;
		bool                  external_probe_before_bvh = false;
		bool                  external_probe_structured = false;
		bool                  external_probe_after_bvh = false;
		uint32_t              external_unwritten_vgpr = std::numeric_limits<uint32_t>::max();
		std::vector<uint32_t> static_state;
		std::shared_ptr<const ShaderRecompiler::ExternalLibraryPlan> external_library;

		bool operator==(const ProgramKey& other) const {
			if (stage != other.stage || hash != other.hash || user_data_count != other.user_data_count ||
			    code_size != other.code_size || static_state != other.static_state ||
			    external_call_probe != other.external_call_probe ||
			    external_probe_before_bvh != other.external_probe_before_bvh ||
			    external_probe_structured != other.external_probe_structured ||
			    external_probe_after_bvh != other.external_probe_after_bvh ||
			    external_unwritten_vgpr != other.external_unwritten_vgpr) return false;
			if (external_library == other.external_library) return true;
			if (!external_library || !other.external_library) return false;
			// The bucket hash is not an identity: all addresses and bytes must agree.
			return external_library->caller_address == other.external_library->caller_address &&
			       external_library->dependency_hash == other.external_library->dependency_hash &&
			       external_library->dependencies == other.external_library->dependencies;
		}
	};

	struct Permutation {
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
	};

	struct SourceEntry {
		explicit SourceEntry(ShaderRecompiler::IR::ResourcePlan plan)
		    : resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		std::vector<Permutation>                    permutations;
	};

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			PipelineKeyHash::Mix(hash, key.user_data_count);
			PipelineKeyHash::Mix(hash, key.code_size);
			PipelineKeyHash::Mix(hash, key.external_call_probe);
			PipelineKeyHash::Mix(hash, key.external_probe_before_bvh);
			PipelineKeyHash::Mix(hash, key.external_probe_structured);
			PipelineKeyHash::Mix(hash,key.external_probe_after_bvh);
			PipelineKeyHash::Mix(hash, key.external_unwritten_vgpr);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			if (key.external_library) {
				PipelineKeyHash::Mix(hash, key.external_library->caller_address);
				PipelineKeyHash::Mix(hash, key.external_library->dependency_hash);
			}
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing the full state first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 32 + ShaderVertexInputInfo::RES_MAX * 6;

	struct LibrarySnapshot {
		uint64_t caller_hash;
		std::shared_ptr<const ShaderRecompiler::ExternalLibraryPlan> plan;
	};

	std::shared_ptr<const ShaderRecompiler::ExternalLibraryPlan> LoadLibrary(
	    const ShaderParams& params, std::span<const uint32_t> user_data) {
		// Validate exact guarded dependencies even on a warm hit: library mappings may relocate
		// or change independently of the unchanged caller shader hash.
		for (auto& snapshot: libraries) {
			if (snapshot.caller_hash != params.hash || snapshot.plan->caller_address != params.Base() ||
			    !ShaderRecompiler::ExternalLibraryInputsMatch(*snapshot.plan, user_data)) continue;
			if (ShaderRecompiler::ValidateExternalLibraryDependencies(
			        *snapshot.plan, ReadShaderGuestMemory, nullptr)) return snapshot.plan;
		}
		ShaderRecompiler::Decoder::Program decoded;
		ShaderRecompiler::Decoder::DecodeProgram(params.code, decoded);
		auto loaded = ShaderRecompiler::LoadExternalLibrary(
		    decoded, params.Base(), user_data, 0, ReadShaderGuestMemory, nullptr);
		if (!loaded.has_calls) return {};
		if (!loaded.plan.complete) {
			EXIT("External shader library rejected hash=0x%016" PRIx64 ": %s\n",
			     params.hash, loaded.failure.c_str());
		}
		auto plan = std::make_shared<const ShaderRecompiler::ExternalLibraryPlan>(std::move(loaded.plan));
		libraries.push_back({params.hash, plan});
		std::printf("External shader library loaded hash=0x%016" PRIx64
		     " functions=%zu call_sites=%zu dependency_hash=0x%016" PRIx64 "\n",
		     params.hash, plan->functions.size(), plan->call_sites.size(), plan->dependency_hash);
		std::fflush(stdout);
		return plan;
	}

	Permutation CompilePermutation(const char*                                  stage_name,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword) {
		const auto external_begin = std::chrono::steady_clock::now();
		if (options.external_library != nullptr) {
			std::printf("External shader phase begin hash=0x%016" PRIx64 " compile-and-emit\n",
			            options.shader_hash);
			std::fflush(stdout);
		}
		ShaderRecompiler::IR::PixelSampleSensitivity pixel_sample_sensitivity;
		if (options.stage == ShaderType::Pixel &&
		    ShaderRecompiler::Diagnostics::CollectEqaaPixelFacts()) {
			pixel_sample_sensitivity = ShaderRecompiler::Diagnostics::CollectPixelSampleSensitivity(
			    options.stage, true, translated.program.info, translated.program.memory_info,
			    specialization);
			for (const auto* block : translated.program.blocks)
				for (const auto& inst : *block) {
					ShaderRecompiler::Diagnostics::ObservePixelSampleOpcode(
					    pixel_sample_sensitivity, inst.GetOpcode());
					if (inst.GetOpcode() == ShaderRecompiler::IR::ValueOpcode::GetBuiltin) {
						const auto& kind = inst.Arg(0);
						const bool known = kind.IsImmediate() && kind.GetType() == ShaderRecompiler::IR::Type::U32;
						ShaderRecompiler::Diagnostics::ObservePixelSampleBuiltin(
						    pixel_sample_sensitivity, known, known ? kind.U32() : UINT32_MAX);
					} else if (inst.GetOpcode() == ShaderRecompiler::IR::ValueOpcode::SetAttribute) {
						const auto index = inst.Flags<ShaderRecompiler::IR::ExportFlags>().index;
						ShaderRecompiler::Diagnostics::ObservePixelSampleExport(
						    pixel_sample_sensitivity, index < translated.program.export_info.size()
						                                  ? &translated.program.export_info[index] : nullptr);
					}
				}
		}
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		if (options.external_library != nullptr) {
			std::printf("External shader phase end hash=0x%016" PRIx64
			            " compile-and-emit words=%zu id_bound=%u elapsed_ms=%" PRIu64 "\n",
			            options.shader_hash, result.spirv.size(),
			            result.spirv.size() > 3u ? result.spirv[3] : 0u,
			            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			                std::chrono::steady_clock::now() - external_begin).count()));
			std::fflush(stdout);
		}
		const ShaderRecompiler::IR::DescriptorBindingLimits binding_limits {
		    .sampled_images = limits.maxPerStageDescriptorSampledImages,
		    .storage_images = limits.maxPerStageDescriptorStorageImages,
		    .samplers = limits.maxPerStageDescriptorSamplers,
		    .storage_buffers = limits.maxPerStageDescriptorStorageBuffers,
		    .total_resources = limits.maxPerStageResources,
		};
		std::string binding_failure;
		if (!ShaderRecompiler::IR::ValidateDescriptorBindingLimits(
		        result.program, binding_limits, binding_failure)) {
			EXIT("%s device descriptor limits exceeded hash=0x%016" PRIx64 ": %s\n",
			     options.dump_label, options.shader_hash, binding_failure.c_str());
		}
		if (options.external_library != nullptr) {
			std::printf("External shader phase begin hash=0x%016" PRIx64 " validate-module\n",
			            options.shader_hash);
			std::fflush(stdout);
		}
		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv,
		                         options.external_library != nullptr)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		if (options.external_library != nullptr) {
			std::printf("External shader phase end hash=0x%016" PRIx64 " validate-module\n",
			            options.shader_hash);
			std::fflush(stdout);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);

		if (options.external_library != nullptr) {
			std::printf("External shader phase begin hash=0x%016" PRIx64 " driver-module\n",
			            options.shader_hash);
			std::fflush(stdout);
		}
		const auto module = CompileSPV(result.spirv, device);
		EXIT_IF(module == nullptr);
		if (options.external_library != nullptr) {
			std::printf("External shader phase end hash=0x%016" PRIx64 " driver-module\n",
			            options.shader_hash);
			std::fflush(stdout);
		}
		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		auto compiled_info = std::move(result.program).TakeCompiledInfo();
		compiled_info.pixel_sample_sensitivity = pixel_sample_sensitivity;
		return {
		    .specialization = std::move(specialization),
		    .program        = std::move(compiled_info),
		    .handle         = {.id = ++next_shader_id, .module = module},
		};
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.logical_stage;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		const bool requested_bvh_probe = ExternalBvhProbeRequested();
		const bool requested_structured_probe = ExternalStructuredProbeRequested();
		const bool requested_after_bvh_probe = ExternalAfterBvhProbeRequested();
		const bool requested_resource_capture = ExternalResourceReadCaptureRequested();
		const bool requested_selected_capture = SelectedCalleeResourceCaptureRequested();
		const auto requested_checked_vgpr = ExternalUnwrittenVgprRequested();
		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = params.user_data_count;
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		lookup_key.external_library.reset();
		lookup_key.external_call_probe = false;
		lookup_key.external_probe_before_bvh = false;
		lookup_key.external_probe_structured = false;
		lookup_key.external_probe_after_bvh = false;
		lookup_key.external_unwritten_vgpr = std::numeric_limits<uint32_t>::max();
		if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			const bool could_call = std::ranges::any_of(params.code, [](uint32_t word) {
				return ((word >> 23u) & 0x1ffu) == 0x17du && ((word >> 8u) & 0xffu) == 0x21u &&
				       ((word >> 16u) & 0x7fu) != 125u;
			});
			const auto* capture_only = std::getenv("KYTY_CAPTURE_EXTERNAL_INPUTS_ONLY");
			if (could_call && capture_only != nullptr && std::strcmp(capture_only, "1") == 0) {
				ShaderRecompiler::Decoder::Program capture_decoded;
				ShaderRecompiler::Decoder::DecodeProgram(params.code, capture_decoded);
				const bool actual_call = std::ranges::any_of(capture_decoded.instructions,
				    [](const auto& inst) {
					    return inst.opcode == ShaderRecompiler::Decoder::Opcode::S_SWAPPC_B64;
				    });
				if (actual_call) {
					// Diagnostic-only: collect actual dispatch inputs before strict library planning
					// or warm shader lookup can return. No external shader is translated or executed.
					ShaderStageInputInfo capture_input {};
					capture_input.compute = &input_info;
					ShaderRecompiler::CompileOptions capture_options;
					capture_options.stage = stage;
					capture_options.shader_hash = params.hash;
					capture_options.user_data = user_data;
					capture_options.wave_size = input_info.wave_size;
					capture_options.input_info = capture_input;
					const bool inputs_captured = DumpShaderCallInputs("cs", capture_options, params.code);
					EXIT("External shader input capture-only mode: files_written=%d; stopped before "
					     "external translation or GPU execution\n", inputs_captured);
				}
			}
			if (could_call) lookup_key.external_library = LoadLibrary(params, user_data);
			lookup_key.external_call_probe = lookup_key.external_library && ExternalProbeRequested();
			lookup_key.external_probe_before_bvh = lookup_key.external_call_probe && requested_bvh_probe;
			lookup_key.external_probe_structured = lookup_key.external_call_probe && requested_structured_probe;
			lookup_key.external_probe_after_bvh = lookup_key.external_call_probe && requested_after_bvh_probe;
			if (lookup_key.external_library) {
				lookup_key.external_unwritten_vgpr = requested_checked_vgpr;
			}
		}
        // Diagnostic CPU-only branch precedes warm program lookup. It never
        // reuses/stores the terminal probe resource plan or a shader permutation.
        if(requested_selected_capture && params.hash==0x3351560625c27256ull) {
            if constexpr (!std::is_same_v<InputInfo,ShaderComputeInputInfo>) {
                EXIT("Selected-callee resource capture requires compute stage\n");
            } else {
                const auto reference=ReadSelectedCalleeReference();
                if(!lookup_key.external_library)EXIT("Selected-callee capture has no complete held library\n");
                const auto& held=*lookup_key.external_library;
                ShaderRecompiler::Decoder::Program decoded;
                ShaderRecompiler::Decoder::DecodeProgram(params.code,decoded);
                auto selected=ShaderRecompiler::Diagnostics::PrepareSelectedCalleeCapture(
                    params.hash,decoded,held,reference);
                if(!selected.success)EXIT("Selected-callee candidate rejected: %s\n",selected.failure.c_str());
                if(!ShaderRecompiler::ValidateExternalLibraryDependencies(held,ReadShaderGuestMemory))
                    EXIT("Selected-callee held dependency validation failed before materialization\n");
                ShaderStageInputInfo selected_input{};selected_input.compute=&input_info;
                ShaderRecompiler::CompileOptions selected_options;
                selected_options.stage=ShaderType::Compute;selected_options.shader_hash=params.hash;
                selected_options.wave_size=input_info.wave_size;selected_options.user_data=user_data;
                selected_options.input_info=selected_input;selected_options.external_library=&selected.overlay;
                selected_options.dump_ir=false;
                // Every probe/checked option remains false/default. The resulting
                // normal callee is translated/materialized on CPU only, never emitted.
                std::filesystem::path input_folder;
                const bool input_written=DumpShaderCallInputs("cs",selected_options,params.code,&input_folder,true);
                if(!input_written)EXIT("Selected-callee input artifact write failed; no materialization\n");
                const auto topology=ResourceCaptureLibraryTopology(selected.overlay);
                const bool overlay_written=WriteCallCaptureFile(input_folder/"selected-overlay-topology-u64le.bin",topology.data(),topology.size()*sizeof(uint64_t));
                const bool reference_written=WriteCallCaptureFile(input_folder/"selected-native-reference.u32le.bin",reference.serialized_words.data(),reference.serialized_words.size()*sizeof(uint32_t));
                std::string candidate=fmt::format(
                    "schema=2 scope=authored_body_candidate_CPU_materialization_only selected_this_run=false\n"
                    "selection_basis=historical_selected_body_exact_reachable_native_reference current_records_are_authored_not_GPU_elected\n"
                    "caller_hash=0x{:016x} caller_base=0x{:016x} call_pc=0x{:08x} domain={} historical_ordinal={} current_function={}\n"
                    "target=0x{:016x} relocated_addresses_allowed=true full_domain_support_proven=false\n"
                    "current_context_policy=all_held_records_for_unique_exact_body max_matching_records={}\n"
                    "selected_compile_external_probe=false selected_compile_before_bvh=false selected_compile_structured=false selected_compile_after_bvh=false selected_compile_checked_vgpr=4294967295 parent_target_probe=true parent_structured_probe={}\n"
                    "reference_sha256=F6917CF654540D698C8DDC86D4114DF6D4720C6244E984E1A5AA829B479A1154 reference_xxh128_high=fade6f1f85f00811 reference_xxh128_low=b76ee41325b5dd93\n"
                    "reference_file=selected-native-reference.u32le.bin reference_native_instructions={} reference_extent_bytes={} overlay_functions=1 overlay_records={}\n"
                    "full_held_plan_artifacts=resource_reads/library-topology-u64le.bin+exact_dependency_files\n"
                    "legacy_input_dump_epoch=separate_reread_not_authoritative_held_plan\n",
                    params.hash,params.Base(),reference.caller_pc,reference.domain,reference.ordinal,selected.function_id,
                    selected.target,ShaderRecompiler::Diagnostics::MaxSelectedBodyContextRecords,lookup_key.external_probe_structured,
                    reference.instructions.size(),reference.extent_bytes,selected.overlay.call_sites.front().records.size());
                for(const auto& record:selected.overlay.call_sites.front().records){
                    candidate+=fmt::format("current_record[{}]: function={} target=0x{:016x} auxiliary=0x{:016x}\n",
                        record.ordinal,record.function_id,record.function_address,record.auxiliary_address);
                }
                const bool selection_written=WriteCallCaptureFile(input_folder/"selected-candidate.txt",candidate.data(),candidate.size());
                if(!overlay_written || !selection_written || !reference_written)EXIT("Selected-callee overlay/reference artifact write failed; no materialization\n");
                const auto domains=ShaderRecompiler::ExternalContextDomains(selected.overlay);
                ShaderRecompiler::IR::SrtRuntime selected_runtime{
                    .user_data=user_data,.shader_base=params.Base(),.read_specialization_memory=ReadShaderGuestMemory,
                    .workgroup_counts=input_info.workgroup_counts,.external_context_domains=domains,
                    .descriptor_limits={.sampled_images=limits.maxPerStageDescriptorSampledImages,
                        .storage_images=limits.maxPerStageDescriptorStorageImages,.samplers=limits.maxPerStageDescriptorSamplers,
                        .storage_buffers=limits.maxPerStageDescriptorStorageBuffers,.total_resources=limits.maxPerStageResources}};
                ShaderRecompiler::IR::SrtReadCaptureIdentity identity;
                identity.stage=ShaderType::Compute;identity.shader_hash=params.hash;identity.shader_base=params.Base();
                static std::atomic_uint64_t next_selected_pass{1};identity.pass_id=next_selected_pass++;
                identity.user_data.assign(user_data.begin(),user_data.end());
                identity.control_schema="compute_v1:wave,host_subgroup,threads_xyz,dispatch_threads_xyz,workgroup_counts_xyz,group_id_xyz,thread_ids,workgroup_register,tg_size_en,dispatch_dimensions,lds_storage,lds_dwords,scratch_dwords,float_mode,external_probe,before_bvh,checked_vgpr,descriptor_limits_5";
                identity.control_words={input_info.wave_size,input_info.host_subgroup_size,
                    input_info.threads_num[0],input_info.threads_num[1],input_info.threads_num[2],
                    input_info.dispatch_threads_num[0],input_info.dispatch_threads_num[1],input_info.dispatch_threads_num[2],
                    input_info.workgroup_counts[0],input_info.workgroup_counts[1],input_info.workgroup_counts[2],
                    input_info.group_id[0],input_info.group_id[1],input_info.group_id[2],
                    static_cast<uint32_t>(input_info.thread_ids_num),static_cast<uint32_t>(input_info.workgroup_register),input_info.tg_size_en,
                    input_info.dispatch_thread_dimensions,static_cast<uint32_t>(input_info.lds_storage),input_info.lds_size_dwords,
                    input_info.scratch_size_dwords,input_info.float_mode,0u,0u,UINT32_MAX,
                    selected_runtime.descriptor_limits.sampled_images,selected_runtime.descriptor_limits.storage_images,
                    selected_runtime.descriptor_limits.samplers,selected_runtime.descriptor_limits.storage_buffers,selected_runtime.descriptor_limits.total_resources};
                identity.caller_identity="actual-caller.bin";
                identity.library_identity="library-topology-u64le.bin+exact_dependency_files";
                identity.input_identity=Common::PathToString(input_folder)+"/selected-candidate.txt";
                ShaderRecompiler::IR::SrtReadCapture capture(std::move(identity));
                selected_runtime.post_read_observer=ShaderRecompiler::IR::SrtReadCapture::Observe;
                selected_runtime.post_read_userdata=&capture;
                std::printf("Selected-callee CPU body capture begin hash=0x%016" PRIx64 " historical_ordinal=%u target=0x%016" PRIx64 " current_contexts=%zu; NOT selected by this run\n",params.hash,reference.ordinal,selected.target,selected.overlay.call_sites.front().records.size());std::fflush(stdout);
                auto translated=ShaderRecompiler::TranslateProgram(params.code,selected_options);
                const auto plan=ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
                ShaderRecompiler::IR::ResourceSnapshot resources;
                ShaderRecompiler::IR::ResourceSpecialization specialization;
                const bool materialized=ShaderRecompiler::IR::MaterializeResources(plan,selected_runtime,resources,specialization);
                const bool dependencies_stable=ShaderRecompiler::ValidateExternalLibraryDependencies(held,ReadShaderGuestMemory);
                capture.Finalize(materialized && dependencies_stable);
                const bool saved=DumpResourceReadCapture(input_folder,capture,held,params.code,
                    input_written && overlay_written && selection_written && reference_written && dependencies_stable,materialized && dependencies_stable);
                EXIT("Selected-callee CPU resource diagnostic stop: materialized=%d dependencies_stable=%d capture_complete=%d files_written=%d; candidate only, no shader emission, driver module, callee or caller continuation execution\n",materialized,dependencies_stable,capture.Complete(),saved);
            }
        }
		auto                                         entry = programs.find(lookup_key);
		std::vector<ShaderRecompiler::IR::ExternalCallContextDomain> context_domains;
		if (lookup_key.external_library) {
			context_domains = ShaderRecompiler::ExternalContextDomains(*lookup_key.external_library);
		}
		ShaderRecompiler::IR::SrtRuntime             runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_specialization_memory = ReadShaderGuestMemory,
		    .external_context_domains    = context_domains,
		    .descriptor_limits           = {
		        .sampled_images = limits.maxPerStageDescriptorSampledImages,
		        .storage_images = limits.maxPerStageDescriptorStorageImages,
		        .samplers = limits.maxPerStageDescriptorSamplers,
		        .storage_buffers = limits.maxPerStageDescriptorStorageBuffers,
		        .total_resources = limits.maxPerStageResources,
		    },
		};
		std::optional<ShaderRecompiler::IR::SrtReadCapture> resource_capture;
		std::filesystem::path resource_input_folder;
		bool resource_input_files_written = false;
		if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			runtime.workgroup_counts = input_info.workgroup_counts;
			if (requested_resource_capture && lookup_key.external_library) {
				// All observed reads retain their original callbacks and synchronization.
				// This collector is invocation-local and is never retained by a cached plan.
				ShaderStageInputInfo capture_input {};
				capture_input.compute = &input_info;
				ShaderRecompiler::CompileOptions capture_options;
				capture_options.stage = stage;
				capture_options.shader_hash = params.hash;
				capture_options.user_data = user_data;
				capture_options.wave_size = input_info.wave_size;
				capture_options.input_info = capture_input;
				capture_options.external_library = lookup_key.external_library.get();
				resource_input_files_written = DumpShaderCallInputs("cs", capture_options,
				    params.code, &resource_input_folder);
				static std::atomic_uint64_t next_capture_pass {1};
				ShaderRecompiler::IR::SrtReadCaptureIdentity identity;
				identity.stage = stage;
				identity.shader_hash = params.hash;
				identity.shader_base = params.Base();
				identity.pass_id = next_capture_pass++;
				identity.user_data.assign(user_data.begin(), user_data.end());
				identity.control_schema = "compute_v1:wave,host_subgroup,threads_xyz,dispatch_threads_xyz,"
				    "workgroup_counts_xyz,group_id_xyz,thread_ids,workgroup_register,tg_size_en,"
				    "dispatch_dimensions,lds_storage,lds_dwords,scratch_dwords,float_mode,"
				    "external_probe,before_bvh,checked_vgpr,descriptor_limits_5";
				identity.control_words = {input_info.wave_size, input_info.host_subgroup_size,
				    input_info.threads_num[0], input_info.threads_num[1], input_info.threads_num[2],
				    input_info.dispatch_threads_num[0], input_info.dispatch_threads_num[1], input_info.dispatch_threads_num[2],
				    input_info.workgroup_counts[0], input_info.workgroup_counts[1], input_info.workgroup_counts[2],
				    input_info.group_id[0], input_info.group_id[1], input_info.group_id[2],
				    static_cast<uint32_t>(input_info.thread_ids_num),
				    static_cast<uint32_t>(input_info.workgroup_register), input_info.tg_size_en,
				    input_info.dispatch_thread_dimensions, static_cast<uint32_t>(input_info.lds_storage),
				    input_info.lds_size_dwords, input_info.scratch_size_dwords, input_info.float_mode,
				    lookup_key.external_call_probe, lookup_key.external_probe_before_bvh,
				    lookup_key.external_unwritten_vgpr, runtime.descriptor_limits.sampled_images,
				    runtime.descriptor_limits.storage_images, runtime.descriptor_limits.samplers,
				    runtime.descriptor_limits.storage_buffers, runtime.descriptor_limits.total_resources};
				identity.caller_identity = "actual-caller.bin";
				identity.library_identity = "library-topology-u64le.bin+exact_dependency_files";
				identity.input_identity = Common::PathToString(resource_input_folder);
				resource_capture.emplace(std::move(identity));
				runtime.post_read_observer = ShaderRecompiler::IR::SrtReadCapture::Observe;
				runtime.post_read_userdata = &*resource_capture;
				PipelineCacheLog("External resource-read capture begin hash=0x{:016x}; stops after "
				    "materialization before shader compilation", params.hash);
			}
		}
		const auto materialize = [&](const auto& plan, auto& resources, auto& specialization) {
			const bool succeeded = ShaderRecompiler::IR::MaterializeResources(plan, runtime,
			    resources, specialization);
			if (resource_capture) {
				resource_capture->Finalize(succeeded);
				const bool saved = DumpResourceReadCapture(resource_input_folder, *resource_capture,
				    *lookup_key.external_library, params.code, resource_input_files_written, succeeded);
				EXIT("External resource-read diagnostic stop: materialized=%d capture_complete=%d "
				     "files_written=%d; stopped before external shader compilation or GPU execution\n",
				     succeeded, resource_capture->Complete(), saved);
			}
			return succeeded;
		};
		if (entry != programs.end()) {
			{
				MenuPerformanceDiagnostic::TimedScope diagnostic_scope(
				    MenuPerformanceDiagnostic::TimedOperation::WarmMaterialize);
				EXIT_IF(!materialize(entry->second.resource_plan, entry->second.resources,
				                     entry->second.specialization));
			}
			if (const auto permutation = std::ranges::find_if(
			        entry->second.permutations, [&](const Permutation& candidate) {
				        const auto& layout = candidate.program.bindings;
				        return layout.push_data_start_dword ==
				                   ShaderRecompiler::IR::PushData::StartFor(
				                       push_data_cursor, layout.ShaderDataDwords()) &&
				               candidate.specialization == entry->second.specialization;
			        });
			    permutation != entry->second.permutations.end()) {
				input_info.stage = {.program   = &permutation->program,
				                    .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}

		MenuPerformanceDiagnostic::TimedScope diagnostic_compile_scope(
		    MenuPerformanceDiagnostic::TimedOperation::Compile);
		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		const char* label = nullptr;
		const char* stage_name = nullptr;
		switch (stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; stage_name = "vs"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; stage_name = "ms"; break;
			case ShaderType::Local: label = "ShaderRecompiler LS"; stage_name = "ls"; break;
			case ShaderType::TessellationControl: label = "ShaderRecompiler HS"; stage_name = "hs"; break;
			case ShaderType::TessellationEvaluation: label = "ShaderRecompiler DS"; stage_name = "ds"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; stage_name = "ps"; break;
			case ShaderType::Compute: label = "ShaderRecompiler CS"; stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;
		options.external_library = lookup_key.external_library.get();
		options.external_call_probe = lookup_key.external_call_probe;
		options.external_probe_before_bvh = lookup_key.external_probe_before_bvh;
		options.external_probe_structured = lookup_key.external_probe_structured;
		options.external_probe_after_bvh = lookup_key.external_probe_after_bvh;
		options.external_unwritten_vgpr = lookup_key.external_unwritten_vgpr;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			options.wave_size = input_info.wave_size;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else {
			options.wave_size = input_info.wave_size;
		}
		DumpShaderOriginal(stage_name, options.shader_hash, params.code);
		if (!resource_capture) DumpShaderCallInputs(stage_name, options, params.code);
		if (options.external_library != nullptr) {
			std::printf("External shader phase begin hash=0x%016" PRIx64 " translate\n",
			            options.shader_hash);
			std::fflush(stdout);
		}
		auto translated = ShaderRecompiler::TranslateProgram(params.code, options);
		if (options.external_library != nullptr) {
			std::printf("External shader phase end hash=0x%016" PRIx64 " translate blocks=%zu\n",
			            options.shader_hash, translated.program.blocks.size());
			std::fflush(stdout);
		}
		if (entry == programs.end()) {
			if (options.external_library != nullptr) {
				std::printf("External shader phase begin hash=0x%016" PRIx64 " material-resources\n",
				            options.shader_hash);
				std::fflush(stdout);
			}
			entry = programs.try_emplace(lookup_key,
			    ShaderRecompiler::IR::ExtractResourcePlan(translated.program)).first;
			EXIT_IF(!materialize(entry->second.resource_plan, entry->second.resources,
			                     entry->second.specialization));
			if (options.external_library != nullptr) {
				const auto& resources = entry->second.resources;
				std::printf("External shader phase end hash=0x%016" PRIx64
				            " material-resources buffers=%zu images=%zu samplers=%zu candidates=%" PRIu64
				            " read_bytes=%" PRIu64 "\n", options.shader_hash,
				            resources.buffers.size(), resources.images.size(), resources.samplers.size(),
				            resources.external_descriptor_candidate_count,
				            resources.external_descriptor_read_bytes);
				std::fflush(stdout);
			}
		}
		entry->second.permutations.push_back(CompilePermutation(
		    stage_name, options, std::move(translated), entry->second.specialization, push_data_cursor));
		MenuPerformanceDiagnostic::CompiledPermutation();
		const auto& permutation = entry->second.permutations.back();
		input_info.stage = {.program = &permutation.program, .resources = &entry->second.resources};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::array<size_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)],
		            counts[static_cast<size_t>(ShaderType::Local)],
		            counts[static_cast<size_t>(ShaderType::TessellationControl)],
		            counts[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
		return permutation.handle;
	}

	explicit ProgramCache(vk::Device device, const vk::PhysicalDeviceLimits& limits)
	    : device(device), limits(limits) {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	std::vector<LibrarySnapshot>                                libraries;
	ProgramKey                                                  lookup_key;
	vk::Device                                                  device;
	vk::PhysicalDeviceLimits                                    limits;
	uint64_t                                                    next_shader_id = 0;
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_program_cache(std::make_unique<ProgramCache>(
          graphics.device, graphics.physical_device_properties.limits)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
}

PipelineCache::~PipelineCache() {
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	const auto checked_vgpr = ExternalUnwrittenVgprRequested();
	const bool requested_bvh_probe = ExternalBvhProbeRequested();
	const bool requested_structured_probe = ExternalStructuredProbeRequested();
		const bool requested_after_bvh_probe = ExternalAfterBvhProbeRequested();
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (non-Release build)");
		return;
	}
	const std::string_view git_hash     = KYTY_GIT_HASH;
	const std::string_view git_revision = KYTY_GIT_REVISION;
	if (git_hash == "unknown" || git_revision == "unknown") {
		PipelineCacheLog("Vulkan pipeline cache: disabled (unknown git revision)");
		return;
	}
	if (git_hash.ends_with("-dirty")) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (dirty build)");
		return;
	}

	auto cache_suffix = requested_after_bvh_probe ?
	    (requested_structured_probe ? std::string("-external-probe-structured-after-bvh.bin") :
	                                  std::string("-external-probe-after-bvh.bin")) :
	    requested_structured_probe ?
	    (requested_bvh_probe ? std::string("-external-probe-structured-bvh.bin") :
	                           std::string("-external-probe-structured.bin")) :
	    requested_bvh_probe ? std::string("-external-probe-bvh.bin") :
	    ExternalProbeRequested() ? std::string("-external-probe.bin") :
	    checked_vgpr != std::numeric_limits<uint32_t>::max() ?
	        fmt::format("-external-vgpr{}.bin", checked_vgpr) : std::string(".bin");
	if (ShaderRecompiler::Diagnostics::EqaaReduced2xRequested()) {
		cache_suffix.resize(cache_suffix.size() - std::string_view(".bin").size());
		cache_suffix += "-experimental-eqaa2x-v3-depth-absence.bin";
	}
	m_driver_cache_path = std::filesystem::path("_PipelineCache") / (title_id + cache_suffix);
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto   signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
		if (file_size >= signature.size() + sizeof(uint64_t) &&
		    file_size <= std::numeric_limits<uint32_t>::max()) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()),
			          &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, emulator, or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)", path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

void PipelineCache::Save() {
	PersistDriverCache(true);
}

void PipelineCache::Checkpoint() {
	PersistDriverCache(false);
}

void PipelineCache::PersistDriverCache(bool retire) {
	if (m_driver_cache == nullptr) {
		return;
	}

	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 vk::to_string(result), size);
		return;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::AtomicReplaceFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	if (retire) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
		m_driver_cache = nullptr;
	}
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
	} else {
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxMeshWorkGroupInvocations ||
		    host_threads > limits.maxMeshWorkGroupSize[0] ||
		    mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
			     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		const auto& blend = context.GetBlendControl(0);
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
		} else if (blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		           pixel_info.target_output_mode[0] != 0 && pixel_info.target_output_mode[0] != 7 &&
		           std::all_of(std::begin(pixel_info.target_output_mode) + 1,
		                       std::end(pixel_info.target_output_mode),
		                       [](uint8_t mode) { return mode == 0; })) {
			switch (ClassifyBlendMapping(blend, pixel_info.target_export_mapping[0])) {
				case BlendMappingSupport::SourceAlpha:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlpha;
					break;
				case BlendMappingSupport::SourceAlphaOne:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaOne;
					break;
				case BlendMappingSupport::SourceAlphaZero:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaZero;
					break;
				case BlendMappingSupport::SourceOneAlphaZero:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceOneAlphaZero;
					break;
				default: break;
			}
			if (pixel_info.alpha_blend_source != ShaderAlphaBlendSource::None) {
				pixel_info.dual_source_blending     = true;
				pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
				pixel_info.target_export_mapping[1] = {};
			}
		}
	}
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = vertex_info[tess_active ? 2u : 0u].clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	}
	uint32_t          push_data_cursor =
	    mesh_active ? ShaderRecompiler::IR::PushData::MeshDrawDwordCount : 0;
	GraphicsPrograms  result;
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] = m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor);
	}
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	const auto* checkpoint = std::getenv("KYTY_EXTERNAL_DIAGNOSTIC_CHECKPOINT");
	if (!m_external_checkpointed && checkpoint != nullptr && std::strcmp(checkpoint, "1") == 0 &&
	    HasExternalCall(params.code)) {
		m_external_checkpointed = true;
		Checkpoint();
	}
	input_info.lds_storage = input_info.lds_size_dwords * 4u >
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	uint32_t          push_data_cursor = 0;
	return m_program_cache->Get(params, input_info, push_data_cursor);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	auto&             ctx = command.GetRegisters();

	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	GraphicsPipelineKey key {};
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		key.vertex_shader_ids[i] = programs.vertex[i].id;
	}
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = 0;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const auto slot = colors[i].target_slot;
		if (colors[i].experimental_reduced_eqaa_2x) key.experimental_eqaa_policy = 2;
		EXIT_IF(slot >= RENDER_COLOR_ATTACHMENTS_MAX);
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[slot] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		rendering.color_formats[slot] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		auto alpha_source = ShaderAlphaBlendSource::None;
		if (slot == 0 && ps_input_info != nullptr) {
			alpha_source = ps_input_info->alpha_blend_source;
		}
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && alpha_source == ShaderAlphaBlendSource::None &&
		    ClassifyBlendMapping(bc, colors[i].export_mapping) != BlendMappingSupport::Direct) {
			static_params.blend_enable[slot] = false;
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: blending disabled for unsupported color mapping "
				    "(slot={} mapping=0x{:02x} color={}/{} alpha={}/{} separate={}).\n",
				    slot, colors[i].export_mapping.packed, bc.color_srcblend, bc.color_destblend,
				    bc.alpha_srcblend, bc.alpha_destblend, bc.separate_alpha_blend ? 1 : 0));
			}
		}
		if (static_params.blend_enable[slot]) {
			auto blend = bc;
			switch (alpha_source) {
				case ShaderAlphaBlendSource::SourceAlpha:
					blend.color_srcblend  = RemapSourceAlphaFactor(blend.color_srcblend);
					blend.color_destblend = RemapSourceAlphaFactor(blend.color_destblend);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::SourceAlphaOne:
				case ShaderAlphaBlendSource::SourceAlphaZero:
					// The second source carries the mapped source factor; its alpha stays logical Sa.
					blend.color_srcblend = static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Color);
					blend.color_destblend =
					    static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::SourceOneAlphaZero:
					// The pixel export preserves logical Sa in source1 and zeros only
					// the primary logical alpha, retaining the separate native equation.
					blend.color_srcblend = static_cast<uint8_t>(Prospero::BlendFactor::kOne);
					blend.color_destblend = static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Alpha);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::None: break;
			}
			static_params.color_srcblend[slot]       = blend.color_srcblend;
			static_params.color_comb_fcn[slot]       = blend.color_comb_fcn;
			static_params.color_destblend[slot]      = blend.color_destblend;
			static_params.separate_alpha_blend[slot] = blend.separate_alpha_blend;
			if (blend.separate_alpha_blend) {
				static_params.alpha_srcblend[slot]  = blend.alpha_srcblend;
				static_params.alpha_comb_fcn[slot]  = blend.alpha_comb_fcn;
				static_params.alpha_destblend[slot] = blend.alpha_destblend;
			}
		}
	}
	const bool with_depth =
	    depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (key.experimental_eqaa_policy != 0) {
		EXIT_IF(!ShaderRecompiler::Diagnostics::EqaaReduced2xRequested() ||
		        attachment_samples != 2 ||
		        (with_depth ? depth.desc.info.samples != 2
		                    : !ShaderRecompiler::Diagnostics::IsReducedEqaaDepthAbsent(ctx)) ||
		        !ps_active || ps_input_info == nullptr || ps_input_info->ps_sample_shading);
	}

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	const bool rect_list = Prospero::IsRectList(command.GetUserConfig().GetPrimType());
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);

	if (vs_input_info.stage.program->stage != ShaderType::Mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
		}
		for (int attribute = 0; attribute < vs_input_info.resources_num; attribute++) {
			const auto binding = vs_input_info.resources_dst[attribute].buffer_index;
			EXIT_IF(binding < 0 || binding >= vs_input_info.buffers_num);
			key.vertex_input.attributes[attribute] = {
			    .offset = static_cast<uint32_t>(vs_input_info.resources[attribute].Base48() -
			                                    vs_input_info.buffers[binding].addr),
			    .binding = static_cast<uint8_t>(binding),
			};
		}
	}

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		MenuPerformanceDiagnostic::GraphicsPipelineLookup(true);
		return *iter->second;
	}
	MenuPerformanceDiagnostic::GraphicsPipelineLookup(false);

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	auto cached = std::make_unique<Pipeline>();
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	{
		MenuPerformanceDiagnostic::TimedScope diagnostic_scope(
		    MenuPerformanceDiagnostic::TimedOperation::GraphicsPipelineCreate);
		CreatePipelineInternal(m_graphics, *cached, rendering, key.vertex_input, vertex_info,
		                       ps_input_info, programs, static_params, m_driver_cache);
	}
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	return *iter->second;
}

PipelineCache::Pipeline&
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                  const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	if (auto iter = m_compute_pipelines.find(compute_program.id);
	    iter != m_compute_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<Pipeline>();
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module, m_driver_cache);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(compute_program.id, std::move(cached));
	EXIT_IF(!inserted);
	if (input_info.stage.program->info.uses_external_call_probe ||
	    input_info.stage.program->info.uses_checked_external_calls) {
		// Capture the newly compiled diagnostic as well as the earlier startup pipelines.
		// The live cache remains valid for future pipeline creation and normal shutdown.
		Checkpoint();
	}

	return *iter->second;
}
} // namespace Libs::Graphics
