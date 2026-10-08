#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Libs::Graphics::CarRenderDiagnostic {

inline bool Enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("KYTY_CAR_RENDER_DIAGNOSTIC");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// This opt-in narrows diagnostic records only; it never changes draw execution.
inline bool GeometryEnabled() {
    if (!Enabled()) return false;
    static const bool enabled = [] {
        const auto* value = std::getenv("KYTY_CAR_RENDER_DIAGNOSTIC_GEOMETRY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// Optional exact prepared-PS identity filter. It narrows diagnostics only.
struct PixelHashFilter {
    bool requested = false, valid = true;
    uint64_t hash = 0;
    static PixelHashFilter Parse(const char* value) {
        if (value == nullptr) return {};
        PixelHashFilter result {true, false, 0};
        for (uint32_t i = 0; i < 16; ++i) {
            const auto c = value[i];
            uint32_t digit = 0;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return result; // Also rejects a short NUL-terminated string before further reads.
            result.hash = (result.hash << 4) | digit;
        }
        result.valid = value[16] == 0;
        return result;
    }
    bool Accept(uint64_t prepared_ps_hash) const {
        return valid && (!requested || prepared_ps_hash == hash);
    }
};

inline const PixelHashFilter& ProcessPixelHashFilter() {
    static const auto filter = PixelHashFilter::Parse(std::getenv("KYTY_CAR_RENDER_DIAGNOSTIC_PS_HASH"));
    return filter;
}

inline bool EligibleDraw(uint32_t source_count, bool depth_test) {
    // DrawIndex supplies guest index_count; DrawIndexAuto supplies guest vertex_count.
    // No instance multiplication or inference that this draw represents a vehicle.
    return !GeometryEnabled() || source_count >= 128 || depth_test;
}

inline bool ValidStartPath(std::string_view input) {
    if (input.empty() || input.size() > 1024 || input.back() == '/' || input.back() == '\\') return false;
    std::string normalized(input);
    for (auto& c: normalized) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    constexpr std::string_view prefix = "d:/codex/gt7-playability/work/";
    if (!normalized.starts_with(prefix) || normalized.size() == prefix.size()) return false;
    if (normalized.find_first_of("\"<>|?*") != std::string::npos || normalized.find(':', 2) != std::string::npos) return false;
    for (size_t first = prefix.size(); first < normalized.size();) {
        const auto end = normalized.find('/', first);
        const auto part = normalized.substr(first, end == std::string::npos ? end : end - first);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        if (end == std::string::npos) break;
        first = end + 1;
    }
    return true;
}

inline bool ReadStartToken(const std::string& path) {
    // Host-only control data, at most15 bytes. No guest memory or shader payload is read.
    std::ifstream file(path, std::ios::binary);
    char bytes[15] {};
    file.read(bytes, sizeof(bytes));
    constexpr char token[] = "KytyCarTrace1\n";
    return file.gcount() == sizeof(token) - 1 && std::memcmp(bytes, token, sizeof(token) - 1) == 0;
}

class StartGate {
public:
    explicit StartGate(std::string path): m_path(std::move(path)), m_valid(m_path.empty() || ValidStartPath(m_path)) {}
    template<class Reader> bool Active(uint64_t now_ns, Reader&& read) {
        if (!m_valid) return false;
        if (m_active) return true;
        if (m_path.empty()) return m_active = true;
        if (m_polled && (now_ns < m_last_poll || now_ns - m_last_poll < 1'000'000'000)) return false;
        m_polled = true;
        m_last_poll = now_ns;
        return m_active = read(m_path);
    }
    bool Valid() const { return m_valid; }
    bool HasStartFile() const { return !m_path.empty(); }
private:
    std::string m_path;
    bool m_valid = false, m_active = false, m_polled = false;
    uint64_t m_last_poll = 0;
};

enum class Claim { Accepted, Duplicate, Full, Oversize };
class SignatureBudget {
public:
    static constexpr size_t MaxRecords = 64, MaxRecordBytes = 32768, MaxTotalBytes = 1024 * 1024;
    static constexpr size_t GeometryMaxRecordBytes = 256 * 1024;
    // Invalid constructor inputs retain the smaller original bound.
    static constexpr size_t ValidatedRecordBytes(size_t requested) {
        return requested == GeometryMaxRecordBytes ? GeometryMaxRecordBytes : MaxRecordBytes;
    }
    explicit SignatureBudget(size_t max_record_bytes = MaxRecordBytes):
        m_max_record_bytes(ValidatedRecordBytes(max_record_bytes)) {}
    size_t RecordBytes() const { return m_max_record_bytes; }
    Claim Admit(const std::string& signature) {
        if (signature.size() > m_max_record_bytes) return Claim::Oversize;
        if (std::find(m_signatures.begin(), m_signatures.end(), signature) != m_signatures.end()) return Claim::Duplicate;
        if (m_signatures.size() == MaxRecords || signature.size() > MaxTotalBytes - m_bytes) return Claim::Full;
        m_signatures.push_back(signature);
        m_bytes += signature.size();
        return Claim::Accepted;
    }
    size_t Count() const { return m_signatures.size(); }
    size_t Bytes() const { return m_bytes; }
    bool Exhausted() const { return m_signatures.size() == MaxRecords || m_bytes == MaxTotalBytes; }
private:
    const size_t m_max_record_bytes;
    std::vector<std::string> m_signatures;
    size_t m_bytes = 0;
};

class BoundedText {
public:
    explicit BoundedText(size_t max_record_bytes = SignatureBudget::MaxRecordBytes):
        m_max_record_bytes(SignatureBudget::ValidatedRecordBytes(max_record_bytes)) {}
    size_t RecordBytes() const { return m_max_record_bytes; }
    bool Add(std::string text) {
        if (text.size() > m_max_record_bytes - m_text.size()) { m_oversize = true; return false; }
        m_text += text;
        return true;
    }
    const std::string& Text() const { return m_text; }
    bool Oversize() const { return m_oversize; }
private:
    const size_t m_max_record_bytes;
    std::string m_text;
    bool m_oversize = false;
};

template<class T> uint64_t HandleBits(T handle) {
    if constexpr (std::is_pointer_v<T>) return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    else return static_cast<uint64_t>(handle);
}

inline StartGate& ProcessStartGate() {
    static StartGate gate([] {
        const auto* path = std::getenv("KYTY_CAR_RENDER_DIAGNOSTIC_START_FILE");
        if (path == nullptr) return std::string {};
        size_t length = 0;
        while (length <= 1024 && path[length] != 0) ++length;
        return std::string(path, length);
    }());
    return gate;
}

// Renderer GPU lane only. Disabled mode returns before clock/file/cache/timing work.
inline bool Ready() {
    if (!Enabled()) return false;
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    return ProcessStartGate().Active(now, ReadStartToken);
}

} // namespace Libs::Graphics::CarRenderDiagnostic
