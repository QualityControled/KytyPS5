#pragma once

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::MenuPerformanceDiagnostic {

// Exact opt-in, frozen at first use. Independent of graphics dump settings.
inline bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_MENU_PERFORMANCE_DIAGNOSTIC");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

struct Counters {
	std::atomic_uint64_t primary_guest_flips {0};
	std::atomic_uint64_t compiled_permutations {0};
	std::atomic_uint64_t compile_scope_active {0};
	std::atomic_uint64_t compile_scope_ns {0};
	std::atomic_uint64_t warm_materialize_calls {0};
	std::atomic_uint64_t warm_materialize_ns {0};
	std::atomic_uint64_t prepare_bda_calls {0};
	std::atomic_uint64_t prepare_bda_ns {0};
	std::atomic_uint64_t backing_read_calls {0};
	std::atomic_uint64_t backing_read_requested_bytes {0};
	std::atomic_uint64_t backing_read_ns {0};
	std::atomic_uint64_t backing_sync_downloads {0};
	std::atomic_uint64_t backing_download_requested_bytes {0};
	std::atomic_uint64_t backing_download_payload_bytes {0};
	std::atomic_uint64_t backing_download_staging_bytes {0};
	std::atomic_uint64_t backing_download_ns {0};
};

inline Counters& State() {
	static Counters counters;
	return counters;
}

inline void CompletedPrimaryGuestFlip() {
	if (Enabled()) State().primary_guest_flips.fetch_add(1, std::memory_order_relaxed);
}

inline void CompiledPermutation() {
	if (Enabled()) State().compiled_permutations.fetch_add(1, std::memory_order_relaxed);
}

inline uint32_t& BackingReadDepth() {
	static thread_local uint32_t depth = 0;
	return depth;
}

inline uint64_t& BackingReadRequestedBytes() {
	static thread_local uint64_t requested_bytes = 0;
	return requested_bytes;
}

class BackingReadScope {
public:
	explicit BackingReadScope(uint64_t requested_bytes) : m_enabled(Enabled()) {
		if (!m_enabled) return;
		m_start = std::chrono::steady_clock::now();
		++BackingReadDepth();
		m_previous_request = BackingReadRequestedBytes();
		BackingReadRequestedBytes() = requested_bytes;
		State().backing_read_calls.fetch_add(1, std::memory_order_relaxed);
		State().backing_read_requested_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);
	}
	~BackingReadScope() {
		if (!m_enabled) return;
		State().backing_read_ns.fetch_add(static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - m_start).count()), std::memory_order_relaxed);
		--BackingReadDepth();
		BackingReadRequestedBytes() = m_previous_request;
	}
	BackingReadScope(const BackingReadScope&) = delete;
	BackingReadScope& operator=(const BackingReadScope&) = delete;
private:
	bool m_enabled;
	uint64_t m_previous_request = 0;
	std::chrono::steady_clock::time_point m_start {};
};

// TryReadBufferBacking invokes synchronous downloads on its existing GPU thread.
// Observe only their already-built copy sizes and original completed publish.
class BackingDownloadScope {
public:
	explicit BackingDownloadScope(bool synchronous)
	    : m_enabled(Enabled() && synchronous && BackingReadDepth() != 0) {
		if (m_enabled) {
			m_start = std::chrono::steady_clock::now();
			m_requested_bytes = BackingReadRequestedBytes();
		}
	}
	bool Active() const { return m_enabled; }
	void Completed(uint64_t payload_bytes, uint64_t staging_bytes) {
		if (!m_enabled) return;
		auto& counters = State();
		counters.backing_download_requested_bytes.fetch_add(m_requested_bytes, std::memory_order_relaxed);
		counters.backing_download_payload_bytes.fetch_add(payload_bytes, std::memory_order_relaxed);
		counters.backing_download_staging_bytes.fetch_add(staging_bytes, std::memory_order_relaxed);
		counters.backing_download_ns.fetch_add(static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - m_start).count()), std::memory_order_relaxed);
		counters.backing_sync_downloads.fetch_add(1, std::memory_order_relaxed);
		m_enabled = false;
	}
private:
	bool m_enabled;
	uint64_t m_requested_bytes = 0;
	std::chrono::steady_clock::time_point m_start {};
};

enum class TimedOperation { WarmMaterialize, PrepareBda, Compile };

// Elapsed host wall time in completed scopes, including any existing waits.
// This is not thread CPU time and introduces no resource reads or GPU waits.
class TimedScope {
public:
	explicit TimedScope(TimedOperation operation) : m_enabled(Enabled()), m_operation(operation) {
		if (m_enabled) {
			m_start = std::chrono::steady_clock::now();
			if (m_operation == TimedOperation::Compile)
				State().compile_scope_active.fetch_add(1, std::memory_order_relaxed);
		}
	}
	~TimedScope() {
		if (!m_enabled) return;
		const auto elapsed = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - m_start).count());
		auto& counters = State();
		if (m_operation == TimedOperation::WarmMaterialize) {
			counters.warm_materialize_ns.fetch_add(elapsed, std::memory_order_relaxed);
			counters.warm_materialize_calls.fetch_add(1, std::memory_order_relaxed);
		} else if (m_operation == TimedOperation::PrepareBda) {
			counters.prepare_bda_ns.fetch_add(elapsed, std::memory_order_relaxed);
			counters.prepare_bda_calls.fetch_add(1, std::memory_order_relaxed);
		} else {
			counters.compile_scope_ns.fetch_add(elapsed, std::memory_order_relaxed);
			counters.compile_scope_active.fetch_sub(1, std::memory_order_relaxed);
		}
	}
	TimedScope(const TimedScope&) = delete;
	TimedScope& operator=(const TimedScope&) = delete;
private:
	bool m_enabled;
	TimedOperation m_operation;
	std::chrono::steady_clock::time_point m_start {};
};

struct Snapshot {
	uint64_t flips;
	uint64_t compiles;
	uint64_t compile_active;
	uint64_t compile_ns;
	uint64_t warm_calls;
	uint64_t warm_ns;
	uint64_t bda_calls;
	uint64_t bda_ns;
	uint64_t backing_calls;
	uint64_t backing_requested_bytes;
	uint64_t backing_ns;
	uint64_t backing_downloads;
	uint64_t backing_download_requested_bytes;
	uint64_t backing_payload_bytes;
	uint64_t backing_staging_bytes;
	uint64_t backing_download_ns;
};

inline Snapshot ReadCounters() {
	const auto& counters = State();
	return {counters.primary_guest_flips.load(std::memory_order_relaxed),
	        counters.compiled_permutations.load(std::memory_order_relaxed),
	        counters.compile_scope_active.load(std::memory_order_relaxed),
	        counters.compile_scope_ns.load(std::memory_order_relaxed),
	        counters.warm_materialize_calls.load(std::memory_order_relaxed),
	        counters.warm_materialize_ns.load(std::memory_order_relaxed),
	        counters.prepare_bda_calls.load(std::memory_order_relaxed),
	        counters.prepare_bda_ns.load(std::memory_order_relaxed),
	        counters.backing_read_calls.load(std::memory_order_relaxed),
	        counters.backing_read_requested_bytes.load(std::memory_order_relaxed),
	        counters.backing_read_ns.load(std::memory_order_relaxed),
	        counters.backing_sync_downloads.load(std::memory_order_relaxed),
	        counters.backing_download_requested_bytes.load(std::memory_order_relaxed),
	        counters.backing_download_payload_bytes.load(std::memory_order_relaxed),
	        counters.backing_download_staging_bytes.load(std::memory_order_relaxed),
	        counters.backing_download_ns.load(std::memory_order_relaxed)};
}

// Main-window thread only. Atomics replace all reads of mutable guest flip state.
// Window maintenance keeps zero-flip reports possible while GPU work is stalled.
inline void Report(uint64_t now_ms) {
	if (!Enabled()) return;
	static bool initialized = false;
	static uint64_t last_ms = 0;
	static Snapshot last {};
	if (!initialized) {
		last_ms = now_ms;
		last = ReadCounters();
		initialized = true;
		std::printf("MenuPerformanceDiagnostic enabled interval_ms=2000 "
		            "source=completed_primary_guest_flips timing=completed_host_wall_scopes "
		            "not_race_fps=1\n");
		std::fflush(stdout);
		return;
	}
	const uint64_t elapsed_ms = now_ms - last_ms;
	if (elapsed_ms < 2000) return;
	const auto current = ReadCounters();
	const uint64_t flips = current.flips - last.flips;
	std::printf("MenuPerformanceDiagnostic elapsed_ms=%" PRIu64
	            " guest_flips=%" PRIu64 " guest_flips_total=%" PRIu64 " guest_flip_hz=%.3f"
	            " compiled_permutations=%" PRIu64 " compiled_permutations_total=%" PRIu64
	            " compile_scope_active=%" PRIu64 " compile_completed_wall_ms=%.3f"
	            " warm_materialize_calls=%" PRIu64 " warm_materialize_wall_ms=%.3f"
	            " prepare_bda_calls=%" PRIu64 " prepare_bda_wall_ms=%.3f"
	            " backing_read_calls=%" PRIu64 " backing_requested_bytes=%" PRIu64
	            " backing_read_wall_ms=%.3f backing_sync_downloads=%" PRIu64
	            " backing_download_parent_request_bytes=%" PRIu64
	            " backing_download_payload_bytes=%" PRIu64 " backing_download_staging_bytes=%" PRIu64
	            " backing_download_wall_ms=%.3f\n",
	            elapsed_ms, flips, current.flips,
	            static_cast<double>(flips) * 1000.0 / static_cast<double>(elapsed_ms),
	            current.compiles - last.compiles, current.compiles,
	            current.compile_active,
	            static_cast<double>(current.compile_ns - last.compile_ns) / 1000000.0,
	            current.warm_calls - last.warm_calls,
	            static_cast<double>(current.warm_ns - last.warm_ns) / 1000000.0,
	            current.bda_calls - last.bda_calls,
	            static_cast<double>(current.bda_ns - last.bda_ns) / 1000000.0,
	            current.backing_calls - last.backing_calls,
	            current.backing_requested_bytes - last.backing_requested_bytes,
	            static_cast<double>(current.backing_ns - last.backing_ns) / 1000000.0,
	            current.backing_downloads - last.backing_downloads,
	            current.backing_download_requested_bytes - last.backing_download_requested_bytes,
	            current.backing_payload_bytes - last.backing_payload_bytes,
	            current.backing_staging_bytes - last.backing_staging_bytes,
	            static_cast<double>(current.backing_download_ns - last.backing_download_ns) / 1000000.0);
	std::fflush(stdout);
	last_ms = now_ms;
	last = current;
}

} // namespace Libs::Graphics::MenuPerformanceDiagnostic
