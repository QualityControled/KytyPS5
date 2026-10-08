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
    Claim Admit(const std::string& signature) {
        if (signature.size() > MaxRecordBytes) return Claim::Oversize;
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
    std::vector<std::string> m_signatures;
    size_t m_bytes = 0;
};

class BoundedText {
public:
    bool Add(std::string text) {
        if (text.size() > SignatureBudget::MaxRecordBytes - m_text.size()) { m_oversize = true; return false; }
        m_text += text;
        return true;
    }
    const std::string& Text() const { return m_text; }
    bool Oversize() const { return m_oversize; }
private:
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
