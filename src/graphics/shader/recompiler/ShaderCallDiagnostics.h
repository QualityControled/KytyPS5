#pragma once

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Diagnostics {

inline constexpr size_t MaxCallSites   = 64;
inline constexpr size_t MaxTableBytes  = 64 * 1024;
inline constexpr size_t MaxTargets     = 64;
inline constexpr size_t MaxTargetBytes = 64 * 1024;
inline constexpr size_t ReadChunkBytes = 4096;
using MemoryReader                     = bool (*)(void*, uint64_t, std::span<uint32_t>);

struct CallTableTrace {
	uint32_t                call_pc              = 0;
	uint32_t                raw_call             = 0;
	uint32_t                target_sgpr          = 0;
	uint32_t                return_sgpr          = 0;
	uint32_t                record_load_pc       = 0;
	uint32_t                descriptor_load_pc   = 0;
	uint32_t                descriptor_user_sgpr = 0;
	int32_t                 descriptor_offset    = 0;
	int32_t                 record_offset        = 0;
	std::array<uint32_t, 2> user_words {};
	uint64_t                descriptor_address = 0;
	// Empty means the origin was proved; a nonempty reason prohibits memory reads.
	std::string rejection;
};

struct CallTableTraceResult {
	std::vector<CallTableTrace> calls;
	bool                        call_sites_truncated = false;
};

struct CallTableSnapshot {
	CallTableTrace          trace;
	std::array<uint32_t, 4> descriptor {};
	bool                    descriptor_read = false;
	uint64_t                table_base      = 0;
	uint64_t                table_size      = 0;
	std::vector<uint32_t>   words;
	bool                    table_truncated = false;
	bool                    read_failed     = false;
	std::string             status;
};

struct TargetSnapshot {
	uint64_t                raw_address  = 0;
	size_t                  table_index  = 0;
	size_t                  record_index = 0;
	std::array<uint32_t, 2> auxiliary_words {};
	std::vector<uint32_t>   words;
	bool                    read_failed   = false;
	bool                    prefix_capped = false;
	std::string             status;
};

struct CallCapture {
	bool                           enabled                       = false;
	bool                           call_sites_truncated          = false;
	bool                           table_budget_exhausted        = false;
	bool                           target_limit_reached          = false;
	size_t                         zero_targets                  = 0;
	size_t                         misaligned_targets            = 0;
	size_t                         outside_address_space_targets = 0;
	size_t                         duplicate_targets             = 0;
	size_t                         descriptor_read_requests      = 0;
	size_t                         table_bytes_reserved          = 0;
	size_t                         table_read_bytes_requested    = 0;
	size_t                         target_read_bytes_requested   = 0;
	std::vector<CallTableSnapshot> tables;
	std::vector<TargetSnapshot>    targets;
};

[[nodiscard]] CallTableTraceResult TraceCallTables(const Decoder::Program&   program,
                                                   std::span<const uint32_t> user_data,
                                                   uint32_t                  user_data_base = 0);
// Pure capture: no files, no guest execution, and no callback calls when disabled.
[[nodiscard]] CallCapture CaptureCallTables(bool enabled, const Decoder::Program& program,
                                            std::span<const uint32_t> user_data,
                                            MemoryReader reader, void* reader_context = nullptr,
                                            uint32_t user_data_base = 0);

} // namespace Libs::Graphics::ShaderRecompiler::Diagnostics
