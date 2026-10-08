#include "graphics/shader/recompiler/BvhNodeCapture.h"

#include <iomanip>
#include <limits>
#include <sstream>

namespace Libs::Graphics::ShaderRecompiler::BvhNodeCapture {
namespace {
uint64_t Pair(uint32_t lo, uint32_t hi) {
	return uint64_t {lo} | (uint64_t {hi} << 32);
}
bool Add(uint64_t a, uint64_t b, uint64_t& result) {
	if (b > std::numeric_limits<uint64_t>::max() - a) return false;
	result = a + b;
	return true;
}
bool Multiply(uint64_t a, uint64_t b, uint64_t& result) {
	if (b != 0 && a > std::numeric_limits<uint64_t>::max() / b) return false;
	result = a * b;
	return true;
}
const char* WidthName(NodeWidth width) {
	switch (width) {
		case NodeWidth::Narrow32: return "externally-proven narrow32";
		case NodeWidth::Wide64: return "externally-proven wide64";
		default: return "unknown: original opcode evidence required";
	}
}
const char* ViewName(View view) {
	return view == View::CpuBefore ? "cpu-before (may be stale)"
	                               : "synchronized-buffer-view (external synchronization required)";
}
} // namespace

DecodedEvent DecodeEvent(const EventWords& w, NodeWidth width, uint64_t address_limit) {
	DecodedEvent e;
	e.raw                  = w;
	e.width                = width;
	e.address_limit        = address_limit;
	e.reported_address     = Pair(w[2], w[3]);
	e.guest_pc             = Pair(w[4], w[5]);
	e.shader_hash          = Pair(w[6], w[7]);
	e.raw_node             = Pair(w[12], w[13]);
	e.kind                 = w[12] & 7;
	e.index                = e.raw_node >> 3;
	e.max_index            = Pair(w[10], w[11] & 0x3ff);
	e.base                 = Pair(w[8], w[9] & 0xff) << 8;
	e.descriptor_type      = w[11] >> 28;
	e.grow_ulps            = (w[9] >> 23) & 0xff;
	e.sort                 = (w[9] >> 31) != 0;
	e.triangle_return_mode = ((w[11] >> 24) & 1) != 0;
	e.reserved_d1          = w[9] & 0x007fff00;
	e.reserved_d3          = w[11] & 0x0efffc00;
	e.block_count          = e.kind == 5 ? 2 : (e.kind <= 4 ? 1 : 0);
	const auto reject      = [&](const char* error) { e.errors.emplace_back(error); };
	if (w[0] != 1 || w[1] != 7) reject("event is not a completed kind7 record");
	if ((w[14] | w[15]) == 0) reject("kind7 event has no active EXEC lane");
	if (width == NodeWidth::Narrow32 && w[13] != 0)
		reject("externally-proven narrow opcode conflicts with nonzero node high word");
	if (e.descriptor_type != 8) reject("unsupported BVH descriptor type (requires8)");
	if (e.kind > 5) reject("unsupported node kind6/7");
	if (address_limit == 0 || address_limit > GuestAddressLimit)
		reject("invalid guest address limit");

	bool arithmetic = Add(e.index, e.kind == 5 ? 1 : 0, e.last_index);
	if (!arithmetic) reject("last-index addition overflow");
	if (arithmetic && !Multiply(e.index, 64, e.byte_offset)) {
		reject("node-index multiplication overflow");
		arithmetic = false;
	}
	if (arithmetic && !Add(e.base, e.byte_offset, e.address)) {
		reject("base-plus-offset addition overflow");
		arithmetic = false;
	}
	if (arithmetic && !Add(e.address, e.block_count * NodeBlockBytes, e.end_exclusive)) {
		reject("node range end addition overflow");
		arithmetic = false;
	}
	e.address_arithmetic_valid = arithmetic;
	if (arithmetic) {
		if (e.last_index > e.max_index) reject("node index exceeds descriptor bound");
		if (e.address >= address_limit || e.end_exclusive > address_limit)
			reject("entire node range is not inside the guest address space");
		if (e.address != e.reported_address)
			reject("checked derived address differs from raw event address");
	}
	// Reserved fields are reported, not normalized or invented as an ISA rule.
	e.may_read = e.errors.empty();
	return e;
}

bool ParseEventLittleEndian(std::span<const uint8_t> bytes, EventWords& words, std::string& error) {
	words = {};
	if (bytes.size() != 64) {
		error = "event must contain exactly64 bytes";
		return false;
	}
	for (size_t i = 0; i < words.size(); ++i) {
		for (size_t byte = 0; byte < 4; ++byte)
			words[i] |= uint32_t {bytes[i * 4 + byte]} << (8 * byte);
	}
	error.clear();
	return true;
}

std::array<uint8_t, 64> EncodeEventLittleEndian(const EventWords& words) {
	std::array<uint8_t, 64> bytes {};
	for (size_t i = 0; i < words.size(); ++i)
		for (size_t byte = 0; byte < 4; ++byte)
			bytes[i * 4 + byte] = static_cast<uint8_t>(words[i] >> (8 * byte));
	return bytes;
}

Snapshot CaptureView(const DecodedEvent& e, View view, Reader reader) {
	Snapshot s;
	s.view      = view;
	s.raw_event = e.raw;
	// Revalidate every field; a mutable public DecodedEvent cannot bypass guards.
	const auto checked = DecodeEvent(e.raw, e.width, e.address_limit);
	if (!e.may_read || !checked.may_read || e.address != checked.address ||
	    e.block_count != checked.block_count) {
		s.error = "invalid event: snapshot reads prohibited";
		return s;
	}
	if (reader.read == nullptr) {
		s.error = "memory reader unavailable";
		return s;
	}
	for (size_t block = 0; block < checked.block_count; ++block) {
		BlockOutcome outcome;
		outcome.address = checked.address + block * NodeBlockBytes;
		if (NodeBlockBytes > MaxViewRequestedBytes - s.requested_bytes) {
			s.error = "bounded view request budget exhausted";
			return s;
		}
		outcome.attempted       = true;
		outcome.requested_bytes = NodeBlockBytes;
		s.requested_bytes += NodeBlockBytes;
		NodeBlock temporary {};
		outcome.succeeded =
		    reader.read(reader.context, outcome.address, temporary.data(), temporary.size());
		if (outcome.succeeded) {
			s.succeeded_bytes += temporary.size();
			outcome.payload.assign(temporary.begin(), temporary.end());
		} else {
			outcome.error = "exact64-byte reader request failed; no payload retained";
			s.error       = outcome.error;
		}
		s.blocks.push_back(std::move(outcome));
		if (!s.blocks.back().succeeded) return s;
	}
	s.complete = true;
	return s;
}

ViewComparison CompareAlreadyCapturedViews(const Snapshot& cpu, const Snapshot& buffer) {
	if (cpu.raw_event != buffer.raw_event || cpu.view != View::CpuBefore ||
	    buffer.view != View::SynchronizedBuffer)
		return ViewComparison::DifferentEvents;
	if (!cpu.complete || !buffer.complete || cpu.blocks.size() != buffer.blocks.size())
		return ViewComparison::Incomplete;
	for (size_t i = 0; i < cpu.blocks.size(); ++i) {
		if (cpu.blocks[i].address != buffer.blocks[i].address || !cpu.blocks[i].succeeded ||
		    !buffer.blocks[i].succeeded || cpu.blocks[i].payload.size() != 64 ||
		    buffer.blocks[i].payload.size() != 64)
			return ViewComparison::Incomplete;
		if (cpu.blocks[i].payload != buffer.blocks[i].payload) return ViewComparison::Different;
	}
	return ViewComparison::Equal;
}

NodeBits DecodeNodeBits(const DecodedEvent& e, const Snapshot& s) {
	NodeBits   result;
	const auto checked = DecodeEvent(e.raw, e.width, e.address_limit);
	result.kind        = checked.kind;
	if (!checked.may_read || s.raw_event != e.raw || !s.complete ||
	    s.blocks.size() != checked.block_count) {
		result.error = "node decode requires a complete snapshot of this exact valid event";
		return result;
	}
	for (size_t block = 0; block < s.blocks.size(); ++block) {
		const auto& b = s.blocks[block];
		if (!b.succeeded || b.payload.size() != 64 || b.address != checked.address + block * 64) {
			result.error = "node decode requires exact successful64-byte block payloads";
			return result;
		}
		for (size_t word = 0; word < 16; ++word) {
			uint32_t value = 0;
			for (size_t byte = 0; byte < 4; ++byte)
				value |= uint32_t {b.payload[word * 4 + byte]} << (8 * byte);
			result.raw_words.push_back(value);
		}
	}
	if (checked.kind <= 3) {
		for (size_t vertex = 0; vertex < 5; ++vertex)
			for (size_t axis = 0; axis < 3; ++axis)
				result.vertex_f32_bits[vertex][axis] = result.raw_words[vertex * 3 + axis];
		const std::array<std::array<uint32_t, 3>, 4> triangles {
		    {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}, {2, 4, 0}}};
		result.selected_vertices = triangles[checked.kind];
		result.triangle_flag     = result.raw_words[15];
	} else {
		for (size_t child = 0; child < 4; ++child) {
			result.children[child] = result.raw_words[child];
			for (size_t component = 0; component < 6; ++component) {
				if (checked.kind == 4) {
					const auto packed = result.raw_words[4 + child * 3 + component / 2];
					result.bounds_f16_bits[child][component] =
					    static_cast<uint16_t>(packed >> ((component % 2) * 16));
				} else {
					result.bounds_f32_bits[child][component] =
					    result.raw_words[4 + child * 6 + component];
				}
			}
		}
	}
	result.complete = true;
	return result;
}

std::string Describe(const DecodedEvent& e) {
	std::ostringstream out;
	out << "node_width=" << WidthName(e.width) << "\nraw_event=";
	out << std::hex << std::setfill('0');
	for (auto word: e.raw)
		out << " " << std::setw(8) << word;
	out << "\nguest_pc=0x" << std::setw(16) << e.guest_pc << " shader_hash=0x" << std::setw(16)
	    << e.shader_hash << "\nraw_node=0x" << std::setw(16) << e.raw_node << " base=0x"
	    << std::setw(16) << e.base << " checked_address=0x" << std::setw(16) << e.address
	    << " reported_address=0x" << std::setw(16) << e.reported_address << " end_exclusive=0x"
	    << std::setw(16) << e.end_exclusive << "\nreserved_d1=0x" << std::setw(8) << e.reserved_d1
	    << " reserved_d3=0x" << std::setw(8) << e.reserved_d3 << std::dec << "\nkind=" << e.kind
	    << " index=" << e.index << " last_index=" << e.last_index << " max_index=" << e.max_index
	    << " type=" << e.descriptor_type << " grow_ulps=" << e.grow_ulps << " sort=" << e.sort
	    << " triangle_return_mode=" << e.triangle_return_mode << " blocks=" << e.block_count
	    << " may_read=" << e.may_read << '\n';
	for (const auto& error: e.errors)
		out << "rejection=" << error << '\n';
	out << "winner_lane=unknown; EXEC is an active mask, not a winner identifier\n";
	return out.str();
}

std::string Describe(const Snapshot& s) {
	std::ostringstream out;
	out << "view=" << ViewName(s.view) << "\ncomplete=" << s.complete
	    << " requested_bytes=" << s.requested_bytes << " succeeded_bytes=" << s.succeeded_bytes
	    << '\n';
	for (size_t i = 0; i < s.blocks.size(); ++i) {
		const auto& block = s.blocks[i];
		out << "block[" << i << "] address=0x" << std::hex << block.address << std::dec
		    << " attempted=" << block.attempted << " succeeded=" << block.succeeded
		    << " requested_bytes=" << block.requested_bytes
		    << " retained_bytes=" << block.payload.size() << '\n';
	}
	if (!s.error.empty()) out << "rejection=" << s.error << '\n';
	return out.str();
}
} // namespace Libs::Graphics::ShaderRecompiler::BvhNodeCapture
