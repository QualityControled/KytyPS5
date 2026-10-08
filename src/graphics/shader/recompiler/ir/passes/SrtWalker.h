#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <span>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, std::span<uint32_t> values);

// A context is one complete native function-table record. Code and auxiliary words retain their
// original bits; records sharing code remain separate when their auxiliary pointers differ.
struct ExternalCallContextRecord {
	uint32_t                ordinal     = 0;
	uint32_t                function_id = 0;
	std::array<uint32_t, 4> words {};
};

struct ExternalCallContextDomain {
	uint32_t                                   domain_id = 0;
	bool                                       complete  = false;
	std::span<const ExternalCallContextRecord> records;
};

struct SrtExternalContext {
	uint32_t                         domain_id = 0;
	const ExternalCallContextRecord* record    = nullptr;
};

struct DescriptorBindingLimits {
	uint32_t sampled_images  = UINT32_MAX;
	uint32_t storage_images  = UINT32_MAX;
	uint32_t samplers        = UINT32_MAX;
	uint32_t storage_buffers = UINT32_MAX;
	uint32_t total_resources = UINT32_MAX;
};

// Diagnostic observations are made only after an existing read. Failure events
// carry a byte count but no potentially uninitialized returned words.
enum class SrtReadKind : uint8_t { Scalar, ScalarBuffer, VectorBuffer, ScalarTable };

struct SrtReadContext {
	uint32_t domain_id = 0;
	uint32_t function_id = 0;
	uint32_t record_ordinal = 0;
	std::array<uint32_t, 4> record_words {};
	bool operator==(const SrtReadContext&) const = default;
};

struct SrtReadObservation {
	SrtReadKind kind = SrtReadKind::Scalar;
	uint64_t address = 0;
	uint64_t requested_bytes = 0;
	bool succeeded = false;
	bool specialization_read = false;
	std::span<const uint32_t> words;
	std::optional<SrtReadContext> context;
	// Relative caller PC only for a caller-only probe and a nonzero explicit
	// MemoryFlags.pc. Other/zero/synthetic relocated locations remain unknown.
	std::optional<uint32_t> native_pc;
};

using SrtPostReadObserver = void (*)(void*, const SrtReadObservation&);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	std::span<const uint32_t> workgroup_counts;
	std::span<const ExternalCallContextDomain> external_context_domains;
	DescriptorBindingLimits                    descriptor_limits;
	// Independent from reader userdata so nested read wrappers preserve the observer.
	SrtPostReadObserver                         post_read_observer = nullptr;
	void*                                      post_read_userdata = nullptr;
	bool                                       post_read_clean = false;
};

inline void ObserveSrtRead(const SrtRuntime& runtime, SrtReadKind kind, uint64_t address,
                           uint64_t requested_bytes, std::span<const uint32_t> words,
                           bool succeeded, std::optional<SrtReadContext> context = {},
                           bool specialization_read = false,
                           std::optional<uint32_t> native_pc = {}) {
	if (runtime.post_read_observer == nullptr) return;
	runtime.post_read_observer(runtime.post_read_userdata,
	    {kind, address, requested_bytes, succeeded, specialization_read,
	     succeeded ? words : std::span<const uint32_t> {},
	     context, native_pc});
}

enum class RuntimeValueType { Any, Integer };

bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType                          type = RuntimeValueType::Any,
                          std::optional<ExternalCallContextBinding> external_context = {});
// Uses the strict reader for values that affect shader specialization.
SrtRuntime CleanRuntime(SrtRuntime runtime);

// One memoized evaluation session shared by the entire shader resource refresh.
class SrtWalker {
public:
	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
	          std::span<const uint8_t> clean_flat_slots = {}, SrtWalker* clean_evaluator = nullptr,
	          Value active_mask = {}, std::optional<SrtExternalContext> external_context = {});
	~SrtWalker();
	SrtWalker(const SrtWalker&)            = delete;
	SrtWalker& operator=(const SrtWalker&) = delete;

	bool Evaluate(Value value, uint32_t& result);
	bool EvaluateDescriptor(uint32_t source, DescriptorValue& result);
	// Refreshes reachable scalar reads and active descriptor sources in one walk.
	bool RefreshFlatBuffer(std::vector<uint32_t>& flat);

private:
	static ResourcePlan::EvaluationContext& AcquireContext(const ResourcePlan& program);
	static float Float32(uint64_t bits);
	bool EvaluateWide(Value value, uint64_t& result);
	bool Arg(const Inst& inst, size_t index, uint64_t& result);
	bool EvaluatePhi(const Inst& inst, uint64_t& result);
	bool EvaluateExtract(const Inst& inst, uint64_t& result);
	bool EvaluateRawRead(const Inst& inst, uint64_t& result);
	bool EvaluateInst(const Inst& inst, uint64_t& result);

	const ResourcePlan&              m_program;
	SrtRuntime                      m_runtime;
	std::span<const uint8_t>         m_clean_flat_slots;
	SrtWalker*                      m_clean_evaluator = nullptr;
	Value                           m_active_mask;
	std::optional<SrtExternalContext> m_external_context;
	ResourcePlan::EvaluationContext& m_context;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
