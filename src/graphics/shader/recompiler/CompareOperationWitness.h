#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics::CompareOperationWitness {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("KYTY_COMPARE_OPERATION_WITNESS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
inline bool Selected(uint32_t stage, uint64_t hash) {
    if (!Enabled() || stage != 2) return false;
    struct Filter { bool present = false, valid = false; uint64_t value = 0; };
    static const Filter filter = [] {
        Filter out;
        const char* value = std::getenv("KYTY_COMPARE_OPERATION_WITNESS_HASH");
        if (value == nullptr) return out;
        out.present = true;
        const auto size = std::strlen(value);
        if (size != 16) return out;
        const auto result = std::from_chars(value, value + size, out.value, 16);
        out.valid = result.ec == std::errc() && result.ptr == value + size;
        return out;
    }();
    return !filter.present || (filter.valid && filter.value == hash);
}

inline constexpr uint32_t Unknown = UINT32_MAX;
inline constexpr size_t MaxOperations = 128, MaxDefinitions = 16384, MaxWords = 1048576;
struct Descriptor {
    uint32_t variable = Unknown, set = Unknown, binding = Unknown, element = Unknown;
    bool known = false;
};
struct Operation {
    uint32_t word = 0, opcode = 0, result = 0, sampled = 0;
    Descriptor image, sampler;
};
struct Analysis {
    bool valid = false, definitions_complete = true, operations_complete = true;
    uint32_t dref_count = 0;
    std::vector<Operation> operations;
};

inline bool IsDref(uint32_t op) {
    switch (op) {
    case spv::OpImageSampleDrefImplicitLod: case spv::OpImageSampleDrefExplicitLod:
    case spv::OpImageSampleProjDrefImplicitLod: case spv::OpImageSampleProjDrefExplicitLod:
    case spv::OpImageDrefGather:
    case spv::OpImageSparseSampleDrefImplicitLod: case spv::OpImageSparseSampleDrefExplicitLod:
    case spv::OpImageSparseSampleProjDrefImplicitLod: case spv::OpImageSparseSampleProjDrefExplicitLod:
    case spv::OpImageSparseDrefGather: return true;
    default: return false;
    }
}

// Reads only the already-created module's words. This is a bounded metadata
// parser, not a validator or an execution/election proof. Ambiguous/dynamic
// pointers, unsupported definitions and truncated graphs remain unknown.
inline Analysis Analyze(std::span<const uint32_t> words) {
    Analysis out;
    if (words.size() < 5 || words.size() > MaxWords || words[0] != spv::MagicNumber || words[3] == 0) return out;
    struct Def { uint32_t id, word; };
    struct Decoration { uint32_t id, kind, value; };
    std::vector<Def> defs;
    std::vector<Decoration> decorations;
    defs.reserve(std::min(words.size() / 4, MaxDefinitions));
    for (size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> 16, op = words[offset] & 0xffffu;
        if (count == 0 || count > words.size() - offset) return out;
        if (op == spv::OpDecorate && count == 4 &&
            (words[offset+2] == spv::DecorationDescriptorSet || words[offset+2] == spv::DecorationBinding)) {
            if (decorations.size() < 512) decorations.push_back({words[offset+1],words[offset+2],words[offset+3]});
            else out.definitions_complete = false;
        }
        const bool keep = op == spv::OpConstant || op == spv::OpTypeInt || op == spv::OpVariable ||
            op == spv::OpLoad || op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain ||
            op == spv::OpSampledImage || op == spv::OpCopyObject;
        if (keep) {
            const size_t result = op == spv::OpTypeInt ? 1 : 2;
            if (count <= result || words[offset+result] == 0 || words[offset+result] >= words[3]) return out;
            if (defs.size() < MaxDefinitions) defs.push_back({words[offset+result],static_cast<uint32_t>(offset)});
            else out.definitions_complete = false;
        }
        if (IsDref(op)) {
            if (count < 6 || words[offset+2] == 0 || words[offset+2] >= words[3]) return out;
            ++out.dref_count;
            if (out.operations.size() < MaxOperations)
                out.operations.push_back({static_cast<uint32_t>(offset),op,words[offset+2],words[offset+3]});
            else out.operations_complete = false;
        }
        offset += count;
    }
    std::sort(defs.begin(),defs.end(),[](const Def& a,const Def& b){return a.id < b.id;});
    for (size_t i=1;i<defs.size();++i) if (defs[i-1].id == defs[i].id) return out;
    const auto definition = [&](uint32_t id) -> std::span<const uint32_t> {
        const auto found = std::lower_bound(defs.begin(),defs.end(),id,[](const Def& a,uint32_t b){return a.id < b;});
        if (found == defs.end() || found->id != id) return {};
        return words.subspan(found->word,words[found->word] >> 16);
    };
    const auto constant = [&](uint32_t id) -> uint32_t {
        const auto d = definition(id);
        if (d.size()!=4 || (d[0]&0xffffu)!=spv::OpConstant) return Unknown;
        const auto type = definition(d[1]);
        if (type.size()!=4 || (type[0]&0xffffu)!=spv::OpTypeInt || type[2]!=32 || type[3]>1) return Unknown;
        return d[3];
    };
    const auto trace = [&](uint32_t id) -> Descriptor {
        Descriptor r;
        uint32_t element = Unknown;
        for (size_t depth=0;depth<16;++depth) {
            const auto d=definition(id);
            if (d.empty()) return r;
            const auto op=d[0]&0xffffu;
            if (op==spv::OpCopyObject && d.size()==4) {id=d[3]; continue;}
            if (op==spv::OpLoad && d.size()>=4) {id=d[3]; continue;}
            if ((op==spv::OpAccessChain || op==spv::OpInBoundsAccessChain) && d.size()==5 && element==Unknown) {
                element=constant(d[4]);
                if (element==Unknown) return r;
                id=d[3]; continue;
            }
            if (op!=spv::OpVariable || d.size()<4 || d[3]!=spv::StorageClassUniformConstant || element==Unknown) return r;
            r.variable=id; r.element=element;
            for (const auto& decoration:decorations) {
                if (decoration.id!=id) continue;
                auto& value=decoration.kind==spv::DecorationDescriptorSet ? r.set : r.binding;
                if (value!=Unknown && value!=decoration.value) return Descriptor {};
                value=decoration.value;
            }
            r.known=r.set!=Unknown && r.binding!=Unknown;
            return r;
        }
        return r;
    };
    for (auto& operation:out.operations) {
        if (!out.definitions_complete) continue;
        uint32_t sampled=operation.sampled;
        for (size_t depth=0;depth<16;++depth) {
            const auto d=definition(sampled);
            if (d.size()==4 && (d[0]&0xffffu)==spv::OpCopyObject) {sampled=d[3];continue;}
            if (d.size()==5 && (d[0]&0xffffu)==spv::OpSampledImage) {
                operation.image=trace(d[3]); operation.sampler=trace(d[4]);
            }
            break;
        }
    }
    out.valid=true;
    return out;
}

struct Binding {
    uint32_t set=0, binding=0;
    std::vector<uint32_t> resources;
};
inline uint32_t Resource(const Descriptor& descriptor,std::span<const Binding> bindings) {
    if (!descriptor.known) return Unknown;
    uint32_t result=Unknown;
    size_t matches=0;
    for (const auto& row:bindings) {
        if (row.set!=descriptor.set || row.binding!=descriptor.binding) continue;
        if (++matches!=1 || descriptor.element>=row.resources.size()) return Unknown;
        result=row.resources[descriptor.element];
    }
    return result;
}
struct Site {uint32_t pc=0, resource=Unknown, sampler=Unknown, canonical_image=Unknown, canonical_sampler=Unknown;};
struct EmittedPair {Operation operation; uint32_t image=Unknown, sampler=Unknown, pc=Unknown;};
struct Module {uint64_t shader=0, spirv_hash=0, id=0; std::vector<EmittedPair> pairs;};
inline uint32_t UniquePc(uint32_t image,uint32_t sampler,std::span<const Site> sites) {
    if (image==Unknown || sampler==Unknown) return Unknown;
    uint32_t pc=Unknown; size_t count=0;
    for (const auto& s:sites) if (s.canonical_image==image && s.canonical_sampler==sampler) {pc=s.pc;++count;}
    return count==1 ? pc : Unknown;
}

class Registry {
public:
    static constexpr size_t MaxRecords=64, MaxBytes=1048576, MaxRecordBytes=32768;
    void Remember(Module module) {
        std::lock_guard lock(mutex);
        if (modules.size()<MaxRecords) modules.push_back(std::move(module));
    }
    Module Lookup(uint64_t id) {
        std::lock_guard lock(mutex);
        for (const auto& m:modules) if (m.id==id) return m;
        return {};
    }
    bool Admit(std::string_view text) {
        std::lock_guard lock(mutex);
        if (text.size()>MaxRecordBytes || records==MaxRecords || text.size()>MaxBytes-bytes) return false;
        if (std::find(signatures.begin(),signatures.end(),text)!=signatures.end()) return false;
        signatures.emplace_back(text); ++records;bytes+=text.size();return true;
    }
private:
    std::mutex mutex;
    std::vector<Module> modules;
    std::vector<std::string> signatures;
    size_t records=0,bytes=0;
};
inline Registry& State() {static Registry state;return state;}
inline void Publish(const std::string& text) {
    const std::string framed = "CompareOperationWitness begin schema=1 max_records=64 max_bytes=1048576 gpu_execution_not_proved=1\n" +
        text + "CompareOperationWitness end body_bytes=" + std::to_string(text.size()) + "\n";
    if (!State().Admit(framed)) return;
    static std::mutex output_mutex;
    std::lock_guard lock(output_mutex);
    std::fwrite(framed.data(),1,framed.size(),stdout);
    std::fflush(stdout);
}

} // namespace Libs::Graphics::CompareOperationWitness
