#pragma once

#include "graphics/shader/shader.h"

#include <charconv>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace CapturedExternalTest {

using Fields = std::map<std::string, std::string>;

// Capture fields are whitespace-separated key=value tokens. Human status prose
// is intentionally ignored; every machine field used below must occur exactly
// once.
inline bool AppendFields(std::string_view line, Fields &fields,
                         std::string &failure) {
  while (!line.empty()) {
    const auto start = line.find_first_not_of(" \t\r");
    if (start == std::string_view::npos)
      break;
    line.remove_prefix(start);
    const auto end = line.find_first_of(" \t\r");
    const auto token = line.substr(0, end);
    const auto equal = token.find('=');
    if (equal != std::string_view::npos && equal != 0u) {
      if (!fields
               .emplace(std::string(token.substr(0, equal)),
                        std::string(token.substr(equal + 1u)))
               .second) {
        failure =
            "duplicate manifest field: " + std::string(token.substr(0, equal));
        return false;
      }
    }
    if (end == std::string_view::npos)
      break;
    line.remove_prefix(end);
  }
  return true;
}

template <typename T> inline bool ParseNumber(std::string_view text, T &value) {
  int base = 10;
  if (text.starts_with("0x")) {
    text.remove_prefix(2u);
    base = 16;
  }
  if (text.empty())
    return false;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value, base);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

inline bool ParseBoolean(std::string_view text, bool &value) {
  if (text == "true") {
    value = true;
    return true;
  }
  if (text == "false") {
    value = false;
    return true;
  }
  return false;
}

template <typename T>
inline bool Number(const Fields &fields, const char *name, T &value,
                   std::string &failure) {
  const auto found = fields.find(name);
  if (found != fields.end() && ParseNumber(found->second, value))
    return true;
  failure = "missing or invalid manifest field: " + std::string(name);
  return false;
}

inline bool Boolean(const Fields &fields, const char *name, bool &value,
                    std::string &failure) {
  const auto found = fields.find(name);
  if (found != fields.end() && ParseBoolean(found->second, value))
    return true;
  failure = "missing or invalid manifest field: " + std::string(name);
  return false;
}

template <typename T, size_t N>
inline bool NumberList(const Fields &fields, const char *name, T (&values)[N],
                       std::string &failure) {
  const auto found = fields.find(name);
  if (found == fields.end()) {
    failure = "missing manifest list: " + std::string(name);
    return false;
  }
  std::string_view text = found->second;
  for (size_t i = 0; i < N; ++i) {
    const auto comma = text.find(',');
    const auto component = text.substr(0, comma);
    const bool valid = [&] {
      if constexpr (std::is_same_v<T, bool>)
        return ParseBoolean(component, values[i]);
      else
        return ParseNumber(component, values[i]);
    }();
    if ((i + 1u < N) != (comma != std::string_view::npos) || !valid) {
      failure = "invalid manifest list: " + std::string(name);
      return false;
    }
    if (comma != std::string_view::npos)
      text.remove_prefix(comma + 1u);
  }
  return true;
}

struct ComputeControls {
  Libs::Graphics::ShaderComputeInputInfo compute;
  uint64_t caller_address = 0;
  uint64_t shader_hash = 0;
  uint32_t user_data_base = 0;
  std::vector<uint32_t> user_data;
  size_t caller_bytes = 0;
  size_t decoded_instructions = 0;
};

inline bool ParseComputeControls(const Fields &fields, ComputeControls &result,
                                 std::string &failure) {
  ComputeControls parsed;
  auto &compute = parsed.compute;
  size_t user_count = 0;
  uint32_t float_mode = 0;
  const auto stage = fields.find("stage");
  if (stage == fields.end() || stage->second != "cs") {
    failure = "actual captured translation requires a compute manifest";
    return false;
  }
  if (!Number(fields, "caller_base", parsed.caller_address, failure) ||
      !Number(fields, "caller_hash", parsed.shader_hash, failure) ||
      !Number(fields, "caller_span_bytes", parsed.caller_bytes, failure) ||
      !Number(fields, "decoded_instructions", parsed.decoded_instructions,
              failure) ||
      !Number(fields, "wave_size", compute.wave_size, failure) ||
      !Number(fields, "user_data_base", parsed.user_data_base, failure) ||
      !Number(fields, "user_data_count", user_count, failure) ||
      !NumberList(fields, "threads_num", compute.threads_num, failure) ||
      !Number(fields, "host_subgroup_size", compute.host_subgroup_size,
              failure) ||
      !Number(fields, "lds_size_dwords", compute.lds_size_dwords, failure) ||
      !Number(fields, "scratch_size_dwords", compute.scratch_size_dwords,
              failure) ||
      !NumberList(fields, "dispatch_threads_num", compute.dispatch_threads_num,
                  failure) ||
      !NumberList(fields, "workgroup_counts", compute.workgroup_counts,
                  failure) ||
      !NumberList(fields, "group_id", compute.group_id, failure) ||
      !Number(fields, "thread_ids_num", compute.thread_ids_num, failure) ||
      !Number(fields, "workgroup_register", compute.workgroup_register,
              failure) ||
      !Boolean(fields, "tg_size_en", compute.tg_size_en, failure) ||
      !Boolean(fields, "dispatch_thread_dimensions",
               compute.dispatch_thread_dimensions, failure) ||
      !Boolean(fields, "lds_storage", compute.lds_storage, failure) ||
      !Number(fields, "float_mode", float_mode, failure))
    return false;
  if (parsed.caller_address == 0u ||
      parsed.caller_address >= (uint64_t{1} << 48u) ||
      (parsed.caller_address & 3u) != 0u || parsed.caller_bytes == 0u ||
      parsed.caller_bytes > 1024u * 1024u || parsed.caller_bytes % 4u != 0u ||
      parsed.decoded_instructions == 0u ||
      (compute.wave_size != 32u && compute.wave_size != 64u) ||
      compute.host_subgroup_size == 0u || compute.host_subgroup_size > 128u ||
      (compute.host_subgroup_size & (compute.host_subgroup_size - 1u)) != 0u ||
      parsed.user_data_base > 108u ||
      user_count > 108u - parsed.user_data_base || float_mode > 255u ||
      compute.thread_ids_num < 1 || compute.thread_ids_num > 3 ||
      compute.workgroup_register < 0 || compute.workgroup_register >= 108) {
    failure = "manifest contains out-of-range compute controls";
    return false;
  }
  compute.float_mode = static_cast<uint8_t>(float_mode);
  for (size_t axis = 0; axis < 3u; ++axis) {
    if (compute.threads_num[axis] == 0u) {
      failure =
          "manifest has invalid inherited dimensions or group-ID controls";
      return false;
    }
  }
  parsed.user_data.resize(user_count);
  std::vector<bool> seen(user_count, false);
  size_t seen_user_words = 0;
  for (const auto &[name, value] : fields) {
    if (!name.starts_with("user_sgpr["))
      continue;
    uint32_t reg = 0;
    uint32_t word = 0;
    if (!name.ends_with(']') ||
        !ParseNumber(std::string_view(name).substr(10u, name.size() - 11u),
                     reg) ||
        reg < parsed.user_data_base ||
        reg - parsed.user_data_base >= user_count ||
        !ParseNumber(value, word)) {
      failure = "manifest user SGPR is outside the declared span or malformed";
      return false;
    }
    if (seen[reg - parsed.user_data_base]) {
      failure = "manifest repeats a user SGPR under another numeric spelling";
      return false;
    }
    seen[reg - parsed.user_data_base] = true;
    parsed.user_data[reg - parsed.user_data_base] = word;
    ++seen_user_words;
  }
  if (seen_user_words != user_count) {
    failure = "manifest omits one or more declared user SGPR words";
    return false;
  }
  result = std::move(parsed);
  return true;
}

struct CaptureManifest {
  Fields global;
  std::vector<Fields> tables;
  std::vector<Fields> targets;
};

inline bool ParseCaptureManifest(std::string_view text, CaptureManifest &result,
                                 std::string &failure) {
  if (text.find("(write failed)") != std::string_view::npos) {
    failure = "manifest records a failed capture-file write";
    return false;
  }
  CaptureManifest parsed;
  Fields *current_table = nullptr;
  while (!text.empty()) {
    const auto newline = text.find('\n');
    auto line = text.substr(0, newline);
    const auto start = line.find_first_not_of(" \t\r");
    line = start == std::string_view::npos ? std::string_view{}
                                           : line.substr(start);
    if (!line.empty() && !line.starts_with("limits:")) {
      if (line.starts_with("call[") || line.starts_with("target[")) {
        const bool table = line.starts_with("call[");
        const size_t index_start = table ? 5u : 7u;
        const auto end = line.find("]:", index_start);
        size_t index = 0;
        auto &rows = table ? parsed.tables : parsed.targets;
        if (end == std::string_view::npos ||
            !ParseNumber(line.substr(index_start, end - index_start), index) ||
            index != rows.size() || rows.size() >= (table ? 64u : 2048u)) {
          failure = "manifest row index is missing, repeated or out of bounds";
          return false;
        }
        rows.emplace_back();
        if (!AppendFields(line.substr(end + 2u), rows.back(), failure))
          return false;
        current_table = table ? &rows.back() : nullptr;
      } else if (!AppendFields(line,
                               current_table == nullptr ? parsed.global
                                                        : *current_table,
                               failure)) {
        return false;
      }
    }
    if (newline == std::string_view::npos)
      break;
    text.remove_prefix(newline + 1u);
  }
  if (parsed.tables.empty() || parsed.targets.empty()) {
    failure = "manifest has no captured function table or prefixes";
    return false;
  }
  result = std::move(parsed);
  return true;
}

} // namespace CapturedExternalTest
