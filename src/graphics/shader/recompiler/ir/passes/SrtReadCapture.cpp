#include "graphics/shader/recompiler/ir/passes/SrtReadCapture.h"

#include <limits>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {

SrtReadCapture::SrtReadCapture(CaptureIdentity identity, CaptureLimits limits)
    : m_identity(std::move(identity)), m_limits(limits) {
	// Supplied limits can narrow the documented bounds, never silently widen them.
	if (m_limits.unique_bytes > 256u * 1024u) m_limits.unique_bytes = 256u * 1024u;
	if (m_limits.observation_bytes > 1024u * 1024u) m_limits.observation_bytes = 1024u * 1024u;
	if (m_limits.observations > 65536u) m_limits.observations = 65536u;
	if (m_identity.stage == ShaderType::Unknown || m_identity.shader_base == 0u || m_identity.pass_id == 0u ||
	    m_identity.user_data.empty() || m_identity.control_schema.empty() ||
	    m_identity.control_words.empty() || m_identity.caller_identity.empty() ||
	    m_identity.library_identity.empty() || m_identity.input_identity.empty())
		m_status = SrtReadCaptureStatus::MissingIdentity;
}

void SrtReadCapture::Observe(void* userdata, const SrtReadObservation& observation) {
	if (userdata != nullptr) static_cast<SrtReadCapture*>(userdata)->Record(observation);
}

void SrtReadCapture::Fail(SrtReadCaptureStatus status, const SrtReadObservation& observation,
                          uint64_t address, uint32_t previous, uint32_t conflicting) {
	m_status = status;
	m_failure = {address, observation.requested_bytes, previous, conflicting,
	             observation.context, observation.native_pc};
}

void SrtReadCapture::Record(const SrtReadObservation& observation) {
	if (m_finalized) return; // A published snapshot and its counters are immutable.
	if (m_counters.hooks != UINT64_MAX) ++m_counters.hooks;
	if (m_status != SrtReadCaptureStatus::Collecting) {
		if (m_counters.ignored_after_invalidation != UINT64_MAX)
			++m_counters.ignored_after_invalidation;
		return; // In particular, do not inspect the returned span again.
	}
	const auto bytes = observation.requested_bytes;
	if (bytes == 0u || (bytes & 3u) != 0u || (observation.address & 3u) != 0u ||
	    bytes > UINT64_MAX - observation.address ||
	    (observation.succeeded ? observation.words.data() == nullptr || observation.words.size() != bytes / sizeof(uint32_t)
	                           : !observation.words.empty())) {
		Fail(SrtReadCaptureStatus::InvalidSpan, observation, observation.address);
		return;
	}
	if (m_counters.accepted_observations >= m_limits.observations) {
		Fail(SrtReadCaptureStatus::ObservationCountLimit, observation, observation.address);
		return;
	}
	if (bytes > m_limits.observation_bytes - m_counters.observed_bytes) {
		Fail(SrtReadCaptureStatus::ObservationByteLimit, observation, observation.address);
		return;
	}
	// Reserve the observation budget even when the underlying reader failed.
	m_counters.observed_bytes += bytes;
	m_observations.push_back({observation.kind, observation.address, bytes,
	                          observation.succeeded, observation.specialization_read,
	                          observation.context, observation.native_pc});
	++m_counters.accepted_observations;
	if (!observation.succeeded) {
		Fail(SrtReadCaptureStatus::ReadFailed, observation, observation.address);
		return;
	}
	uint64_t new_words = 0;
	for (size_t i = 0; i < observation.words.size(); ++i) {
		const auto address = observation.address + i * uint64_t {sizeof(uint32_t)};
		const auto previous = m_words.find(address);
		if (previous == m_words.end()) {
			++new_words;
		} else if (previous->second != observation.words[i]) {
			Fail(SrtReadCaptureStatus::ConflictingWord, observation, address,
			     previous->second, observation.words[i]);
			return;
		}
	}
	if (new_words > m_limits.unique_bytes / sizeof(uint32_t) - m_counters.unique_words) {
		Fail(SrtReadCaptureStatus::UniqueWordLimit, observation, observation.address);
		return;
	}
	for (size_t i = 0; i < observation.words.size(); ++i)
		m_words.emplace(observation.address + i * uint64_t {sizeof(uint32_t)}, observation.words[i]);
	m_counters.unique_words += new_words;
}

void SrtReadCapture::Finalize(bool materialization_succeeded) {
	if (m_finalized) return;
	if (m_status == SrtReadCaptureStatus::Collecting)
		m_status = materialization_succeeded ? SrtReadCaptureStatus::Complete
		                                    : SrtReadCaptureStatus::MaterializationFailed;
	m_finalized = true;
}

const char* SrtReadCapture::StatusName(SrtReadCaptureStatus status) {
	switch (status) {
		case SrtReadCaptureStatus::Collecting: return "collecting";
		case SrtReadCaptureStatus::Complete: return "complete";
		case SrtReadCaptureStatus::MissingIdentity: return "missing-identity";
		case SrtReadCaptureStatus::ReadFailed: return "read-failed";
		case SrtReadCaptureStatus::InvalidSpan: return "invalid-span";
		case SrtReadCaptureStatus::ObservationByteLimit: return "observation-byte-limit";
		case SrtReadCaptureStatus::ObservationCountLimit: return "observation-count-limit";
		case SrtReadCaptureStatus::UniqueWordLimit: return "unique-word-limit";
		case SrtReadCaptureStatus::ConflictingWord: return "conflicting-word";
		case SrtReadCaptureStatus::MaterializationFailed: return "materialization-failed";
	}
	return "unknown-status";
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
