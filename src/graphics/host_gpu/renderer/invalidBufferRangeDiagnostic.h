#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::InvalidBufferRangeDiagnostic {

inline bool Enabled() noexcept {
    // Called only after the existing range query has failed. No valid-binding
    // environment check, file access, or formatting is added.
    static const bool enabled = [] {
        const auto* value = std::getenv("KYTY_INVALID_BUFFER_RANGE_DIAGNOSTIC");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

class Record {
public:
    static constexpr size_t MaxBytes = 32768;
    static constexpr size_t FooterReserve = 160;
    template<class... Args> void Line(const char* format, Args... args) noexcept {
        if (m_truncated) return;
        const auto available = MaxBytes - FooterReserve - m_size;
        const int written = std::snprintf(m_bytes.data() + m_size, available, format, args...);
        if (written < 0 || static_cast<size_t>(written) >= available) {
            m_truncated = true;
            m_bytes[m_size] = '\0';
            return;
        }
        m_size += static_cast<size_t>(written);
    }
    void Flush() noexcept {
        const int footer = std::snprintf(m_bytes.data() + m_size, MaxBytes - m_size,
            "RangeFaultEnd truncated=%u payload_bytes=%zu no_new_guest_reads=1 output_only=1\n",
            unsigned(m_truncated), m_size);
        if (footer > 0 && static_cast<size_t>(footer) < MaxBytes - m_size) m_size += footer;
        std::fwrite(m_bytes.data(), 1, m_size, stdout);
        std::fflush(stdout);
    }
private:
    std::array<char, MaxBytes> m_bytes {};
    size_t m_size = 0;
    bool m_truncated = false;
};

} // namespace Libs::Graphics::InvalidBufferRangeDiagnostic
