#include "graphics/shader/recompiler/BvhDiagnosticRecord.h"
#include "graphics/shader/recompiler/BvhNodeCapture.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Libs::Graphics::ShaderRecompiler::BvhNodeCapture;
namespace {
void Require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
bool Contains(const DecodedEvent &e, const std::string &text) {
  return std::any_of(e.errors.begin(), e.errors.end(), [&](const auto &s) {
    return s.find(text) != std::string::npos;
  });
}
EventWords Event(uint32_t kind = 0, uint64_t index = 2,
                 uint64_t base = 0x123400, uint64_t bound = 100) {
  EventWords w{};
  w[0] = 1;
  w[1] = 7;
  const auto address = base + index * 64;
  w[2] = static_cast<uint32_t>(address);
  w[3] = static_cast<uint32_t>(address >> 32);
  w[4] = 0x12345678;
  w[5] = 0x12;
  w[6] = 0x1e1ea34a;
  w[7] = 0xcfbc46ff;
  const auto encoded_base = base >> 8;
  w[8] = static_cast<uint32_t>(encoded_base);
  w[9] = static_cast<uint32_t>(encoded_base >> 32);
  w[10] = static_cast<uint32_t>(bound);
  w[11] = 0x80000000 | static_cast<uint32_t>(bound >> 32);
  const auto node = (index << 3) | kind;
  w[12] = static_cast<uint32_t>(node);
  w[13] = static_cast<uint32_t>(node >> 32);
  w[14] = 0x80000000;
  w[15] = 0;
  return w;
}
struct Mock {
  std::vector<std::pair<uint64_t, size_t>> requests;
  size_t fail_request = 0;
  uint8_t salt = 0;
  std::vector<uint32_t> explicit_words;
  uint64_t first_address = 0;
  static bool ReadBytes(void *context, uint64_t address, uint8_t *output,
                        size_t bytes) {
    auto &m = *static_cast<Mock *>(context);
    m.requests.emplace_back(address, bytes);
    // Always write even on failure: the helper must not retain these bytes.
    for (size_t i = 0; i < bytes; ++i)
      output[i] = static_cast<uint8_t>((address + i) ^ m.salt);
    if (!m.explicit_words.empty()) {
      const auto offset = address - m.first_address;
      for (size_t i = 0; i < bytes; ++i) {
        const auto position = offset + i;
        output[i] = static_cast<uint8_t>(m.explicit_words[position / 4] >>
                                         ((position % 4) * 8));
      }
    }
    return m.fail_request == 0 || m.requests.size() != m.fail_request;
  }
  Reader ReaderValue() { return {ReadBytes, this}; }
};

void SchemaAndRaw() {
  auto w = Event();
  w[9] |= 0xd5800000;
  w[11] |= 0x01000000;
  auto e = DecodeEvent(w);
  Require(e.may_read && e.raw == w, "valid raw16 event not preserved");
  Require(e.width == NodeWidth::Unknown && e.raw[13] == 0,
          "node high0 must not infer instruction width");
  Require(e.grow_ulps == 0xab && e.sort && e.triangle_return_mode,
          "descriptor control fields decode mismatch");
  Require(e.guest_pc == 0x1212345678 && e.shader_hash == 0xcfbc46ff1e1ea34a,
          "full64 PC/hash truncated");
  Require(e.base == 0x123400 && e.index == 2 && e.address == 0x123480,
          "checked address decode mismatch");
  auto bytes = EncodeEventLittleEndian(w);
  EventWords parsed;
  std::string error;
  Require(ParseEventLittleEndian(bytes, parsed, error) && parsed == w,
          "LE raw event roundtrip failed");
  std::array<uint8_t, 63> short_bytes{};
  Require(!ParseEventLittleEndian(short_bytes, parsed, error) &&
              parsed == EventWords{},
          "partial event accepted or stale output retained");
  auto extra = std::vector<uint8_t>(65);
  Require(!ParseEventLittleEndian(extra, parsed, error),
          "overlong event accepted");
  std::cout << "schema/raw/endianness passed\n";
}

void WidthEvidence() {
  auto w = Event();
  Require(DecodeEvent(w, NodeWidth::Narrow32).may_read &&
              DecodeEvent(w, NodeWidth::Wide64).may_read,
          "explicit width evidence with high0 rejected");
  w = Event(4, uint64_t{1} << 29, 0x123400, (uint64_t{1} << 29) + 2);
  Require(w[13] != 0 && DecodeEvent(w, NodeWidth::Wide64).may_read,
          "valid wide node rejected");
  Require(!DecodeEvent(w, NodeWidth::Narrow32).may_read &&
              Contains(DecodeEvent(w, NodeWidth::Narrow32), "narrow opcode"),
          "narrow proof conflicting raw highword accepted");
  Require(DecodeEvent(w).width == NodeWidth::Unknown,
          "nonzero highword must not claim decoded opcode evidence");
  std::cout << "externally-proven/unknown width passed\n";
}

void TypeKindBounds() {
  for (uint32_t kind = 0; kind <= 5; ++kind) {
    auto e = DecodeEvent(Event(kind, 9, 0x1000, kind == 5 ? 10 : 9));
    Require(e.may_read && e.block_count == (kind == 5 ? 2 : 1),
            "valid inclusive bound failed");
    e = DecodeEvent(Event(kind, 9, 0x1000, kind == 5 ? 9 : 8));
    Require(!e.may_read && Contains(e, "descriptor bound"),
            "index/second block out-of-bound accepted");
  }
  for (auto kind : {6u, 7u})
    Require(!DecodeEvent(Event(kind)).may_read, "unsupported kind accepted");
  auto wrong = Event();
  wrong[11] = 0x70000000;
  Require(!DecodeEvent(wrong).may_read, "wrong descriptor type accepted");
  wrong = Event();
  wrong[0] = 0;
  Require(!DecodeEvent(wrong).may_read, "unfinished CAS record accepted");
  wrong = Event();
  wrong[1] = 3;
  Require(!DecodeEvent(wrong).may_read, "non-kind7 record accepted");
  wrong = Event();
  wrong[14] = wrong[15] = 0;
  Require(!DecodeEvent(wrong).may_read, "inactive kind7 record accepted");
  std::cout << "type/kind/format/inclusive bounds passed\n";
}

void ArithmeticAndSpace() {
  auto w = Event();
  w[12] = w[13] = 0xffffffff;
  auto e = DecodeEvent(w);
  Require(!e.may_read && Contains(e, "multiplication overflow"),
          "wrapped node offset accepted");
  w = Event(0, 0, GuestAddressLimit - 256, 3);
  // Raw index gives offsetUINT64_MAX rounded down to64; valid base then
  // overflows.
  const auto index = std::numeric_limits<uint64_t>::max() / 64;
  const auto node = index << 3;
  w[12] = static_cast<uint32_t>(node);
  w[13] = static_cast<uint32_t>(node >> 32);
  e = DecodeEvent(w);
  Require(!e.may_read && Contains(e, "base-plus-offset addition overflow"),
          "wrapped base plus offset accepted");
  e = DecodeEvent(Event(0, index, 0, 100));
  Require(!e.may_read && Contains(e, "range end addition overflow"),
          "wrapped range end accepted");
  // Valid highest final64B block ends exactly at the exclusive48-bit limit.
  e = DecodeEvent(Event(0, 3, GuestAddressLimit - 256, 3));
  Require(e.may_read && e.end_exclusive == GuestAddressLimit,
          "last complete block at48-bit boundary rejected");
  e = DecodeEvent(Event(5, 3, GuestAddressLimit - 256, 4));
  Require(!e.may_read && Contains(e, "entire node range"),
          "FP32 second block outside48-bit range accepted");
  auto edge = Event(0, 0, 0x1000, 0);
  Require(DecodeEvent(edge, NodeWidth::Unknown, 0x1040).may_read,
          "explicit lower address limit inclusive end rejected");
  Require(!DecodeEvent(edge, NodeWidth::Unknown, 0x103f).may_read,
          "partial final block below limit accepted");
  auto bad_limit = DecodeEvent(edge, NodeWidth::Unknown, 0);
  Require(!bad_limit.may_read, "invalid boundary accepted");
  w = Event();
  ++w[2];
  Require(!DecodeEvent(w).may_read && Contains(DecodeEvent(w), "differs"),
          "event-derived address mismatch accepted");
  std::cout << "checked arithmetic/full-range/event address passed\n";
}

void ReservedBits() {
  auto w = Event();
  w[9] |= 0x00420100;
  w[11] |= 0x04002400;
  auto e = DecodeEvent(w);
  Require(e.may_read && e.reserved_d1 == 0x00420100 &&
              e.reserved_d3 == 0x04002400,
          "reserved words silently cleared or invented as rejection rule");
  Require(e.raw == w, "reserved raw fields not retained");
  std::cout << "reserved-field reporting passed\n";
}

void ExactBudgets() {
  for (uint32_t kind = 0; kind <= 5; ++kind) {
    auto e = DecodeEvent(Event(kind));
    Mock m;
    auto s = CaptureView(e, View::SynchronizedBuffer, m.ReaderValue());
    const size_t count = kind == 5 ? 2 : 1;
    Require(s.complete && s.blocks.size() == count &&
                s.requested_bytes == count * 64 &&
                s.succeeded_bytes == count * 64,
            "request/success budget mismatch");
    Require(m.requests.size() == count, "unexpected node tree traversal");
    for (size_t i = 0; i < count; ++i)
      Require(m.requests[i] ==
                  std::pair<uint64_t, size_t>{e.address + i * 64, 64},
              "request is not exact separately-labeled64B block");
  }
  std::cout << "exact64/128byte request budgets passed\n";
}

void FailedReads() {
  const auto e = DecodeEvent(Event(5));
  for (size_t failing : {1u, 2u}) {
    Mock m;
    m.fail_request = failing;
    auto s = CaptureView(e, View::SynchronizedBuffer, m.ReaderValue());
    Require(!s.complete && m.requests.size() == failing &&
                s.requested_bytes == failing * 64 &&
                s.succeeded_bytes == (failing - 1) * 64,
            "failed request uncharged or requests continue past failure");
    Require(s.blocks.back().payload.empty() && !s.blocks.back().succeeded,
            "scribbled failed payload treated as success");
    Require(!DecodeNodeBits(e, s).complete, "partial node decoded as complete");
  }
  Mock zero;
  zero.salt = 0;
  zero.explicit_words.resize(32);
  zero.first_address = e.address;
  auto s = CaptureView(e, View::SynchronizedBuffer, zero.ReaderValue());
  Require(s.complete && std::all_of(s.blocks[0].payload.begin(),
                                    s.blocks[0].payload.end(),
                                    [](auto byte) { return byte == 0; }),
          "valid all-zero reader payload mistaken for failure");
  std::cout << "failed-first/second/zero-success distinction passed\n";
}

void InvalidNoReads() {
  Mock m;
  for (auto invalid : {Event(6), Event(5, 2, 0x1000, 2)}) {
    auto e = DecodeEvent(invalid);
    auto s = CaptureView(e, View::CpuBefore, m.ReaderValue());
    Require(!s.complete && s.requested_bytes == 0 && m.requests.empty(),
            "invalid inputs triggered memory reader");
  }
  auto e = DecodeEvent(Event());
  auto absent = CaptureView(e, View::CpuBefore, {});
  Require(!absent.complete && absent.requested_bytes == 0,
          "missing callback accepted");
  auto forged = e;
  forged.address += 64;
  auto s = CaptureView(forged, View::CpuBefore, m.ReaderValue());
  Require(!s.complete && m.requests.empty(),
          "mutable decoded address bypassed raw guards");
  forged = DecodeEvent(Event(5, 2, 0x1000, 2));
  forged.may_read = true;
  s = CaptureView(forged, View::CpuBefore, m.ReaderValue());
  Require(!s.complete && m.requests.empty(),
          "mutable success bypassed raw guards");
  std::cout << "invalid/missing/forged inputs no-read passed\n";
}

void SeparateViews() {
  auto e = DecodeEvent(Event());
  Mock cpu, buffer;
  cpu.salt = 0x11;
  buffer.salt = 0x22;
  auto c = CaptureView(e, View::CpuBefore, cpu.ReaderValue());
  auto b = CaptureView(e, View::SynchronizedBuffer, buffer.ReaderValue());
  Require(CompareAlreadyCapturedViews(c, b) == ViewComparison::Different,
          "distinct stale CPU and buffer bytes conflated");
  Require(cpu.requests.size() == 1 && buffer.requests.size() == 1,
          "view comparison caused more reads");
  buffer.salt = cpu.salt;
  b = CaptureView(e, View::SynchronizedBuffer, buffer.ReaderValue());
  Require(CompareAlreadyCapturedViews(c, b) == ViewComparison::Equal,
          "identical views differ");
  b.raw_event[4]++;
  Require(CompareAlreadyCapturedViews(c, b) == ViewComparison::DifferentEvents,
          "different/stale event identity compared as current equality");
  b.raw_event = c.raw_event;
  b.complete = false;
  Require(CompareAlreadyCapturedViews(c, b) == ViewComparison::Incomplete,
          "failed view treated as equality");
  std::cout << "separate CPU/buffer/identity outcomes passed\n";
}

void NodeRawDecode() {
  for (uint32_t kind = 0; kind <= 5; ++kind) {
    const auto e = DecodeEvent(Event(kind));
    Mock m;
    m.explicit_words.resize(kind == 5 ? 32 : 16);
    m.first_address = e.address;
    for (size_t i = 0; i < m.explicit_words.size(); ++i)
      m.explicit_words[i] = 0x80010000 | static_cast<uint32_t>(i);
    m.explicit_words[0] =
        0x7fc01234; // PreserveNaN payload, do not run float math.
    m.explicit_words[1] = 0x80000000; // Preserve negativezero exactly.
    auto s = CaptureView(e, View::SynchronizedBuffer, m.ReaderValue());
    auto node = DecodeNodeBits(e, s);
    Require(node.complete && node.raw_words == m.explicit_words,
            "node raw endian payload not preserved");
    if (kind <= 3) {
      const std::array<std::array<uint32_t, 3>, 4> triangles{
          {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}, {2, 4, 0}}};
      Require(node.selected_vertices == triangles[kind] &&
                  node.vertex_f32_bits[0][0] == 0x7fc01234 &&
                  node.vertex_f32_bits[0][1] == 0x80000000 &&
                  node.triangle_flag == m.explicit_words[15],
              "triangle selected/raw decode failed");
    } else if (kind == 4) {
      for (size_t child = 0; child < 4; ++child)
        for (size_t component = 0; component < 6; ++component)
          Require(node.bounds_f16_bits[child][component] ==
                      static_cast<uint16_t>(
                          m.explicit_words[4 + child * 3 + component / 2] >>
                          ((component % 2) * 16)),
                  "FP16 packed bounds mismatch");
    } else {
      Require(node.bounds_f32_bits[3][5] == m.explicit_words[27] &&
                  node.raw_words.size() == 32,
              "FP32 second-block bounds/padding lost");
    }
    if (kind >= 4)
      Require(node.children[0] == 0x7fc01234, "raw child word changed");
    s.blocks[0].payload.pop_back();
    Require(!DecodeNodeBits(e, s).complete, "short payload decode accepted");
  }
  std::cout << "raw triangle/FP16/FP32 decode passed\n";
}

void RaySidecarSchema() {
  using namespace Libs::Graphics::ShaderRecompiler::Diagnostics;
  std::array<uint32_t, BvhDiagnosticWords> words{};
  const auto first16 = Event(5);
  std::copy(first16.begin(), first16.end(), words.begin());
  words[14] = 0;
  words[15] = 0x80000000u;
  for (size_t i = 0; i < BvhRayWords; ++i)
    words[BvhRayWord + i] = 0x7fc01234u + uint32_t(i);
  words[BvhNodeWidthWord] = 2;
  words[BvhNativeLaneWord] = 63;
  words[BvhSchemaMagicWord] = BvhSchemaMagic;
  words[BvhSchemaVersionWord] = BvhSchemaVersion;
  Require(ValidBvhRaySidecar(words),
          "wide high0 plus valid explicit schema rejected");
  auto mutation = words;
  mutation[BvhNodeWidthWord] = 1;
  Require(ValidBvhRaySidecar(mutation),
          "explicit narrow schema with high0 rejected");
  mutation[13] = 1;
  Require(!ValidBvhRaySidecar(mutation),
          "narrow schema with nonzero raw highword accepted");
  for (auto index :
       {BvhSchemaMagicWord, BvhSchemaVersionWord, size_t(30), size_t(31)}) {
    mutation = words;
    mutation[index] ^= 1;
    Require(!ValidBvhRaySidecar(mutation),
            "corrupt magic/version/reserved schema accepted");
  }
  for (auto lane : {0u, 32u, 62u, 64u, UINT32_MAX}) {
    mutation = words;
    mutation[BvhNativeLaneWord] = lane;
    Require(!ValidBvhRaySidecar(mutation),
            "inactive/out-of-range sidecar winner accepted");
  }
  for (auto width : {0u, 3u, UINT32_MAX}) {
    mutation = words;
    mutation[BvhNodeWidthWord] = width;
    Require(!ValidBvhRaySidecar(mutation),
            "invalid externally-tagged width accepted");
  }
  Require(!ValidBvhRaySidecar(first16),
          "legacy16word record mistaken for ray sidecar");
  mutation = words;
  mutation[1] = 6;
  Require(!ValidBvhRaySidecar(mutation),
          "another diagnostic kind accepted as ray sidecar");
  mutation = words;
  mutation[0] = 0;
  Require(!ValidBvhRaySidecar(mutation),
          "unfinished event accepted as ray sidecar");
  std::cout << "same-winner ray sidecar explicit width/magic/version/EXEC "
               "schema passed\n";
}
} // namespace

int main() {
  try {
    SchemaAndRaw();
    WidthEvidence();
    TypeKindBounds();
    ArithmeticAndSpace();
    ReservedBits();
    ExactBudgets();
    FailedReads();
    InvalidNoReads();
    SeparateViews();
    NodeRawDecode();
    RaySidecarSchema();
    std::cout << "all11 bounded node/schema groups passed (mock reader only)\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
