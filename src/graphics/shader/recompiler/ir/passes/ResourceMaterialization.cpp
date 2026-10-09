#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/SoftwareColorComparison.h"
#include "graphics/shader/recompiler/frontend/decode/ImageOps.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask            = 0x0000ffffffffffffull;
constexpr uint64_t MaxIndirectDescriptorProbes = 65536u;
constexpr uint64_t MaxExternalDescriptorReadBytes  = 64u * 1024u * 1024u;
constexpr uint64_t MaxExternalDescriptorCandidates = 1u << 20u;

struct ExternalReadTrace {
	SrtMemoryReader reader           = nullptr;
	void*           userdata         = nullptr;
	uint64_t*       total_requested  = nullptr;
	uint64_t        failed_address   = 0;
	size_t          failed_dwords    = 0;
	bool            failed           = false;
	bool            budget_exhausted = false;

	static bool Read(void* userdata, uint64_t address, std::span<uint32_t> values) {
		auto&      trace = *static_cast<ExternalReadTrace*>(userdata);
		const auto bytes = static_cast<uint64_t>(values.size()) * sizeof(uint32_t);
		if (bytes > MaxExternalDescriptorReadBytes - *trace.total_requested) {
			trace.budget_exhausted = true;
		} else {
			*trace.total_requested += bytes;
			if (trace.reader != nullptr && trace.reader(trace.userdata, address, values))
				return true;
		}
		trace.failed         = true;
		trace.failed_address = address;
		trace.failed_dwords  = values.size();
		return false;
	}
};

struct ExternalDescriptorCandidate {
	uint32_t        key;
	DescriptorValue descriptor;
};

using ExternalDescriptorCandidates = std::vector<std::vector<ExternalDescriptorCandidate>>;

bool SpecializationFail(std::string_view message) {
	std::fprintf(stderr, "shader resource specialization failed: %.*s\n",
	             static_cast<int>(message.size()), message.data());
	return false;
}

Decoder::ImageDimension DescriptorDimension(const DescriptorValue&  descriptor,
                                            Decoder::ImageDimension requested) {
	const bool is_array = requested == Decoder::ImageDimension::Dim1DArray ||
	                      requested == Decoder::ImageDimension::Dim2DArray ||
	                      requested == Decoder::ImageDimension::Dim2DMsaaArray;
	switch (static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu)) {
		case Prospero::ImageType::kColor1D: return Decoder::ImageDimension::Dim1D;
		case Prospero::ImageType::kColor1DArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim1DArray;
			}
			return Decoder::ImageDimension::Dim1D;
		case Prospero::ImageType::kColor3D: return Decoder::ImageDimension::Dim3D;
		case Prospero::ImageType::kCube: return Decoder::ImageDimension::Dim2DArray;
		case Prospero::ImageType::kColor2DArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim2DArray;
			}
			return Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2DMsaaArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim2DMsaaArray;
			}
			return Decoder::ImageDimension::Dim2DMsaa;
		case Prospero::ImageType::kColor2D: return Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2DMsaa: return Decoder::ImageDimension::Dim2DMsaa;
		default: return Decoder::ImageDimension::Unknown;
	}
}

bool NullImageDescriptor(const DescriptorValue& descriptor) {
	return descriptor.dwords[0] == 0 && (descriptor.dwords[1] & 0xffu) == 0;
}

bool ValidImageDescriptor(const DescriptorValue& descriptor, bool r128 = false) {
	const auto& words = descriptor.dwords;
	// Reject texture descriptors with nonzero reserved bits.
	if ((words[1] & 0x20000000u) != 0u || (words[2] & 0x70003000u) != 0u ||
	    (!r128 && ((words[4] & 0xe000e000u) != 0u || (words[5] & 0xf9000000u) != 0u ||
	               (words[6] & 0x00007b00u) != 0u))) {
		return false;
	}
	const auto type   = static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu);
	const auto format = static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	if (type < Prospero::ImageType::kColor1D || format == Prospero::BufferFormat::kInvalid ||
	    format > Prospero::BufferFormat::kBc7Srgb) {
		return false;
	}
	if (r128 && type != Prospero::ImageType::kColor1D && type != Prospero::ImageType::kColor2D &&
	    type != Prospero::ImageType::kColor2DMsaa) {
		return false;
	}
	const bool array = type == Prospero::ImageType::kColor1DArray ||
	                   type == Prospero::ImageType::kColor2DArray ||
	                   type == Prospero::ImageType::kColor2DMsaaArray ||
	                   type == Prospero::ImageType::kCube;
	if (array && ((words[4] >> 16u) & 0x1fffu) > (words[4] & 0x1fffu)) {
		return false;
	}
	if (type == Prospero::ImageType::kColor2DMsaa ||
	    type == Prospero::ImageType::kColor2DMsaaArray) {
		const auto base_level = (descriptor.dwords[3] >> 12u) & 0xfu;
		const auto fragments  = (descriptor.dwords[3] >> 16u) & 0xfu;
		const auto max_mip    = (descriptor.dwords[5] >> 4u) & 0xfu;
		return base_level == 0 && fragments >= 1 && fragments <= 3 &&
		       (r128 || max_mip == fragments);
	}
	return true;
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

Prospero::BufferFormat ImageConversionFormat(Prospero::BufferFormat format) {
	return Prospero::RemapTextureFormat(format) != format ? format
	                                                      : Prospero::BufferFormat::kInvalid;
}

enum class SamplerClass : uint8_t { Float, Integer, PointInteger, FloatComparison };

SamplerClass ClassifySampler(const ImageResource& image, bool separate_compare = false) {
	if (separate_compare && image.numeric_class == Prospero::TextureNumericClass::Float &&
	    image.depth_compare && image.comparison_mode == ImageComparisonMode::Native)
		return SamplerClass::FloatComparison;
	if (image.numeric_class == Prospero::TextureNumericClass::Sint ||
	    (image.numeric_class == Prospero::TextureNumericClass::Uint &&
	     image.conversion_format != Prospero::BufferFormat::kInvalid)) {
		return SamplerClass::PointInteger;
	}
	return image.numeric_class == Prospero::TextureNumericClass::Uint ? SamplerClass::Integer
	                                                               : SamplerClass::Float;
}

bool DescriptorIsCube(const DescriptorValue& descriptor) {
	return static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu) ==
	       Prospero::ImageType::kCube;
}

uint32_t ImageMipCount(const ImageResource& image, const DescriptorValue& descriptor) {
	if (image.mip_mode != ImageMipMode::Dynamic || NullImageDescriptor(descriptor)) {
		return 1;
	}
	const auto base = (descriptor.dwords[3] >> 12u) & 0xfu;
	const auto last = (descriptor.dwords[3] >> 16u) & 0xfu;
	return base <= last ? last - base + 1u : 0u;
}

bool DecodeBufferDescriptor(const DescriptorValue& descriptor, ShaderBufferResource& result) {
	if (descriptor.dword_count != std::size(result.fields)) {
		return false;
	}
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return true;
}

struct ReadCapture {
	SrtRuntime                                  source;
	std::vector<std::pair<uint64_t, uint64_t>>& ranges;
};

bool CaptureStrictRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<ReadCapture*>(userdata);
	if (!capture.source.read_specialization_memory(capture.source.userdata, address, values)) {
		return false;
	}
	capture.ranges.emplace_back(address, values.size_bytes());
	return true;
}

bool CaptureOrdinaryRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<ReadCapture*>(userdata);
	if (capture.source.read_memory != nullptr) {
		if (!capture.source.read_memory(capture.source.userdata, address, values)) return false;
	} else {
		std::memcpy(values.data(), reinterpret_cast<const void*>(address), values.size_bytes());
	}
	capture.ranges.emplace_back(address, values.size_bytes());
	return true;
}

const DescriptorSource* Source(const ResourcePlan& program, uint32_t source) {
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
}

void MarkCleanFlatSlots(const ResourcePlan& program, const DescriptorSource* source,
                        std::span<uint8_t> slots, Value extra = {}) {
	if (source == nullptr && extra.IsEmpty()) {
		return;
	}
	std::vector<Value>       pending;
	if (source != nullptr) {
		pending.assign(source->dwords.begin(), source->dwords.begin() + source->dword_count);
	}
	if (!extra.IsEmpty()) pending.push_back(extra);
	std::vector<const Inst*> visited;
	while (!pending.empty()) {
		auto value = pending.back().Resolve();
		pending.pop_back();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || std::ranges::find(visited, inst) != visited.end()) {
			continue;
		}
		visited.push_back(inst);
		if (inst->GetOpcode() == ValueOpcode::ReadConst) {
			const auto slot = inst->Arg(1).Resolve();
			if (slot.IsImmediate() && slot.GetType() == Type::U32 && slot.U32() < slots.size()) {
				slots[slot.U32()] = 1u;
				pending.push_back(program.srt_reads[slot.U32()].value);
			}
			continue;
		}
		for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
			pending.push_back(inst->Arg(arg));
		}
	}
}

bool ReadScalarTable(uint64_t base, uint64_t size, uint64_t dynamic_offset,
                     const SrtRuntime& runtime, std::span<uint32_t> words) {
	const auto offset = dynamic_offset & ~uint64_t {3};
	const auto count = std::min<uint64_t>(words.size(), offset < size ? (size - offset) / 4u : 0u);
	std::ranges::fill(words.subspan(count), 0u);
	if (count == 0u) {
		return true;
	}
	base &= AddressMask & ~uint64_t {3};
	if (offset > AddressMask - base) {
		return false;
	}
	const auto address = base + offset;
	const auto prefix = words.first(count);
	if (prefix.size_bytes() - 1u > AddressMask - address ||
	    runtime.read_specialization_memory == nullptr) return false;
	const bool succeeded = runtime.read_specialization_memory(runtime.userdata, address, prefix);
	ObserveSrtRead(runtime, SrtReadKind::ScalarTable, address, prefix.size_bytes(), prefix,
	               succeeded, {}, true);
	return succeeded;
}

bool NormalizeIndirectStoreBuffer(DescriptorValue& value) {
	ShaderBufferResource descriptor;
	if (!DecodeBufferDescriptor(value, descriptor)) return false;
	if (descriptor.Type() != 0u || descriptor.Format() == Prospero::BufferFormat::kInvalid ||
	    descriptor.Base48() == 0u || descriptor.GetSize() == 0u) {
		value.dwords.fill(0u);
	} else if (descriptor.Stride() != 1u || descriptor.SwizzleEnabled() || descriptor.AddTid() ||
	           descriptor.OutOfBounds() != 3u) {
		return SpecializationFail("indirect buffer stores require linear byte bounds");
	}
	return true;
}

bool IsBoundedDescriptorTable(const ResourcePlan& program,
                              const DescriptorSource::IndirectDescriptor& indirect) {
	const auto* table = Source(program, indirect.table_source);
	return indirect.sources.empty() && !indirect.selector &&
	       indirect.workgroup_axis == UINT32_MAX && table != nullptr && table->dword_count == 4u;
}

bool PrepareExternalDescriptors(const ResourcePlan& program, const SrtRuntime& runtime,
                                ExternalDescriptorCandidates& candidates,
                                ResourceSnapshot&             snapshot) {
	candidates.resize(program.descriptor_sources.size());
	snapshot.external_descriptor_candidate_count = 0u;
	snapshot.external_descriptor_read_bytes      = 0u;
	auto& total_requested                        = snapshot.external_descriptor_read_bytes;
	std::vector<ExternalCallContextBinding> bindings;
	for (const auto& source: program.descriptor_sources) {
		if (!source.indirect_descriptor || !source.indirect_descriptor->external_context) continue;
		const auto&                      context = *source.indirect_descriptor->external_context;
		const ExternalCallContextBinding binding {context.domain_id, context.function_id};
		if (context.expression_source >= program.descriptor_sources.size() ||
		    std::ranges::find(program.external_context_bindings, binding) ==
		        program.external_context_bindings.end())
			return SpecializationFail("external descriptor has an invalid context binding");
		if (std::ranges::find(bindings, binding) == bindings.end()) bindings.push_back(binding);
	}
	for (const auto& binding: bindings) {
		const ExternalCallContextDomain* domain = nullptr;
		for (const auto& candidate: runtime.external_context_domains) {
			if (candidate.domain_id != binding.domain_id) continue;
			if (domain != nullptr) return SpecializationFail("duplicate external context domain");
			domain = &candidate;
		}
		if (domain == nullptr || !domain->complete || domain->records.empty() ||
		    domain->records.size() > MaxIndirectDescriptorProbes)
			return SpecializationFail(
			    "external descriptor requires a complete bounded context domain");
		std::unordered_set<uint32_t> keys;
		for (const auto& record: domain->records) {
			if (!keys.insert(record.ordinal).second)
				return SpecializationFail("external context domain has duplicate record ordinals");
		}
		std::vector<uint32_t> expressions;
		for (const auto& source: program.descriptor_sources) {
			if (!source.indirect_descriptor || !source.indirect_descriptor->external_context)
				continue;
			const auto& context = *source.indirect_descriptor->external_context;
			if (context.domain_id != binding.domain_id ||
			    context.function_id != binding.function_id)
				continue;
			if (std::ranges::find(expressions, context.expression_source) == expressions.end())
				expressions.push_back(context.expression_source);
		}
		for (const auto expression: expressions) {
			const auto& source = program.descriptor_sources[expression];
			if (source.indirect_descriptor ||
			    (source.dword_count != 4u && source.dword_count != 8u))
				return SpecializationFail("external descriptor expression has an invalid shape");
			for (uint32_t word = 0; word < source.dword_count; ++word) {
				if (!ValidateRuntimeValue(program, source.dwords[word], RuntimeValueType::Integer,
				                          binding))
					return SpecializationFail(
					    "external descriptor expression is not valid in its context");
			}
		}
		bool found = false;
		for (const auto& record: domain->records) {
			if (record.function_id != binding.function_id) continue;
			found = true;
			// Share one evaluation memo across every descriptor chain in this native context.
			// The selected lane/record key is never evaluated on the host.
			auto              observed = CleanRuntime(runtime);
			ExternalReadTrace trace {observed.read_memory, observed.userdata, &total_requested};
			observed.read_memory                = ExternalReadTrace::Read;
			observed.read_specialization_memory = ExternalReadTrace::Read;
			observed.userdata                   = &trace;
			SrtWalker evaluator(program, observed, {}, nullptr, {},
			                    SrtExternalContext {binding.domain_id, &record});
			for (const auto expression: expressions) {
				if (snapshot.external_descriptor_candidate_count >= MaxExternalDescriptorCandidates)
					return SpecializationFail(fmt::format(
					    "hash=0x{:016x} external candidate capacity exhausted: domain={} "
					    "function={} "
					    "record={} source={} candidates={} tuple_bytes={} limit={} "
					    "requested_bytes={}",
					    program.shader_hash, binding.domain_id, binding.function_id, record.ordinal,
					    expression, snapshot.external_descriptor_candidate_count,
					    snapshot.external_descriptor_candidate_count *
					        sizeof(ExternalDescriptorCandidate),
					    MaxExternalDescriptorCandidates, total_requested));
				DescriptorValue descriptor;
				if (!evaluator.EvaluateDescriptor(expression, descriptor))
					return SpecializationFail(fmt::format(
					    "hash=0x{:016x} external descriptor failed: domain={} function={} "
					    "record={} "
					    "source={} code=0x{:016x} aux=0x{:016x} failed_read={} address=0x{:016x} "
					    "dwords={} requested_bytes={} budget_exhausted={}",
					    program.shader_hash, binding.domain_id, binding.function_id, record.ordinal,
					    expression,
					    uint64_t {record.words[0]} | (uint64_t {record.words[1]} << 32u),
					    uint64_t {record.words[2]} | (uint64_t {record.words[3]} << 32u),
					    trace.failed, trace.failed_address, trace.failed_dwords, total_requested,
					    trace.budget_exhausted));
				candidates[expression].push_back({record.ordinal, descriptor});
				++snapshot.external_descriptor_candidate_count;
			}
		}
		if (!found) return SpecializationFail("external context domain omits a linked function");
	}
	return true;
}

template <typename Specialization, typename Normalize>
bool MaterializeIndirectDescriptor(
    const ResourcePlan& program, const DescriptorSource::IndirectDescriptor& indirect,
    uint32_t resource_index, uint32_t dword_count, const SrtRuntime& runtime, SrtWalker& clean,
    const ExternalDescriptorCandidates& external_candidates, ResourceSnapshot& snapshot,
    std::vector<DescriptorValue>& descriptors, std::vector<Specialization>& specializations,
    uint32_t maximum_resources, Normalize&& normalize) {
	const auto  descriptor_bytes = dword_count * sizeof(uint32_t);
	const auto& sources = indirect.sources;
	const auto* selector = indirect.selector ? &*indirect.selector : nullptr;
	auto& keys = program.material_keys;
	const auto children_begin = descriptors.size();
	const auto root_resource = specializations[resource_index];
	const auto intern_candidate = [&](const DescriptorValue& candidate, uint32_t& ordinal) {
		ordinal = 0u;
		if (candidate == descriptors[resource_index]) return true;
		const auto found = std::find(descriptors.begin() + children_begin, descriptors.end(), candidate);
		ordinal = static_cast<uint32_t>(found - descriptors.begin() - children_begin + 1u);
		if (found == descriptors.end()) {
			if (descriptors.size() >= maximum_resources) return false;
			descriptors.push_back(candidate);
			auto child = root_resource;
			child.indirect_root = resource_index;
			specializations.push_back(child);
		}
		return true;
	};
	if (indirect.external_context) {
		const auto expression = indirect.external_context->expression_source;
		if (expression >= external_candidates.size() || external_candidates[expression].empty())
			return SpecializationFail("external descriptor candidates are unavailable");
		auto entries = external_candidates[expression];
		std::ranges::sort(entries, {}, &ExternalDescriptorCandidate::key);
		const auto mapping_offset = snapshot.flattened_srt.size();
		snapshot.flattened_srt.resize(mapping_offset + 1u + entries.size() * 2u);
		snapshot.flattened_srt[mapping_offset] = static_cast<uint32_t>(entries.size());
		// A key that does not belong to this function selects the typed null resource.
		descriptors[resource_index] = {.dword_count = dword_count};
		for (uint32_t entry = 0; entry < entries.size(); ++entry) {
			auto candidate = entries[entry].descriptor;
			if (candidate.dword_count != dword_count || !normalize(candidate)) return false;
			uint32_t ordinal = 0;
			if (!intern_candidate(candidate, ordinal)) return false;
			snapshot.flattened_srt[mapping_offset + 1u + entry * 2u] = entries[entry].key;
			snapshot.flattened_srt[mapping_offset + 2u + entry * 2u] = ordinal;
		}
		// Even a default sampler needs its complete key domain: an unknown context must
		// report a fault rather than silently succeeding with the identical null payload.
		auto& root                      = specializations[resource_index];
		root.indirect_root              = resource_index;
		root.indirect_mapping_offset    = static_cast<uint32_t>(mapping_offset);
		root.indirect_search_iterations = std::bit_width(entries.size());
		return true;
	}
	const auto read_keys = [&](const ShaderBufferResource& material, uint64_t first,
	                           uint64_t step, uint64_t count) {
		keys.resize(count);
		if (step == 4u) {
			return ReadScalarTable(material.Base48(), material.GetSize(), first, runtime, keys);
		}
		for (auto& key: keys) {
			if (!ReadScalarTable(material.Base48(), material.GetSize(), first, runtime, {&key, 1}))
				return false;
			first += step;
		}
		return true;
	};
	uint64_t table_base = 0;
	uint64_t table_size = UINT64_MAX; // Scalar addresses have no buffer descriptor bounds.
	if (sources.empty()) {
		keys.clear();
		DescriptorValue material_value;
		DescriptorValue table_value;
		if ((selector != nullptr &&
		     !clean.EvaluateDescriptor(selector->source, material_value)) ||
		    !clean.EvaluateDescriptor(indirect.table_source, table_value)) {
			return false;
		}
		ShaderBufferResource table;
		if (table_value.dword_count == 2u) {
			table_base = (static_cast<uint64_t>(table_value.dwords[1]) << 32u) | table_value.dwords[0];
		} else if (DecodeBufferDescriptor(table_value, table)) {
			table_base = table.Base48();
			table_size = table.GetSize();
		} else {
			return false;
		}
		if (IsBoundedDescriptorTable(program, indirect)) {
			if (!indirect.table_scalar && table.Format() == Prospero::BufferFormat::kInvalid) {
				descriptors[resource_index] = {.dword_count = dword_count};
				return true;
			}
			if (table_size > UINT32_MAX ||
			    (indirect.table_record_bytes != 0u &&
			     (table.Type() != 0u || table.SwizzleEnabled() || table.AddTid() ||
			      table.OutOfBounds() != 0u || (table_base & 3u) != 0u ||
			      (table.Stride() & 3u) != 0u || table.Stride() < indirect.table_record_bytes)))
				return false;
			// Descriptor bytes are refreshed together. The GPU selects the DWORD address;
			// no material selector or unused table field is interpreted on the CPU.
			keys.resize(table_size / 4u);
			if (!ReadScalarTable(table_base, table_size, 0u, runtime, keys)) return false;
			const auto mapping_offset = snapshot.flattened_srt.size();
			descriptors[resource_index] = {.dword_count = dword_count};
			snapshot.flattened_srt.resize(mapping_offset + 2u, 0u);
			for (uint32_t word = 0; word < keys.size(); ++word) {
				DescriptorValue candidate {.dword_count = dword_count};
				std::copy_n(keys.begin() + word, std::min<size_t>(dword_count, keys.size() - word),
				            candidate.dwords.begin());
				if (!normalize(candidate)) return false;
				uint32_t ordinal;
				if (!intern_candidate(candidate, ordinal)) return false;
				if (ordinal == 0u) continue;
				snapshot.flattened_srt.resize(mapping_offset + 2u + word, 0u);
				snapshot.flattened_srt[mapping_offset + 1u + word] = ordinal;
			}
			if (descriptors.size() == children_begin) {
				snapshot.flattened_srt.resize(mapping_offset);
			} else {
				snapshot.flattened_srt[mapping_offset] =
				    static_cast<uint32_t>(snapshot.flattened_srt.size() - mapping_offset - 1u);
				auto& root = specializations[resource_index];
				root.indirect_root = resource_index;
				root.indirect_mapping_offset = static_cast<uint32_t>(mapping_offset);
				root.indirect_search_iterations = 0u;
			}
			return true;
		}
		if (selector == nullptr) {
			uint32_t key_count = 0;
			if (indirect.workgroup_axis != UINT32_MAX) {
				if (indirect.workgroup_axis >= runtime.workgroup_counts.size()) return false;
				key_count = runtime.workgroup_counts[indirect.workgroup_axis];
				if (key_count == 0u) return false;
			} else {
				if (table_value.dword_count != 2u || !clean.Evaluate(indirect.key_count, key_count))
					return false;
				if (std::bit_cast<int32_t>(key_count) <= 0) key_count = 0;
			}
			if (indirect.table_stride == 0u || key_count > MaxIndirectDescriptorProbes ||
			    (key_count != 0u && uint64_t {indirect.table_offset} + indirect.table_immediate +
			         uint64_t {key_count - 1u} * indirect.table_stride + descriptor_bytes >
			             UINT32_MAX + 1ull)) {
				return false;
			}
			keys.resize(key_count);
			std::iota(keys.begin(), keys.end(), 0u);
		} else if (!indirect.selector_first.IsEmpty()) {
			ShaderBufferResource material;
			uint32_t             first = 0, count = 0;
			if (!DecodeBufferDescriptor(material_value, material) || material.Type() != 0u ||
			    material.SwizzleEnabled() ||
			    material.AddTid() || material.OutOfBounds() != 0u ||
			    uint64_t {selector->offset} + 4u > material.Stride() ||
			    !clean.Evaluate(indirect.selector_first, first) ||
			    !clean.Evaluate(indirect.key_count, count) || count > MaxIndirectDescriptorProbes ||
			    uint64_t {first} + count > material.NumRecords())
				return false;
			if (count != 0u && (uint64_t {first} + count - 1u) * material.Stride() +
			                           selector->offset + 4u >
			                       uint64_t {UINT32_MAX} + 1u)
				return false;
			if (!read_keys(material, uint64_t {first} * material.Stride() + selector->offset,
			               material.Stride(), count)) return false;
		} else if (!indirect.selector_mask.IsEmpty()) {
			uint32_t mask = 0;
			uint32_t count = 0;
			if (material_value.dword_count != 2u || table_value.dword_count != 2u ||
			    !clean.Evaluate(indirect.selector_mask, mask) ||
			    !clean.Evaluate(indirect.key_count, count) || count == 0u || count > 32u) {
				return false;
			}
			if (count < 32u) mask &= (1u << count) - 1u;
			const auto material_base =
			    (static_cast<uint64_t>(material_value.dwords[1]) << 32u) | material_value.dwords[0];
			keys.reserve(std::popcount(mask));
			while (mask != 0u) {
				const auto index = std::countr_zero(mask);
				const auto offset = static_cast<uint64_t>(selector->offset) +
				                    static_cast<uint64_t>(index) * selector->stride;
				if (offset > UINT32_MAX) return false;
				uint32_t key = 0;
				if (!ReadScalarTable(material_base, UINT64_MAX, static_cast<uint32_t>(offset),
				                     runtime, {&key, 1})) return false;
				keys.push_back(key);
				mask &= mask - 1u;
			}
		} else return false;
		if (selector != nullptr) {
			std::ranges::sort(keys);
			keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
		}
	}

	const auto mapping_offset = snapshot.flattened_srt.size();
	const auto key_count = sources.empty() ? keys.size() : sources.size();
	snapshot.flattened_srt.resize(mapping_offset + 1u + key_count * 2u);
	snapshot.flattened_srt[mapping_offset] = static_cast<uint32_t>(key_count);
	for (uint32_t entry = 0; entry < key_count; ++entry) {
		const auto key = sources.empty() ? keys[entry] : entry;
		DescriptorValue candidate;
		candidate.dword_count = dword_count;
		if (sources.empty()) {
			const uint64_t table_offset =
			    uint64_t {(key * indirect.table_stride + indirect.table_offset) & ~3u} +
			    (indirect.table_immediate & ~3u);
			if (!ReadScalarTable(table_base, table_size, table_offset, runtime,
			                     std::span(candidate.dwords).first(dword_count))) {
				return false;
			}
		} else if (!clean.EvaluateDescriptor(sources[entry], candidate)) {
			return false;
		}
		if (!normalize(candidate)) return false;
		uint32_t ordinal = 0;
		if (entry == 0) {
			descriptors[resource_index] = candidate;
		} else if (!intern_candidate(candidate, ordinal)) {
			return false;
		}
		snapshot.flattened_srt[mapping_offset + 1u + entry * 2u] = key;
		snapshot.flattened_srt[mapping_offset + 2u + entry * 2u] = ordinal;
	}
	if (descriptors.size() == children_begin) {
		snapshot.flattened_srt.resize(mapping_offset);
	} else {
		auto& root                      = specializations[resource_index];
		root.indirect_root              = resource_index;
		root.indirect_mapping_offset = static_cast<uint32_t>(mapping_offset);
		root.indirect_search_iterations = std::bit_width(key_count);
	}
	return true;
}

} // namespace

struct SamplerPlan {
	struct Binding {
		uint32_t     source;
		SamplerClass type;
	};
	std::vector<std::array<uint32_t, 4>>                      mapping;
	std::vector<Binding>                                      bindings;
	uint32_t                                                  sampler_count = 0;
	bool                                                      separate_compare = false;
};

static bool BuildBindingAliases(const ResourcePlan& program, const ResourceSnapshot& snapshot,
                                ResourceSpecialization& specialization);

template <typename T, typename Keep>
void CompactImages(std::vector<T>& images, Keep&& keep) {
	size_t count = 0;
	for (size_t index = 0; index < images.size(); ++index) {
		if (!keep(index)) continue;
		if (count != index) images[count] = std::move(images[index]);
		++count;
	}
	images.resize(count);
}

struct ImageRemap {
	explicit ImageRemap(const ResourceSpecialization& specialization)
	    : indices(specialization.images.size()),
	      source_count(static_cast<uint32_t>(specialization.images.size())) {
		for (uint32_t index = 0; index < source_count; index++) {
			indices[index] = specialization.images[index].fmask ? UINT32_MAX : count++;
		}
	}

	uint32_t operator[](uint32_t index) const {
		EXIT_IF(index >= source_count);
		return indices[index];
	}

	template <typename T>
	void Apply(std::vector<T>& images) const {
		EXIT_IF(images.size() != source_count);
		if (count == source_count) {
			return;
		}
		CompactImages(images, [&](size_t index) { return indices[index] != UINT32_MAX; });
	}

private:
	std::vector<uint32_t> indices;
	uint32_t                                    source_count;
	uint32_t                                    count = 0;
};

static bool BuildResourceSpecialization(const ResourcePlan& program, ResourceSnapshot& snapshot,
                                        ResourceSpecialization& specialization) {
	for (uint32_t i = 0; i < specialization.images.size(); i++) {
		const auto& descriptor = snapshot.images[i];
		auto&       image      = specialization.images[i];
		const auto  base_index = i < program.info.images.size() ? i : image.indirect_root;
		if (base_index >= program.info.images.size()) {
			return SpecializationFail(fmt::format("image resource {} has an invalid root", i));
		}
		const auto& base = program.info.images[base_index];
		if (base.resource_class == ImageResourceClass::None ||
		    (base.atomic && base.resource_class != ImageResourceClass::Storage)) {
			return SpecializationFail(fmt::format("image resource {} has an invalid class", i));
		}
		image.mip_count = ImageMipCount(base, descriptor);
		if (image.mip_count == 0u) {
			return SpecializationFail(
			    fmt::format("image descriptor {} has an invalid mip range", i));
		}
		if (NullImageDescriptor(descriptor)) {
			image.numeric_class = base.atomic ? Prospero::TextureNumericClass::Uint
			                      : base.numeric_class != Prospero::TextureNumericClass::Unsupported
			                          ? base.numeric_class
			                          : Prospero::TextureNumericClass::Float;
			image.dimension     = base.dimension != Decoder::ImageDimension::Unknown
			                          ? base.dimension
			                          : Decoder::ImageDimension::Dim2D;
			image.cube          = base.cube;
			continue;
		}
		const auto descriptor_dimension = DescriptorDimension(descriptor, base.dimension);
		if (descriptor_dimension == Decoder::ImageDimension::Unknown) {
			return SpecializationFail(fmt::format(
			    "image descriptor {} has unsupported type {}: {:08x},{:08x},{:08x},{:08x},"
			    "{:08x},{:08x},{:08x},{:08x}",
			    i, (descriptor.dwords[3] >> 28u) & 0xfu, descriptor.dwords[0], descriptor.dwords[1],
			    descriptor.dwords[2], descriptor.dwords[3], descriptor.dwords[4],
			    descriptor.dwords[5], descriptor.dwords[6], descriptor.dwords[7]));
		}
		image.dimension = descriptor_dimension;
		image.cube      = DescriptorIsCube(descriptor);
		const auto format =
		    static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
		if (base.atomic &&
		    (base.atomic64 ? format != Prospero::BufferFormat::k32_32UInt
		                   : format != Prospero::BufferFormat::k32UInt &&
		                         format != Prospero::BufferFormat::k32Float)) {
			return SpecializationFail(
			    fmt::format("atomic image descriptor {} uses unsupported format {}", i,
			                static_cast<uint32_t>(format)));
		}
		const bool storage      = base.resource_class == ImageResourceClass::Storage;
		image.fmask             = Prospero::IsFmaskTextureFormat(format);
		if (image.fmask) {
			if (storage || base.depth_compare ||
			    image.indirect_root != ImageResource::NoIndirectImage ||
			    std::ranges::any_of(program.info.sampled_pairs,
			                        [&](const auto& pair) { return pair.image == i; })) {
				return SpecializationFail("FMASK requires a direct image load");
			}
		}
		image.conversion_format = ImageConversionFormat(format);
		if (storage || image.conversion_format != Prospero::BufferFormat::kInvalid) {
			image.shader_swizzle = DescriptorImageSwizzle(descriptor);
		}
		const bool raw_sint_storage = storage && format == Prospero::BufferFormat::k32SInt &&
		                              base.written && !base.read && !base.atomic;
		image.numeric_class         = Prospero::SampledTextureNumericClass(format);
		if (storage) {
			if ((!raw_sint_storage && image.numeric_class == Prospero::TextureNumericClass::Sint) ||
			    image.numeric_class == Prospero::TextureNumericClass::Unsupported) {
				return SpecializationFail(
				    fmt::format("storage image descriptor {} uses unsupported format {}", i,
				                static_cast<uint32_t>(format)));
			}
			if (raw_sint_storage || base.atomic) {
				image.numeric_class = Prospero::TextureNumericClass::Uint;
			}
		} else if (image.numeric_class == Prospero::TextureNumericClass::Unsupported ||
		           (base.depth_compare &&
		            image.numeric_class != Prospero::TextureNumericClass::Float)) {
			return SpecializationFail(
			    fmt::format("sampled image descriptor {} uses unsupported format {}", i,
			                static_cast<uint32_t>(format)));
		}
	}
	for (uint32_t root_index = 0; root_index < specialization.images.size(); root_index++) {
		auto& root = specialization.images[root_index];
		if (root.indirect_root != root_index) {
			continue;
		}
		const auto key_count = root.indirect_mapping_offset < snapshot.flattened_srt.size()
		                           ? snapshot.flattened_srt[root.indirect_mapping_offset]
		                           : 0u;
		if (key_count == 0u ||
		    static_cast<size_t>(root.indirect_mapping_offset) + 1u +
		            static_cast<size_t>(key_count) * (root.indirect_search_iterations == 0u ? 1u : 2u) >
		        snapshot.flattened_srt.size()) {
			return SpecializationFail("indirect image specialization has an invalid key mapping");
		}
		uint32_t exemplar       = ImageResource::NoIndirectImage;
		uint32_t resource_count = 0;
		for (uint32_t resource = 0; resource < specialization.images.size(); resource++) {
			if (specialization.images[resource].indirect_root != root_index) {
				continue;
			}
			resource_count++;
			if (exemplar == ImageResource::NoIndirectImage &&
			    !NullImageDescriptor(snapshot.images[resource])) {
				exemplar = resource;
			}
		}
		if (exemplar == ImageResource::NoIndirectImage && resource_count == 1u) {
			const auto& base   = program.info.images[root_index];
			const auto* source = Source(program, base.source);
			if (source != nullptr && source->indirect_descriptor &&
			    source->indirect_descriptor->external_context) {
				if (base.numeric_class != Prospero::TextureNumericClass::Unsupported &&
				    base.dimension != Decoder::ImageDimension::Unknown)
					continue;
				const auto& context = *source->indirect_descriptor->external_context;
				return SpecializationFail(
				    fmt::format("hash=0x{:016x} all-null external image lacks a proven type: "
					            "root={} pc=0x{:08x} "
					            "domain={} function={} source={} numeric_class={} dimension={}",
					            program.shader_hash, root_index, base.first_use_pc,
					            context.domain_id, context.function_id, context.expression_source,
					            static_cast<uint32_t>(base.numeric_class),
					            static_cast<uint32_t>(base.dimension)));
			}
		}
		if (resource_count < 2u || exemplar == ImageResource::NoIndirectImage) {
			return SpecializationFail("indirect image specialization has no typed candidate");
		}
		const auto& image_class = specialization.images[exemplar];
		const auto is_2d = [](Decoder::ImageDimension dimension) {
			return dimension == Decoder::ImageDimension::Dim2D ||
			       dimension == Decoder::ImageDimension::Dim2DArray;
		};
		for (uint32_t candidate = 0; candidate < specialization.images.size(); candidate++) {
			auto& image = specialization.images[candidate];
			if (image.indirect_root != root_index) {
				continue;
			}
			if (NullImageDescriptor(snapshot.images[candidate])) {
				image.numeric_class     = image_class.numeric_class;
				image.dimension         = image_class.dimension;
				image.mip_count         = image_class.mip_count;
				image.conversion_format = image_class.conversion_format;
				image.shader_swizzle    = image_class.shader_swizzle;
				image.cube              = image_class.cube;
			}
			const bool same_coordinates = image.dimension == image_class.dimension &&
			                              image.cube == image_class.cube;
			if (image.numeric_class != image_class.numeric_class ||
			    (!same_coordinates && !(is_2d(image.dimension) && is_2d(image_class.dimension))) ||
			    image.mip_count != image_class.mip_count ||
			    image.conversion_format != image_class.conversion_format ||
			    image.shader_swizzle != image_class.shader_swizzle) {
				return SpecializationFail(
				    fmt::format("indirect image table at pc 0x{:08x} has incompatible candidates",
				                program.info.images[root_index].first_use_pc));
			}
		}
	}
	for (uint32_t sampler = 0u; sampler < specialization.samplers.size(); ++sampler) {
		const auto& descriptor = snapshot.samplers.at(sampler);
		if (descriptor.dword_count != 4u) return SpecializationFail("invalid original sampler coordinate descriptor");
		// Only indirect descriptors need this module-affecting candidate fact. Direct
		// descriptors keep bit15 dynamic in executable original S# DWORD0.
		specialization.samplers[sampler].force_unnormalized_coordinates =
		    specialization.samplers[sampler].indirect_root != SamplerResource::NoIndirectSampler &&
		    (descriptor.dwords[0] & (1u << 15u)) != 0u;
	}
	// Cube UNORM conversion is not established. Reject any potentially paired
	// ForceUnorm sampler/cube candidate before binding, including indirect children.
	// This is intentionally conservative when independent indirect sets are paired.
	for (const auto& pair: program.info.sampled_pairs) {
		for (uint32_t image = 0u; image < specialization.images.size(); ++image) {
			const auto& candidate = specialization.images[image];
			if (image != pair.image && candidate.indirect_root != pair.image) continue;
			if (!candidate.cube) continue;
			for (uint32_t sampler = 0u; sampler < specialization.samplers.size(); ++sampler) {
				if (sampler != pair.sampler && specialization.samplers[sampler].indirect_root != pair.sampler) continue;
				if ((snapshot.samplers.at(sampler).dwords[0] & (1u << 15u)) != 0u)
					return SpecializationFail("unnormalized cube sampler is outside the supported coordinate scope");
			}
		}
	}
	if (!BuildBindingAliases(program, snapshot, specialization)) return false;
	CompactImages(snapshot.images, [&](size_t index) { return !specialization.images[index].fmask; });
	return true;
}

bool BuildSamplerPlan(const ShaderInfo& base, SamplerPlan& plan, bool external_contexts, bool separate_compare = false) {
	plan.separate_compare = separate_compare;
	if (!external_contexts && base.samplers.size() > ShaderInfo::MaxSamplers) return false;
	plan.mapping.resize(base.samplers.size());
	plan.bindings.resize(base.samplers.size());
	std::vector<uint8_t> usage(base.samplers.size());
	plan.sampler_count = static_cast<uint32_t>(base.samplers.size());
	for (const auto& pair: base.sampled_pairs) {
		if (pair.image >= base.images.size() || pair.sampler >= base.samplers.size()) {
			return false;
		}
		usage[pair.sampler] |= 1u << static_cast<uint32_t>(ClassifySampler(base.images[pair.image], separate_compare));
	}
	for (uint32_t index = 0; index < base.samplers.size(); ++index) {
		const auto root = base.samplers[index].indirect_root;
		if (root == SamplerResource::NoIndirectSampler || root == index) continue;
		if (root >= base.samplers.size()) return false;
		usage[index] = usage[root];
	}
	for (uint32_t index = 0; index < base.samplers.size(); index++) {
		auto& mapping = plan.mapping[index];
		mapping.fill(UINT32_MAX);
		const auto classes = usage[index] == 0u ? 1u : usage[index];
		bool       first   = true;
		for (uint32_t type = 0; type < mapping.size(); type++) {
			if ((classes & (1u << type)) == 0u) continue;
			const auto target = first ? index : plan.sampler_count++;
			if (!external_contexts && target >= ShaderInfo::MaxSamplers) return false;
			if (target >= plan.bindings.size()) plan.bindings.resize(target + 1u);
			mapping[type]         = target;
			plan.bindings[target] = {index, static_cast<SamplerClass>(type)};
			first                = false;
		}
	}
	return true;
}

static void ApplySamplerVariants(ShaderInfo& info, const SamplerPlan& plan) {
	auto&      samplers       = info.samplers;
	const auto original_count = samplers.size();
	samplers.reserve(plan.sampler_count);
	for (uint32_t index = 0; index < plan.sampler_count; ++index) {
		const auto& binding = plan.bindings[index];
		if (index >= original_count) samplers.push_back(samplers[binding.source]);
		auto& sampler                 = samplers[index];
		sampler.snapshot_index        = binding.source;
		sampler.force_point_filtering = binding.type == SamplerClass::PointInteger;
		sampler.integer_border        = binding.type == SamplerClass::Integer ||
		                                binding.type == SamplerClass::PointInteger;
		if (plan.separate_compare) sampler.depth_compare = binding.type == SamplerClass::FloatComparison;
	}
	for (uint32_t index = 0; index < plan.sampler_count; ++index) {
		auto& sampler = samplers[index];
		if (sampler.indirect_root == SamplerResource::NoIndirectSampler) continue;
		const auto type       = static_cast<uint32_t>(plan.bindings[index].type);
		sampler.indirect_root = plan.mapping[sampler.indirect_root][type];
		EXIT_IF(sampler.indirect_root == UINT32_MAX);
		for (auto& resource: sampler.indirect_resources) {
			resource = plan.mapping[resource][type];
			EXIT_IF(resource == UINT32_MAX);
		}
	}
	for (auto& pair: info.sampled_pairs) {
		const auto type = static_cast<uint32_t>(ClassifySampler(info.images[pair.image], plan.separate_compare));
		pair.sampler    = plan.mapping[pair.sampler][type];
		EXIT_IF(pair.sampler == UINT32_MAX);
		if (!plan.separate_compare)
			samplers[pair.sampler].depth_compare |= info.images[pair.image].depth_compare;
	}
	for (auto& sampler: samplers) {
		if (sampler.indirect_root != SamplerResource::NoIndirectSampler)
			sampler.depth_compare |= samplers[sampler.indirect_root].depth_compare;
	}
}

static bool BuildBindingAliases(const ResourcePlan& program, const ResourceSnapshot& snapshot,
                                ResourceSpecialization& specialization) {
	specialization.sampler_binding_aliases.clear();
	for (auto& image: specialization.images)
		image.binding_alias = UINT32_MAX;
	if (program.external_context_bindings.empty()) return true;
	ShaderInfo prospective        = program.info;
	const auto native_image_count = prospective.images.size();
	for (uint32_t index = 0; index < specialization.images.size(); ++index) {
		const auto& source = specialization.images[index];
		if (index >= native_image_count) {
			if (source.indirect_root >= native_image_count) return false;
			prospective.images.push_back(prospective.images[source.indirect_root]);
		}
		auto& image             = prospective.images[index];
		image.numeric_class     = source.numeric_class;
		image.dimension         = source.dimension;
		image.mip_count         = source.mip_count;
		image.conversion_format = source.conversion_format;
		image.shader_swizzle    = source.shader_swizzle;
		image.comparison_mode   = source.comparison_mode;
		image.cube              = source.cube;
	}
	const auto native_sampler_count = prospective.samplers.size();
	for (uint32_t index = 0; index < specialization.samplers.size(); ++index) {
		const auto& source = specialization.samplers[index];
		if (index >= native_sampler_count) {
			if (source.indirect_root >= native_sampler_count) return false;
			prospective.samplers.push_back(prospective.samplers[source.indirect_root]);
		}
		auto& sampler         = prospective.samplers[index];
		sampler.indirect_root = source.indirect_root;
		sampler.indirect_resources.clear();
	}
	for (uint32_t index = 0; index < prospective.samplers.size(); ++index) {
		const auto root = prospective.samplers[index].indirect_root;
		if (root != SamplerResource::NoIndirectSampler) {
			if (root >= prospective.samplers.size()) return false;
			prospective.samplers[root].indirect_resources.push_back(index);
		}
	}
	SamplerPlan plan;
	if (!BuildSamplerPlan(prospective, plan, true, specialization.software_color_dref_enabled)) return false;
	ApplySamplerVariants(prospective, plan);

	// Exact descriptor payload and view/class metadata determine binding equivalence. The
	// record-key maps and provenance remain independent, even when their bindings coincide.
	struct KeyHash {
		size_t operator()(const std::vector<uint32_t>& words) const {
			size_t hash = 1469598103934665603ull;
			for (const auto word: words)
				hash = (hash ^ word) * 1099511628211ull;
			return hash;
		}
	};
	std::unordered_map<std::vector<uint32_t>, uint32_t, KeyHash> images, samplers;
	const auto DescriptorKey = [](const DescriptorValue& descriptor) {
		std::vector<uint32_t> key {descriptor.dword_count};
		key.insert(key.end(), descriptor.dwords.begin(), descriptor.dwords.end());
		return key;
	};
	for (uint32_t index = 0; index < prospective.images.size(); ++index) {
		if (specialization.images[index].fmask) continue;
		const auto& image = prospective.images[index];
		auto        key   = DescriptorKey(snapshot.images[index]);
		key.insert(key.end(),
		           {static_cast<uint32_t>(image.resource_class),
		            static_cast<uint32_t>(image.numeric_class),
		            static_cast<uint32_t>(image.dimension), static_cast<uint32_t>(image.mip_mode),
		            image.mip_count, static_cast<uint32_t>(image.conversion_format),
		            image.shader_swizzle, image.read, image.written, image.atomic, image.atomic64,
		            image.depth_compare, image.cube, image.r128, static_cast<uint32_t>(image.comparison_mode)});
		const auto [it, inserted] = images.emplace(std::move(key), index);
		if (!inserted) specialization.images[index].binding_alias = it->second;
	}
	specialization.sampler_binding_aliases.assign(prospective.samplers.size(), UINT32_MAX);
	for (uint32_t index = 0; index < prospective.samplers.size(); ++index) {
		const auto& sampler = prospective.samplers[index];
		if (sampler.snapshot_index >= snapshot.samplers.size()) return false;
		auto key = DescriptorKey(snapshot.samplers[sampler.snapshot_index]);
		key.insert(key.end(), {sampler.force_point_filtering, sampler.depth_compare,
		                       sampler.integer_border, sampler.gather_lod});
		const auto [it, inserted] = samplers.emplace(std::move(key), index);
		if (!inserted) specialization.sampler_binding_aliases[index] = it->second;
	}
	return true;
}

template <typename Predicate>
static std::vector<ResourceBlock> ResourceControlFlow(const Program& program, const Predicate& predicate) {
	// The renderer checks captured scalar reads against final buffer and image write ranges.
	if (program.blocks.size() != program.block_info.size() || program.has_address_writes) {
		return {};
	}
	std::unordered_map<uint32_t, uint32_t> indices;
	for (uint32_t i = 0; i < program.block_info.size(); i++) {
		if (!indices.emplace(program.block_info[i].id, i).second) {
			return {};
		}
	}
	std::vector<ResourceBlock> blocks(program.blocks.size());
	for (uint32_t i = 0; i < blocks.size(); i++) {
		auto&                 block      = blocks[i];
		const auto&           info       = program.block_info[i];
		const auto&           terminator = info.terminator;
		std::vector<uint32_t> successors;
		switch (terminator.kind) {
			case CFG::TerminatorKind::Branch: successors.push_back(terminator.true_block); break;
			case CFG::TerminatorKind::ConditionalBranch:
				successors = {terminator.true_block, terminator.false_block};
				block.condition = info.condition;
				break;
			case CFG::TerminatorKind::IndirectBranch:
				successors = terminator.indirect_targets;
				break;
			case CFG::TerminatorKind::Return: break;
			default: return {};
		}
		for (const auto successor: successors) {
			const auto found = indices.find(successor);
			if (found == indices.end()) {
				return {};
			}
			block.successors.push_back(found->second);
		}
		for (const auto& inst: *program.blocks[i]) {
			const auto op     = inst.GetOpcode();
			if (op == ValueOpcode::ReadConst) {
				block.srt_reads.push_back(inst.Arg(1).Resolve().U32());
				continue;
			}
			const auto buffer = BufferAccessOf(op);
			const auto image  = ImageOpcodeInfoOf(op);
			if (buffer == BufferAccess::None && image.access == ImageAccess::None) {
				continue;
			}
			const auto& memory = program.memory_info.at(inst.Flags<MemoryFlags>().index);
			if (memory.planning_only || memory.kind == ResourceKind::IndirectBuffer) {
				continue;
			}
			if (buffer != BufferAccess::None) {
				block.sources.push_back(program.info.buffers.at(memory.resource).source);
			} else {
				block.sources.push_back(program.info.images.at(memory.resource).source);
				if (image.needs_sampler) {
					block.sources.push_back(program.info.samplers.at(memory.sampler).source);
				}
			}
		}
		std::ranges::sort(block.sources);
		block.sources.erase(std::unique(block.sources.begin(), block.sources.end()),
		                    block.sources.end());
	}
	for (auto& block: blocks) block.condition = predicate(block.condition);
	if (std::ranges::none_of(
	        blocks, [](const ResourceBlock& block) { return !block.condition.IsEmpty(); })) {
		return {};
	}
	return blocks;
}

// ResolveComputeBufferFill proves complete workgroups and matching requested extents, so every
// dispatch-bound predicate accepted here is true for every invocation covered by the fill.
static bool IsFullDispatchPredicate(Value value, uint32_t depth = 0) {
	value = value.Resolve();
	if (value == Value(true)) return true;
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || depth > 32) return false;
	if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
		return IsFullDispatchPredicate(inst->Arg(0), depth + 1) &&
		       IsFullDispatchPredicate(inst->Arg(1), depth + 1);
	}
	if (inst->GetOpcode() != ValueOpcode::ULessThan32) return false;
	const auto* global = inst->Arg(0).Resolve().TryInstruction();
	const auto* extent = inst->Arg(1).Resolve().TryInstruction();
	if (global == nullptr || global->GetOpcode() != ValueOpcode::GetBuiltin ||
	    global->Arg(0).Resolve() != Value(static_cast<uint32_t>(StageInputKind::GlobalInvocationId)) ||
	    extent == nullptr || extent->GetOpcode() != ValueOpcode::GetDispatchThreadExtent)
		return false;
	const auto axis = global->Arg(1).Resolve();
	return axis.IsImmediate() && axis.U32() < 3 && extent->Arg(0).Resolve() == axis;
}

// Nonnegative affine coefficients for constant, local and workgroup coordinates. Reject modular
// arithmetic that could wrap; runtime coverage also bounds the largest invocation index.
static std::optional<std::array<uint64_t, 3>> FillIndex(Value value, uint32_t axis, Value guard,
                                                      uint32_t depth = 0) {
	value = ResolveActiveU32(value, guard);
	if (depth > 32 || value.GetType() != Type::U32) {
		return {};
	}
	if (value.IsImmediate()) {
		return std::array<uint64_t, 3> {value.U32(), 0, 0};
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return {};
	}
	const auto op = inst->GetOpcode();
	if (op == ValueOpcode::GetBuiltin && inst->Arg(1) == Value(axis)) {
		if (inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId))) {
			return std::array<uint64_t, 3> {0, 1, 0};
		}
		if (inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::WorkgroupId))) {
			return std::array<uint64_t, 3> {0, 0, 1};
		}
	}
	if (op != ValueOpcode::IAdd32 && op != ValueOpcode::IMul32 &&
	    op != ValueOpcode::ShiftLeftLogical32) {
		return {};
	}
	auto left  = FillIndex(inst->Arg(0), axis, guard, depth + 1);
	auto right = FillIndex(inst->Arg(1), axis, guard, depth + 1);
	if (!left || !right) {
		return {};
	}
	if (op == ValueOpcode::IMul32 && ((*right)[1] != 0 || (*right)[2] != 0)) {
		std::swap(left, right);
	}
	if (op != ValueOpcode::IAdd32 && ((*right)[1] != 0 || (*right)[2] != 0)) {
		return {};
	}
	if (op == ValueOpcode::ShiftLeftLogical32) {
		if ((*right)[0] >= 32) return {};
		(*right)[0] = uint64_t {1} << (*right)[0];
	}
	for (uint32_t i = 0; i < left->size(); ++i) {
		(*left)[i] =
		    op == ValueOpcode::IAdd32 ? (*left)[i] + (*right)[i] : (*left)[i] * (*right)[0];
		if ((*left)[i] > UINT32_MAX) return {};
	}
	return left;
}

static UniformFillPlan AnalyzeUniformFill(const Program& program) {
	if (program.stage != ShaderType::Compute || program.blocks.empty() ||
	    program.blocks.size() != program.block_info.size() || program.info.uses_dma ||
	    !program.info.samplers.empty()) {
		return {};
	}
	std::unordered_set<uint32_t> visited;
	uint32_t                     index = 0;
	const Inst*                  store = nullptr;
	for (;;) {
		if (!visited.insert(index).second) return {};
		for (const auto& inst: *program.blocks[index]) {
			if (AddressOpcodeInfoOf(inst.GetOpcode()).access != AddressAccess::None) return {};
			if (!inst.MayHaveSideEffects()) continue;
			if (store != nullptr || (BufferAccessOf(inst.GetOpcode()) != BufferAccess::Write &&
			                         inst.GetOpcode() != ValueOpcode::ImageWrite))
				return {};
			store = &inst;
		}
		const auto& term = program.block_info[index].terminator;
		if (term.kind == CFG::TerminatorKind::Return) break;
		if (term.kind != CFG::TerminatorKind::Branch) return {};
		const auto next = std::ranges::find(program.block_info, term.true_block, &BlockInfo::id);
		if (next == program.block_info.end()) return {};
		index = static_cast<uint32_t>(next - program.block_info.begin());
	}
	if (store == nullptr || visited.size() != program.blocks.size()) return {};
	for (const auto& buffer: program.info.buffers) {
		if (buffer.read && (!buffer.scalar || buffer.written)) return {};
	}
	const auto& memory = program.memory_info.at(store->Flags<MemoryFlags>().index);
	UniformFillPlan result;
	result.fill.resource = memory.resource;
	Value data;
	Value guard(true);
	if (store->GetOpcode() == ValueOpcode::ImageWrite) {
		if (program.info.images.size() != 1 || memory.dmask != 1 || memory.data_bits != 32 ||
		    memory.image_has_mip || memory.image_sample_flags != 0 || memory.image_r128 ||
		    memory.image_dimension != Decoder::ImageDimension::Dim2DArray ||
		    store->Arg(3).Resolve() != Value(true)) return {};
		const auto& image = program.info.images[memory.resource];
		if (image.read || image.atomic || image.mip_mode != ImageMipMode::None) return {};
		const auto* address = store->Arg(1).ResolveInstruction();
		if (address == nullptr || address->GetOpcode() != ValueOpcode::MakeImageAddress) return {};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const auto index = FillIndex(address->Arg(axis), axis, guard);
			if (!index || (*index)[0] != 0) return {};
			if (axis < 2) {
				if ((*index)[1] != 1 || (*index)[2] == 0) return {};
			} else if ((*index)[1] != 0 || (*index)[2] != 1) {
				return {};
			}
			result.fill.group_stride[axis] = static_cast<uint32_t>((*index)[2]);
		}
		const auto* values = store->Arg(2).ResolveInstruction();
		if (values == nullptr || values->GetOpcode() != ValueOpcode::CompositeConstructU32x4)
			return {};
		result.fill.kind  = UniformFillKind::Image;
		result.fill.words = 1;
		data = values->Arg(0);
	} else {
		if (!program.info.images.empty()) return {};
		const auto           op = store->GetOpcode();
		constexpr std::array stores {ValueOpcode::StoreBufferU32, ValueOpcode::StoreBufferU32x2,
		                             ValueOpcode::StoreBufferU32x3, ValueOpcode::StoreBufferU32x4};
		const auto           store_op = std::ranges::find(stores, op);
		if (store_op == stores.end() || store->Arg(2).Resolve() != Value(0u) ||
		    store->Arg(3).Resolve() != Value(0u))
			return {};
		guard = store->Arg(5).Resolve();
		if (!IsFullDispatchPredicate(guard)) return {};
		if (!memory.formatted || memory.typed || !memory.idxen || memory.offen || memory.offset != 0 ||
		    memory.data_bits != 32 ||
		    memory.data_dwords != static_cast<uint32_t>(store_op - stores.begin() + 1))
			return {};
		const auto address = FillIndex(store->Arg(1), 0, guard);
		if (!address || (*address)[0] != 0 || (*address)[1] != 1 || (*address)[2] == 0) return {};
		result.fill.kind = UniformFillKind::Buffer;
		result.fill.group_stride[0] = static_cast<uint32_t>((*address)[2]);
		result.fill.words = memory.data_dwords;
		data = store->Arg(4);
	}
	data = ResolveActiveU32(data, guard);
	const auto*          vector = data.TryInstruction();
	constexpr std::array composites {ValueOpcode::CompositeConstructU32x2,
	                                 ValueOpcode::CompositeConstructU32x3,
	                                 ValueOpcode::CompositeConstructU32x4};
	if (result.fill.words > 1 &&
	    (vector == nullptr || vector->GetOpcode() != composites[result.fill.words - 2]))
		return {};
	for (uint32_t i = 0; i < result.fill.words; ++i) {
		const auto word = ResolveActiveU32(result.fill.words == 1 ? data : vector->Arg(i), guard);
		if (word.GetType() != Type::U32 ||
		    !ValidateRuntimeValue(program, word, RuntimeValueType::Integer))
			return {};
		result.values[i] = word;
	}
	return result;
}

ResourcePlan ExtractResourcePlan(const Program& program) {
	ResourcePlan plan;
	plan.stage                      = program.stage;
	plan.shader_hash                = program.shader_hash;
	plan.user_data_base             = program.user_data_base;
	plan.user_data_count            = program.user_data_count;
	plan.external_context_bindings  = program.external_context_bindings;
	plan.info                       = program.info;
	plan.memory_info                = program.memory_info;
	plan.srt_plan_complete          = program.srt_plan_complete;
	plan.resource_tracking_complete = program.resource_tracking_complete;

	// Only executable consumers count. Dead raw MemoryInfo rows can retain untracked IDs.
	std::vector<bool> seen(plan.info.images.size(), false);
	std::vector<bool> eligible(plan.info.images.size(), true);
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (ImageOpcodeInfoOf(inst.GetOpcode()).access == ImageAccess::None) continue;
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= program.memory_info.size()) continue; // Existing specialization still checks invalid live metadata.
			const auto& memory = program.memory_info[index];
			if (memory.resource >= eligible.size()) continue;
			seen[memory.resource] = true;
			constexpr uint32_t allowed = Decoder::ImageSampleFlagCompare |
			                            Decoder::ImageSampleFlagLevelZero |
			                            Decoder::ImageSampleFlagLod |
			                            Decoder::ImageSampleFlagBias |
			                            Decoder::ImageSampleFlagDerivative;
			eligible[memory.resource] = eligible[memory.resource] &&
			    inst.GetOpcode() == ValueOpcode::ImageSampleRaw && memory.data_bits == 32u &&
			    memory.dmask != 0u && memory.dmask <= 15u && !memory.image_sample_status &&
			    (memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0u &&
			    (memory.image_sample_flags & ~allowed) == 0u;
		}
	}
	for (size_t index = 0; index < eligible.size(); ++index) {
		plan.info.images[index].software_comparison_seen = seen[index];
		plan.info.images[index].software_comparison_eligible = seen[index] && eligible[index];
	}


	std::unordered_map<const Inst*, Inst*> cloned;
	std::function<Value(Value)>            Clone = [&](Value value) -> Value {
		value              = value.Resolve();
		const auto* source = value.TryInstruction();
		if (source == nullptr) {
			return value;
		}
		if (source->GetOpcode() == ValueOpcode::ReadFirstLane &&
		    ValidateRuntimeValue(program, source->Arg(0))) {
			// Uniform values need no new EXEC context; preserve their shared evaluation memo.
			return Clone(source->Arg(0));
		}
		if (source->GetOpcode() == ValueOpcode::Phi) {
			const auto invariant = ResolveInvariantPhi(program, value);
			if (!invariant.IsEmpty() && invariant != value) {
				return Clone(invariant);
			}
		}
		if (const auto found = cloned.find(source); found != cloned.end()) {
			return Value(found->second);
		}
		if (source->GetOpcode() == ValueOpcode::LoadBufferU32) {
			plan.requires_specialization_memory = true;
			plan.capture_specialization_reads   = true;
		}
		auto& target =
		    plan.value_storage.emplace_back(source->GetOpcode(), source->Flags<uint64_t>());
		cloned.emplace(source, &target);
		if (source->GetOpcode() == ValueOpcode::ExternalCallContextWord) {
			// This graph is host-only. Keep the context identity, while the actual selected
			// words and ordinal remain exclusively in executable GPU IR.
			for (uint32_t index = 0; index < source->NumArgs(); ++index)
				target.SetArg(index, index == 0u || index == 2u ? Value(0u) : source->Arg(index));
		} else if (source->GetOpcode() == ValueOpcode::Phi) {
			for (size_t index = 0; index < source->NumArgs(); index++) {
				target.AddPhiOperand(nullptr, Clone(source->Arg(index)));
			}
		} else {
			for (size_t index = 0; index < source->NumArgs(); index++) {
				target.SetArg(index, Clone(source->Arg(index)));
			}
		}
		return Value(&target);
	};

	plan.descriptor_sources.reserve(program.descriptor_sources.size());
	for (const auto& source: program.descriptor_sources) {
		auto& target          = plan.descriptor_sources.emplace_back();
		target.dword_count    = source.dword_count;
		target.indirect_descriptor = source.indirect_descriptor;
		if (target.indirect_descriptor.has_value()) {
			target.indirect_descriptor->key_count = Clone(target.indirect_descriptor->key_count);
			target.indirect_descriptor->selector_first =
			    Clone(target.indirect_descriptor->selector_first);
			target.indirect_descriptor->selector_mask =
			    Clone(target.indirect_descriptor->selector_mask);
		}
		for (uint32_t dword = 0; dword < source.dword_count; dword++) {
			target.dwords[dword] = Clone(source.dwords[dword]);
		}
	}
	plan.srt_reads.reserve(program.srt_reads.size());
	for (const auto& read: program.srt_reads) {
		plan.srt_reads.push_back({Clone(read.value), read.flat_offset});
	}
	// A proven uniform factor can decide a branch even when its other lanes are unknown.
	// Keep only that Boolean structure, never the varying shader dependency graph.
	Value unknown;
	std::function<Value(Value)> ClonePredicate = [&](Value value) -> Value {
		value = value.Resolve();
		if (value.IsEmpty()) return {};
		if (ValidateRuntimeValue(program, value, RuntimeValueType::Integer)) return Clone(value);
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return {};
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::ConditionRef || op == ValueOpcode::LogicalNot) {
			const auto operand = ClonePredicate(inst->Arg(0));
			if (operand.IsEmpty() || op == ValueOpcode::ConditionRef) return operand;
			auto& node = plan.value_storage.emplace_back(op);
			node.SetArg(0, operand);
			return Value(&node);
		}
		if (op != ValueOpcode::LogicalAnd && op != ValueOpcode::LogicalOr) return {};
		auto left = ClonePredicate(inst->Arg(0));
		auto right = ClonePredicate(inst->Arg(1));
		if (left.IsEmpty() && right.IsEmpty()) return {};
		if (left.IsEmpty() || right.IsEmpty()) {
			if (unknown.IsEmpty()) unknown = Value(&plan.value_storage.emplace_back(ValueOpcode::UndefU1));
			if (left.IsEmpty()) left = unknown;
			if (right.IsEmpty()) right = unknown;
		}
		auto& node = plan.value_storage.emplace_back(op);
		node.SetArg(0, left);
		node.SetArg(1, right);
		return Value(&node);
	};
	plan.control_flow = ResourceControlFlow(program, ClonePredicate);
	plan.uniform_fill = AnalyzeUniformFill(program);
	for (uint32_t i = 0; i < plan.uniform_fill.fill.words; ++i) {
		plan.uniform_fill.values[i] = Clone(plan.uniform_fill.values[i]);
	}
	plan.clean_flat_slots.resize(plan.srt_reads.size());
	bool capture_indirect_reads = false;
	for (const auto& source: plan.descriptor_sources) {
		if (!source.indirect_descriptor.has_value()) continue;
		const auto& indirect                = *source.indirect_descriptor;
		plan.requires_specialization_memory = true;
		capture_indirect_reads = true;
		if (indirect.selector)
			MarkCleanFlatSlots(plan, Source(plan, indirect.selector->source), plan.clean_flat_slots);
		MarkCleanFlatSlots(plan, nullptr, plan.clean_flat_slots, indirect.selector_mask);
		MarkCleanFlatSlots(plan, nullptr, plan.clean_flat_slots, indirect.selector_first);
		MarkCleanFlatSlots(plan, nullptr, plan.clean_flat_slots, indirect.key_count);
		if (indirect.external_context) {
			MarkCleanFlatSlots(plan, Source(plan, indirect.external_context->expression_source),
			                   plan.clean_flat_slots);
		} else if (indirect.sources.empty()) {
			MarkCleanFlatSlots(plan, Source(plan, indirect.table_source), plan.clean_flat_slots);
		} else {
			for (const auto candidate: indirect.sources) {
				MarkCleanFlatSlots(plan, Source(plan, candidate), plan.clean_flat_slots);
			}
		}
	}
	plan.capture_specialization_reads |= capture_indirect_reads || !plan.control_flow.empty();
	if (plan.capture_specialization_reads) {
		// Clean writable addresses prove that shader writes cannot overlap captured reads.
		for (const auto& buffer: plan.info.buffers) {
			if (buffer.written) MarkCleanFlatSlots(plan, Source(plan, buffer.source), plan.clean_flat_slots);
		}
		for (const auto& image: plan.info.images) {
			if (image.written) MarkCleanFlatSlots(plan, Source(plan, image.source), plan.clean_flat_slots);
		}
	}
	if (capture_indirect_reads) plan.resource_tracking_complete &= !program.has_address_writes;
	return plan;
}

bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization) {
	// Clear per-pass policy state even when this materialization fails before alias construction.
	specialization.software_color_dref_enabled = false;
	if (!program.resource_tracking_complete ||
	    (program.requires_specialization_memory && runtime.read_specialization_memory == nullptr)) {
		return false;
	}
	const bool capture_reads = program.capture_specialization_reads;
	auto& reads = snapshot.specialization_reads;
	reads.clear();
	ReadCapture capture {runtime, reads};
	SrtRuntime observed = runtime;
	if (capture_reads) {
		observed.userdata = &capture;
		observed.read_specialization_memory = runtime.read_specialization_memory != nullptr
		                                         ? CaptureStrictRead : nullptr;
		observed.read_memory = CaptureOrdinaryRead;
	}
	SrtWalker clean(program, CleanRuntime(observed));
	ExternalDescriptorCandidates external_candidates;
	if (!PrepareExternalDescriptors(program, observed, external_candidates, snapshot)) return false;
	specialization.dynamic_buffer_formats.clear();
	for (uint32_t index = 0; index < program.memory_info.size(); ++index) {
		const auto& memory = program.memory_info[index];
		if (memory.dynamic_descriptor_source == UINT32_MAX) continue;
		const auto* source = Source(program, memory.dynamic_descriptor_source);
		if (source == nullptr || !source->indirect_descriptor ||
		    !source->indirect_descriptor->external_context || !memory.formatted ||
		    memory.kind != ResourceKind::IndirectBuffer)
			return SpecializationFail("invalid finite-context buffer format provenance");
		const auto expression = source->indirect_descriptor->external_context->expression_source;
		if (expression >= external_candidates.size() || external_candidates[expression].empty())
			return SpecializationFail("finite-context buffer format candidates are unavailable");
		DynamicBufferFormatSet set {.memory_index = index};
		for (const auto& candidate: external_candidates[expression]) {
			ShaderBufferResource descriptor;
			if (!DecodeBufferDescriptor(candidate.descriptor, descriptor)) return false;
			set.formats.push_back(static_cast<uint32_t>(descriptor.Format()));
		}
		std::ranges::sort(set.formats);
		set.formats.erase(std::unique(set.formats.begin(), set.formats.end()), set.formats.end());
		specialization.dynamic_buffer_formats.push_back(std::move(set));
	}
	SrtWalker walker(program, observed, program.clean_flat_slots,
	                 capture_reads || program.requires_specialization_memory ? &clean : nullptr);
	if (!walker.RefreshFlatBuffer(snapshot.flattened_srt)) {
		return false;
	}
	const auto active = std::span<const uint8_t>(program.active_sources);
	snapshot.uniform_fill = {};
	const auto& fill = program.uniform_fill;
	const auto words = fill.fill.words;
	std::array<uint32_t, 4> stored {};
	bool uniform_fill = words != 0;
	for (uint32_t i = 0; i < words && uniform_fill; ++i) {
		uniform_fill = clean.Evaluate(fill.values[i], stored[i]) && stored[i] == stored[0];
	}
	if (uniform_fill) {
		snapshot.uniform_fill = fill.fill;
		snapshot.uniform_fill.value = stored[0];
	}
	const auto evaluate = [&](uint32_t source, DescriptorValue& value, bool written = false) {
		if (source >= program.descriptor_sources.size()) {
			return false;
		}
		if (active.empty() || active[source]) {
			return (capture_reads && written ? clean : walker).EvaluateDescriptor(source, value);
		}
		value = {};
		value.dword_count = program.descriptor_sources[source].dword_count;
		return true;
	};
	snapshot.buffers.resize(program.info.buffers.size());
	specialization.buffers.assign(program.info.buffers.size(), {});
	for (uint32_t i = 0; i < snapshot.buffers.size(); ++i) {
		const auto base_index =
		    i < program.info.buffers.size() ? i : specialization.buffers[i].indirect_root;
		if (base_index >= program.info.buffers.size()) return false;
		const auto& base = program.info.buffers[base_index];
		if (i < program.info.buffers.size()) {
			const auto* source = Source(program, base.source);
			if (source == nullptr) return false;
			if (source->indirect_descriptor.has_value()) {
				snapshot.buffers[i] = {.dword_count = 4u};
				if (active.empty() || active[base.source]) {
					if (!MaterializeIndirectDescriptor(
					        program, *source->indirect_descriptor, i, 4u, observed, clean,
					        external_candidates, snapshot, snapshot.buffers, specialization.buffers,
					        ShaderInfo::MaxBuffers, NormalizeIndirectStoreBuffer))
						return false;
				}
			} else if (!evaluate(base.source, snapshot.buffers[i], base.written)) {
				return false;
			}
		}
		auto&                descriptor_value = snapshot.buffers[i];
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(descriptor_value, descriptor)) {
			return SpecializationFail(fmt::format("buffer descriptor {} has invalid width", i));
		}
		if (descriptor.Type() != 0) {
			descriptor_value.dwords.fill(0);
			descriptor = {};
		}
		auto       packed_stride = descriptor.PackedStride();
		const auto stride        = packed_stride & 0x3fffu;
		const bool swizzle       = stride != 0u && ((packed_stride >> 14u) & 1u) != 0u;
		if (stride == 0u) {
			packed_stride &= ~((1u << 14u) | (3u << 16u));
		} else if (!swizzle) {
			packed_stride &= ~(3u << 16u);
		}
		auto& buffer         = specialization.buffers[i];
		buffer.packed_stride = packed_stride;
		buffer.descriptor_format =
		    base.formatted ? descriptor.Format() : Prospero::BufferFormat::kInvalid;
		buffer.descriptor_swizzle = base.formatted ? descriptor.DstSelXYZW() : DstSel(4, 5, 6, 7);
		buffer.zero_stride_oob    = descriptor.OutOfBounds() == 0u && stride == 0u;
	}
	snapshot.images.resize(program.info.images.size());
	specialization.images.resize(program.info.images.size());
	for (uint32_t i = 0; i < program.info.images.size(); ++i) {
		const auto& image = program.info.images[i];
		specialization.images[i] = {
		    .numeric_class = image.numeric_class,
		    .dimension = image.dimension,
		    .mip_count = image.mip_count,
		    .conversion_format = image.conversion_format,
		    .shader_swizzle = image.shader_swizzle,
		    .indirect_root = image.indirect_root,
		    .indirect_mapping_offset = image.indirect_mapping_offset,
		    .indirect_search_iterations = image.indirect_search_iterations,
		    .cube = image.cube,
		};
		const auto* source = Source(program, image.source);
		if (source == nullptr) {
			return false;
		}
		if (source->indirect_descriptor.has_value()) {
			snapshot.images[i] = {.dword_count = 8u};
			if (!active.empty() && !active[image.source]) {
				continue;
			}
			const auto& indirect = *source->indirect_descriptor;
			const bool bounded_table = IsBoundedDescriptorTable(program, indirect);
			if (!MaterializeIndirectDescriptor(
			        program, indirect, i, 8u, observed, clean, external_candidates, snapshot,
			        snapshot.images, specialization.images, UINT32_MAX,
			        [&](DescriptorValue& value) {
				        // A broad heap also contains resources for other typed image operations.
				        if (NullImageDescriptor(value) || !ValidImageDescriptor(value, image.r128) ||
				            (bounded_table && DescriptorDimension(value, image.dimension) != image.dimension))
					        value.dwords.fill(0u);
				        return true;
			        })) {
				return false;
			}
		} else {
			if (!evaluate(image.source, snapshot.images[i], image.written)) {
				return false;
			}
			if (!ValidImageDescriptor(snapshot.images[i], image.r128)) {
				snapshot.images[i].dwords.fill(0);
			}
		}
	}
	snapshot.samplers.resize(program.info.samplers.size());
	specialization.samplers.assign(program.info.samplers.size(), {});
	for (uint32_t i = 0; i < snapshot.samplers.size(); ++i) {
		const auto root =
		    i < program.info.samplers.size() ? i : specialization.samplers[i].indirect_root;
		if (root >= program.info.samplers.size()) return false;
		const auto& sampler = program.info.samplers[root];
		if (i < program.info.samplers.size()) {
			const auto* source = Source(program, sampler.source);
			if (source == nullptr) return false;
			if (source->indirect_descriptor) {
				if (!source->indirect_descriptor->external_context)
					return SpecializationFail("unsupported indirect sampler provenance");
				snapshot.samplers[i] = {.dword_count = 4u};
				if (!MaterializeIndirectDescriptor(
				        program, *source->indirect_descriptor, i, 4u, observed, clean,
				        external_candidates, snapshot, snapshot.samplers, specialization.samplers,
				        UINT32_MAX, [](DescriptorValue&) { return true; }))
					return false;
			} else if (!evaluate(sampler.source, snapshot.samplers[i]))
				return false;
		}
		if (sampler.gather_lod) {
			const auto control = snapshot.samplers[i].dwords[2];
			const auto filter = (control >> 26u) & 3u;
			// MipNone always selects the base level. Explicit point gathers currently require
			// encoded-zero primary and secondary bias; linear primary-mip selection is unsupported.
			if (filter > 1u || (filter == 1u && (control & 0xfffffu) != 0u)) {
				return SpecializationFail(
				    "explicit-LOD gather requires mip filtering None or Point with zero LOD biases");
			}
		}
	}
	snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	return BuildResourceSpecialization(program, snapshot, specialization);
}


bool ConfigureSoftwareColorComparison(const ResourcePlan& plan, const ResourceSnapshot& snapshot,
                                      ResourceSpecialization& specialization, bool enabled,
                                      bool r8_native_comparison_supported,
                                      SoftwareColorComparisonFailureReport* failure_report) {
	if (failure_report != nullptr) *failure_report = {};
	const auto reject = [&](SoftwareColorComparisonFailure reason, uint32_t image = UINT32_MAX,
	                        uint32_t sampler = UINT32_MAX, const SampledResourcePair* pair = nullptr,
	                        uint32_t scope_reason = UINT32_MAX) {
		if (failure_report != nullptr) {
			*failure_report = {reason, image, sampler, pair != nullptr ? pair->first_use_pc : 0u,
			                   pair != nullptr, scope_reason};
		}
		return false;
	};
	if (!enabled || r8_native_comparison_supported) return true;
	if (snapshot.images.size() != specialization.images.size()) return reject(SoftwareColorComparisonFailure::ImageCountMismatch);
	bool any = false;
	for (uint32_t index = 0; index < plan.info.images.size(); ++index) {
		const auto& image = plan.info.images[index];
		if (!image.depth_compare || !image.software_comparison_seen) continue;
		if (index >= snapshot.images.size() || snapshot.images[index].dword_count != 8u) return reject(SoftwareColorComparisonFailure::InvalidImageSnapshot, index);
		ShaderTextureResource descriptor;
		std::copy_n(snapshot.images[index].dwords.begin(), 8u, descriptor.fields);
		if (descriptor.Format() != Prospero::BufferFormat::k8UNorm) continue;
		const bool unsupported_texture = descriptor.IsNull() || descriptor.BaseLevel() != 0u || descriptor.LastLevel() != 0u ||
		    descriptor.MaxMip() != 0u || descriptor.MinLod() != 0u || descriptor.BaseArray5() != 0u ||
		    descriptor.PrtDefColor() || descriptor.CornerSample() || descriptor.MsaaDepth() || descriptor.MetaCompress();
		if (unsupported_texture || !image.software_comparison_eligible) return reject(
		    unsupported_texture ? SoftwareColorComparisonFailure::UnsupportedTextureFields
		                        : SoftwareColorComparisonFailure::UnsupportedLiveConsumer, index);
		for (uint32_t channel = 0; channel < 4u; ++channel) {
			const auto selector = (descriptor.DstSelXYZW() >> (channel * 3u)) & 7u;
			if (selector == 2u || selector == 3u) return reject(SoftwareColorComparisonFailure::ReservedSwizzle, index);
		}
		SoftwareColorComparison::Scope scope;
		scope.enabled = scope.capability_known = scope.comparison = true;
		scope.host_format = 9u; // k8UNorm maps exactly to VK_FORMAT_R8_UNORM.
		scope.dimensions = specialization.images[index].dimension == Decoder::ImageDimension::Dim2D ? 2u : 0u;
		scope.samples = 1u;
		scope.mip_levels = specialization.images[index].mip_count;
		scope.direct = image.indirect_root == ImageResource::NoIndirectImage;
		scope.sampled = image.resource_class == ImageResourceClass::Sampled;
		scope.read_only = image.read && !image.written && !image.atomic;
		scope.converted = specialization.images[index].conversion_format != Prospero::BufferFormat::kInvalid;
		scope.width = descriptor.Width5() + 1u;
		scope.height = descriptor.Height5() + 1u;
		bool paired = false;
		std::optional<SoftwareColorComparison::Mode> selected_mode;
		for (const auto& pair: plan.info.sampled_pairs) {
			if (pair.image != index) continue;
			if (pair.sampler >= plan.info.samplers.size() || pair.sampler >= snapshot.samplers.size() ||
			    plan.info.samplers[pair.sampler].indirect_root != SamplerResource::NoIndirectSampler ||
			    snapshot.samplers[pair.sampler].dword_count != 4u) return reject(SoftwareColorComparisonFailure::InvalidSamplerSnapshot, index, pair.sampler, &pair);
			std::copy_n(snapshot.samplers[pair.sampler].dwords.begin(), 4u, scope.sampler.begin());
			const auto decision = SoftwareColorComparison::Classify(scope);
			if (decision.mode != SoftwareColorComparison::Mode::SoftwarePointR8 &&
			    decision.mode != SoftwareColorComparison::Mode::SoftwareLinearR8) return reject(SoftwareColorComparisonFailure::RejectedScope, index, pair.sampler, &pair, static_cast<uint32_t>(decision.reason));
			if (selected_mode && *selected_mode != decision.mode) return reject(SoftwareColorComparisonFailure::ConflictingSamplerModes, index, pair.sampler, &pair);
			selected_mode = decision.mode;
			paired = true;
		}
		if (!paired) return reject(SoftwareColorComparisonFailure::MissingSampledPair, index);
		auto& specialized = specialization.images[index];
		specialized.comparison_mode = *selected_mode == SoftwareColorComparison::Mode::SoftwareLinearR8
		    ? ImageComparisonMode::SoftwareLinearR8 : ImageComparisonMode::SoftwarePointR8;
		specialized.shader_swizzle = descriptor.DstSelXYZW();
		any = true;
	}
	if (!any) return true;
	specialization.software_color_dref_enabled = true;
	if (!BuildBindingAliases(plan, snapshot, specialization)) return reject(SoftwareColorComparisonFailure::BindingAliases);
	return true;
}

void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization) {
	EXIT_IF(!program.resource_tracking_complete || program.shader_info_complete ||
	        program.binding_layout_complete);
	EXIT_IF(program.info.buffers.size() > specialization.buffers.size() ||
	        program.info.images.size() > specialization.images.size());
	program.dynamic_buffer_formats = specialization.dynamic_buffer_formats;

	auto& buffers = program.info.buffers;
	const auto original_buffer_count = buffers.size();
	buffers.reserve(specialization.buffers.size());
	for (uint32_t index = 0; index < specialization.buffers.size(); ++index) {
		const auto& source = specialization.buffers[index];
		if (index >= buffers.size()) {
			EXIT_IF(source.indirect_root >= original_buffer_count);
			buffers.push_back(buffers[source.indirect_root]);
		}
		auto& buffer                      = buffers[index];
		buffer.packed_stride              = source.packed_stride;
		buffer.descriptor_format          = source.descriptor_format;
		buffer.descriptor_swizzle         = source.descriptor_swizzle;
		buffer.indirect_root              = source.indirect_root;
		buffer.indirect_mapping_offset    = source.indirect_mapping_offset;
		buffer.indirect_search_iterations = source.indirect_search_iterations;
		buffer.indirect_resources.clear();
	}
	for (uint32_t index = 0; index < buffers.size(); ++index) {
		const auto root = buffers[index].indirect_root;
		if (root != BufferResource::NoIndirectBuffer) {
			EXIT_IF(root >= buffers.size());
			buffers[root].indirect_resources.push_back(index);
		}
	}
	auto& images = program.info.images;
	const auto original_image_count = images.size();
	images.reserve(specialization.images.size());
	for (uint32_t index = 0; index < specialization.images.size(); index++) {
		const auto& source = specialization.images[index];
		if (index >= images.size()) {
			EXIT_IF(source.indirect_root >= original_image_count);
			images.push_back(images[source.indirect_root]);
		}
		auto& image                      = images[index];
		image.numeric_class              = source.numeric_class;
		image.dimension                  = source.dimension;
		image.mip_count                  = source.mip_count;
		image.conversion_format          = source.conversion_format;
		image.shader_swizzle             = source.shader_swizzle;
		image.comparison_mode            = source.comparison_mode;
		image.indirect_root              = source.indirect_root;
		image.indirect_mapping_offset    = source.indirect_mapping_offset;
		image.indirect_search_iterations = source.indirect_search_iterations;
		image.cube                       = source.cube;
		image.binding_alias              = source.binding_alias;
		image.indirect_resources.clear();
	}
	for (uint32_t index = 0; index < images.size(); index++) {
		const auto root = images[index].indirect_root;
		if (root != ImageResource::NoIndirectImage) {
			EXIT_IF(root >= images.size());
			images[root].indirect_resources.push_back(index);
		}
	}

	auto& samplers      = program.info.samplers;
	auto& sampled_pairs = program.info.sampled_pairs;
	const auto native_sampler_count = samplers.size();
	for (uint32_t index = 0; index < specialization.samplers.size(); ++index) {
		const auto& source = specialization.samplers[index];
		if (index >= native_sampler_count) {
			EXIT_IF(source.indirect_root >= native_sampler_count);
			samplers.push_back(samplers[source.indirect_root]);
		}
		auto& sampler                      = samplers[index];
		sampler.snapshot_index             = index;
		sampler.force_unnormalized_coordinates = source.force_unnormalized_coordinates;
		sampler.indirect_root              = source.indirect_root;
		sampler.indirect_mapping_offset    = source.indirect_mapping_offset;
		sampler.indirect_search_iterations = source.indirect_search_iterations;
		sampler.binding_alias              = UINT32_MAX;
		sampler.indirect_resources.clear();
	}
	for (uint32_t index = 0; index < samplers.size(); ++index) {
		const auto root = samplers[index].indirect_root;
		if (root != SamplerResource::NoIndirectSampler) {
			EXIT_IF(root >= samplers.size());
			samplers[root].indirect_resources.push_back(index);
		}
	}
	SamplerPlan sampler_plan;
	EXIT_IF(
	    !BuildSamplerPlan(program.info, sampler_plan, !program.external_context_bindings.empty(),
	                      specialization.software_color_dref_enabled));
	const auto original_sampler_count = samplers.size();
	ApplySamplerVariants(program.info, sampler_plan);
	if (!specialization.sampler_binding_aliases.empty()) {
		EXIT_IF(specialization.sampler_binding_aliases.size() != samplers.size());
		for (uint32_t index = 0; index < samplers.size(); ++index)
			samplers[index].binding_alias = specialization.sampler_binding_aliases[index];
	}

	auto& memory_info = program.memory_info;
	// DCE can leave native image metadata without an executable consumer. Only
	// surviving image operations have had their raw resource IDs tracked.
	std::vector<bool> active_image_memory(memory_info.size(), false);
	const ImageRemap image_remap(specialization);
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
			if (inst.GetOpcode() == ValueOpcode::GetImageResource) {
				inst.SetFlags(image_remap[inst.Flags<uint32_t>()]);
				continue;
			}
			if (BufferAccessOf(inst.GetOpcode()) == BufferAccess::Read) {
				const auto& memory = memory_info[inst.Flags<MemoryFlags>().index];
				if (memory.kind == ResourceKind::Buffer &&
				    specialization.buffers[memory.resource].zero_stride_oob) {
					// Bounds mode 0 checks offset >= stride, so zero stride
					// makes every vector read out of bounds regardless of its address.
					const auto count = BufferComponentCount(inst.GetOpcode());
					std::array<Value, 4> values {Value(0u), Value(0u), Value(0u), Value(0u)};
					if (memory.formatted && !memory.typed) {
						const auto& buffer = buffers[memory.resource];
						const auto format = Format::GetFormatInfo(buffer.descriptor_format);
						for (uint32_t component = 0; component < count; component++) {
							if (format.type == Format::ComponentType::Unknown ||
							    GetDstSel(buffer.descriptor_swizzle, component) != 1u) continue;
							const auto one = Format::FormattedConstantBits(
							    format, Format::FormattedSourceKind::One);
							values[component] = Value(&*block->PrependNewInst(
							    it, ValueOpcode::SelectU32,
							    {inst.Arg(inst.NumArgs() - 1), Value(one), Value(0u)}));
						}
					}
					Value result = values[0];
					switch (inst.GetType()) {
						case Type::U8: result = Value(uint8_t {0}); break;
						case Type::U16: result = Value(uint16_t {0}); break;
						case Type::U32x2:
							result = Value(&*block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x2,
							                                      {values[0], values[1]}));
							break;
						case Type::U32x3:
							result = Value(&*block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x3,
							                                      {values[0], values[1], values[2]}));
							break;
						case Type::U32x4:
							result = Value(&*block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x4,
							                                      {values[0], values[1], values[2], values[3]}));
							break;
						default: break;
					}
					inst.ReplaceUsesWith(result);
				}
				continue;
			}
			const auto image_opcode = ImageOpcodeInfoOf(inst.GetOpcode());
			if (image_opcode.access == ImageAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			EXIT_IF(index >= memory_info.size());
			active_image_memory[index] = true;
			auto& memory = memory_info[index];
			EXIT_IF(memory.resource >= images.size());
			const auto& image = images[memory.resource];
			if (specialization.images[memory.resource].fmask) {
				EXIT_IF(inst.GetOpcode() != ValueOpcode::ImageRead || memory.data_bits != 32u);
				// Vulkan MSAA stores each sample directly; FMASK's four-bit fragment indices
				// therefore map each coverage sample to the same host sample.
				constexpr uint32_t indices[] = {0x76543210u, 0xfedcba98u};
				std::array<Value, 2> fragments;
				for (uint32_t component = 0; component < fragments.size(); component++) {
					const auto selected = block->PrependNewInst(
					    it, ValueOpcode::SelectU32, {inst.Arg(2), Value(indices[component]), Value(0u)});
					fragments[component] = Value(&*selected);
				}
				const auto result = block->PrependNewInst(
				    it, ValueOpcode::CompositeConstructU32x4,
				    {fragments[0], fragments[1], Value(0u), Value(0u)});
				inst.ReplaceUsesWith(Value(&*result));
				continue;
			}
			if (image_opcode.needs_sampler &&
			    memory.sampler < original_sampler_count) {
				const auto type = static_cast<uint32_t>(ClassifySampler(image, sampler_plan.separate_compare));
				memory.sampler = sampler_plan.mapping[memory.sampler][type];
				EXIT_IF(memory.sampler == UINT32_MAX);
			}
			if (image.indirect_root == memory.resource &&
			    inst.GetOpcode() != ValueOpcode::ImageSampleRaw &&
			    inst.GetOpcode() != ValueOpcode::ImageQueryDimensions &&
			    inst.GetOpcode() != ValueOpcode::ImageRead &&
			    inst.GetOpcode() != ValueOpcode::ImageWrite) {
				const auto opcode = ValueOpcodeName(inst.GetOpcode());
				EXIT("unsupported indirect image operation: shader=0x%016llx stage=%u pc=0x%08x "
				     "opcode=%.*s\n",
				     static_cast<unsigned long long>(program.shader_hash),
				     static_cast<unsigned>(program.stage), inst.Flags<MemoryFlags>().pc,
				     static_cast<int>(opcode.size()), opcode.data());
			}
		}
	}
	for (size_t index = 0; index < memory_info.size(); ++index) {
		auto& memory = memory_info[index];
		if (active_image_memory[index] && memory.kind == ResourceKind::Image &&
		    !memory.planning_only) {
			memory.resource = image_remap[memory.resource];
		}
	}
	for (auto& buffer: buffers) {
		if (buffer.image_alias != BufferResource::NoImageAlias) {
			buffer.image_alias = image_remap[buffer.image_alias];
		}
	}
	for (auto& pair: sampled_pairs) {
		pair.image = image_remap[pair.image];
	}
	for (auto& image: images) {
		if (image.binding_alias != UINT32_MAX)
			image.binding_alias = image_remap[image.binding_alias];
		if (image.indirect_root != ImageResource::NoIndirectImage) {
			image.indirect_root = image_remap[image.indirect_root];
		}
		for (auto& resource: image.indirect_resources) {
			resource = image_remap[resource];
		}
	}
	image_remap.Apply(images);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
