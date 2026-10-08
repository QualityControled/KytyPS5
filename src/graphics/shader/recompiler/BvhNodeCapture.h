#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// No guest pointer dereference, runtime synchronization or
// intersection oracle is implemented here.
namespace Libs::Graphics::ShaderRecompiler::BvhNodeCapture {

inline constexpr uint64_t GuestAddressLimit     = uint64_t {1} << 48;
inline constexpr size_t   NodeBlockBytes        = 64;
inline constexpr size_t   MaxViewRequestedBytes = 128;
using EventWords                                = std::array<uint32_t, 16>;
using NodeBlock                                 = std::array<uint8_t, NodeBlockBytes>;

enum class NodeWidth { Unknown, Narrow32, Wide64 };
enum class View { CpuBefore, SynchronizedBuffer };

struct DecodedEvent {
	EventWords               raw {};
	NodeWidth                width                    = NodeWidth::Unknown;
	uint64_t                 reported_address         = 0;
	uint64_t                 guest_pc                 = 0;
	uint64_t                 shader_hash              = 0;
	uint64_t                 raw_node                 = 0;
	uint64_t                 base                     = 0;
	uint64_t                 index                    = 0;
	uint64_t                 last_index               = 0;
	uint64_t                 max_index                = 0;
	uint64_t                 byte_offset              = 0;
	uint64_t                 address                  = 0;
	uint64_t                 end_exclusive            = 0;
	uint64_t                 address_limit            = GuestAddressLimit;
	uint32_t                 kind                     = 0;
	uint32_t                 descriptor_type          = 0;
	uint32_t                 grow_ulps                = 0;
	bool                     sort                     = false;
	bool                     triangle_return_mode     = false;
	uint32_t                 reserved_d1              = 0;
	uint32_t                 reserved_d3              = 0;
	size_t                   block_count              = 0;
	bool                     address_arithmetic_valid = false;
	bool                     may_read                 = false;
	std::vector<std::string> errors;
};

// Width must be established from a decoded original opcode by the caller. A
// high zero word alone proves neither a narrow nor a wide instruction.
DecodedEvent DecodeEvent(const EventWords& words, NodeWidth width = NodeWidth::Unknown,
                         uint64_t address_limit = GuestAddressLimit);
bool ParseEventLittleEndian(std::span<const uint8_t> bytes, EventWords& words, std::string& error);
std::array<uint8_t, 64> EncodeEventLittleEndian(const EventWords& words);

using Read = bool (*)(void* context, uint64_t address, uint8_t* output, size_t bytes);
struct Reader {
	Read  read    = nullptr;
	void* context = nullptr;
};

struct BlockOutcome {
	uint64_t             address         = 0;
	size_t               requested_bytes = 0;
	bool                 attempted       = false;
	bool                 succeeded       = false;
	std::vector<uint8_t> payload; // Empty on every failure, including scribbling callbacks.
	std::string          error;
};

struct Snapshot {
	View                      view = View::SynchronizedBuffer;
	EventWords                raw_event {};
	bool                      complete        = false;
	size_t                    requested_bytes = 0;
	size_t                    succeeded_bytes = 0;
	std::vector<BlockOutcome> blocks;
	std::string               error;
};

// One named view only: at most two requests of exactly64 bytes. Failed attempts
// consume the budget. Reading stops at the first failure. CpuBefore can be stale;
// SynchronizedBuffer is only a label for an externally synchronized reader.
Snapshot CaptureView(const DecodedEvent& event, View view, Reader reader);

enum class ViewComparison { DifferentEvents, Incomplete, Equal, Different };
ViewComparison CompareAlreadyCapturedViews(const Snapshot& cpu_before, const Snapshot& buffer_view);
struct NodeBits {
	bool                                   complete = false;
	uint32_t                               kind     = 0;
	std::vector<uint32_t>                  raw_words;
	std::array<std::array<uint32_t, 3>, 5> vertex_f32_bits {};
	std::array<uint32_t, 3>                selected_vertices {};
	uint32_t                               triangle_flag = 0;
	std::array<uint32_t, 4>                children {};
	// Per child: minXYZ then maxXYZ. FP16 values remain raw16-bit patterns.
	std::array<std::array<uint16_t, 6>, 4> bounds_f16_bits {};
	std::array<std::array<uint32_t, 6>, 4> bounds_f32_bits {};
	std::string                            error;
};
NodeBits    DecodeNodeBits(const DecodedEvent& event, const Snapshot& snapshot);
std::string Describe(const DecodedEvent& event);
std::string Describe(const Snapshot& snapshot);

} // namespace Libs::Graphics::ShaderRecompiler::BvhNodeCapture
