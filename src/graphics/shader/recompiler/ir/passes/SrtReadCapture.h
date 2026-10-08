#pragma once

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <map>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// The host supplies real immutable input/library artifact identities. These are
// references to exact captured inputs, never substitutes for byte equality.
struct SrtReadCaptureIdentity {
	ShaderType stage = ShaderType::Unknown;
	uint64_t shader_hash = 0;
	uint64_t shader_base = 0;
	uint64_t pass_id = 0;
	uint32_t user_data_base = 0;
	std::vector<uint32_t> user_data;
	std::string control_schema;
	std::vector<uint32_t> control_words;
	std::string caller_identity;
	std::string library_identity;
	std::string input_identity;
};

struct SrtReadCaptureLimits {
	uint64_t unique_bytes = 256u * 1024u;
	uint64_t observation_bytes = 1024u * 1024u;
	uint64_t observations = 65536u;
};

enum class SrtReadCaptureStatus {
	Collecting, Complete, MissingIdentity, ReadFailed, InvalidSpan,
	ObservationByteLimit, ObservationCountLimit, UniqueWordLimit,
	ConflictingWord, MaterializationFailed
};

struct SrtReadCaptureCounters {
	uint64_t hooks = 0;
	uint64_t accepted_observations = 0;
	uint64_t ignored_after_invalidation = 0;
	uint64_t observed_bytes = 0;
	uint64_t unique_words = 0;
};

struct SrtReadCaptureObservation {
	SrtReadKind kind = SrtReadKind::Scalar;
	uint64_t address = 0;
	uint64_t requested_bytes = 0;
	bool succeeded = false;
	bool specialization_read = false;
	std::optional<SrtReadContext> context;
	std::optional<uint32_t> native_pc;
};

struct SrtReadCaptureFailure {
	uint64_t address = 0;
	uint64_t requested_bytes = 0;
	uint32_t previous_word = 0;
	uint32_t conflicting_word = 0;
	std::optional<SrtReadContext> context;
	std::optional<uint32_t> native_pc;
};

// Pure observer: it never reads guest memory, calls the original reader, writes
// files, changes returned words, or caches a runtime/userdata pointer.
class SrtReadCapture {
public:
	using CaptureIdentity = SrtReadCaptureIdentity;
	using CaptureLimits = SrtReadCaptureLimits;
	using WordMap = std::map<uint64_t, uint32_t>;

	explicit SrtReadCapture(CaptureIdentity identity, CaptureLimits limits = {});
	static void Observe(void* userdata, const SrtReadObservation& observation);
	void Finalize(bool materialization_succeeded);

	[[nodiscard]] const CaptureIdentity& Identity() const { return m_identity; }
	[[nodiscard]] const CaptureLimits& Limits() const { return m_limits; }
	[[nodiscard]] SrtReadCaptureStatus Status() const { return m_status; }
	[[nodiscard]] bool Complete() const { return m_finalized && m_status == SrtReadCaptureStatus::Complete; }
	[[nodiscard]] bool Finalized() const { return m_finalized; }
	[[nodiscard]] const WordMap& Words() const { return m_words; }
	[[nodiscard]] const std::vector<SrtReadCaptureObservation>& Observations() const { return m_observations; }
	[[nodiscard]] const SrtReadCaptureCounters& Counters() const { return m_counters; }
	[[nodiscard]] const SrtReadCaptureFailure& Failure() const { return m_failure; }
	[[nodiscard]] static const char* StatusName(SrtReadCaptureStatus status);

private:
	void Record(const SrtReadObservation& observation);
	void Fail(SrtReadCaptureStatus status, const SrtReadObservation& observation,
	          uint64_t address, uint32_t previous = 0, uint32_t conflicting = 0);
	CaptureIdentity m_identity;
	CaptureLimits m_limits;
	SrtReadCaptureStatus m_status = SrtReadCaptureStatus::Collecting;
	bool m_finalized = false;
	WordMap m_words;
	std::vector<SrtReadCaptureObservation> m_observations;
	SrtReadCaptureCounters m_counters;
	SrtReadCaptureFailure m_failure;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR
