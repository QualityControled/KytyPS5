#pragma once

#include "graphics/shader/recompiler/ExternalProgram.h"
#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <xxhash.h>

namespace Libs::Graphics::ShaderRecompiler::Diagnostics {

// Diagnostic CPU domain only. No selection/election, compilation cache or GPU
// execution policy is established by this result.
struct SelectedNativeInstruction {
    uint32_t offset=0;
    std::vector<uint32_t> words;
};
struct SelectedNativeReference {
    uint64_t caller_hash=0;
    uint32_t caller_pc=0, domain=0, ordinal=0, link_sgpr=0, extent_bytes=0;
    std::vector<SelectedNativeInstruction> instructions;
    std::vector<uint32_t> serialized_words;
};
struct SelectedReferencePolicy {
    uint64_t caller_hash=0x3351560625c27256ull;
    uint32_t caller_pc=0x1a4u, domain=0u, ordinal=8156u, link_sgpr=14u;
    uint32_t instructions=1457u, extent_bytes=0x1f34u;
    uint64_t digest_low=0xb76ee41325b5dd93ull, digest_high=0xfade6f1f85f00811ull;
};
struct SelectedCapturePreparation {
    ExternalLibraryPlan overlay;
    uint64_t target=0;
    uint32_t function_id=0;
    bool success=false;
    std::string failure;
};

inline bool ParseSelectedNativeReference(std::span<const uint32_t> words,
                                         const SelectedReferencePolicy& policy,
                                         SelectedNativeReference& reference,
                                         std::string& failure) {
    const auto reject=[&](const char* text){failure=text;return false;};
    if(words.size()<12u || words.size()>16384u)return reject("reference size outside bounded schema");
    const auto digest=XXH3_128bits(words.data(),words.size_bytes());
    if(digest.low64!=policy.digest_low || digest.high64!=policy.digest_high)
        return reject("reference digest mismatch");
    if(words[0]!=0x53435231u || words[1]!=1u || words[10]!=words.size() || words[11]!=0u)
        return reject("reference schema/header mismatch");
    const uint64_t hash=uint64_t(words[2])|(uint64_t(words[3])<<32u);
    if(hash!=policy.caller_hash || words[4]!=policy.caller_pc || words[5]!=policy.domain ||
       words[6]!=policy.ordinal || words[7]!=policy.link_sgpr ||
       words[8]!=policy.instructions || words[9]!=policy.extent_bytes ||
       words[8]==0u || words[8]>2048u || words[9]==0u || words[9]>65536u || (words[9]&3u))
        return reject("reference policy/finite extent mismatch");
    SelectedNativeReference result;
    result.caller_hash=hash;result.caller_pc=words[4];result.domain=words[5];
    result.ordinal=words[6];result.link_sgpr=words[7];result.extent_bytes=words[9];
    size_t at=12u;uint64_t prior_end=0;
    for(uint32_t i=0;i<words[8];++i){
        if(words.size()-at<2u)return reject("truncated reference instruction header");
        const uint32_t offset=words[at++],count=words[at++];
        if((offset&3u) || count==0u || count>32u || count>words.size()-at ||
           offset<prior_end || uint64_t(offset)+uint64_t(count)*4u>result.extent_bytes ||
           (i==0u && offset!=0u))return reject("invalid reference instruction bounds/order");
        result.instructions.push_back({offset,{words.begin()+at,words.begin()+at+count}});
        at+=count;prior_end=uint64_t(offset)+uint64_t(count)*4u;
    }
    if(at!=words.size() || prior_end!=result.extent_bytes)return reject("reference trailing data/extent mismatch");
    result.serialized_words.assign(words.begin(),words.end());
    reference=std::move(result);return true;
}

inline constexpr uint32_t MaxSelectedBodyContextRecords=128u;

// The producer is the production guarded LoadExternalLibrary held plan. The
// reference ordinal identifies historical evidence only. It never chooses a
// current table row or establishes GPU election in this invocation.
inline SelectedCapturePreparation PrepareSelectedCalleeCapture(
    uint64_t caller_hash,const Decoder::Program& caller,const ExternalLibraryPlan& held,
    const SelectedNativeReference& reference) {
    SelectedCapturePreparation result;
    const auto reject=[&](const char* text){result.failure=text;return result;};
    if(caller_hash!=reference.caller_hash || !held.complete || held.functions.empty() ||
       held.functions.size()>2048u || held.call_sites.size()!=1u || held.dependencies.empty())
        return reject("body diagnostic requires matching caller and complete bounded held plan");
    const auto& site=held.call_sites.front();
    if(site.caller_pc!=reference.caller_pc || site.context_domain!=reference.domain ||
       site.target_sgpr!=reference.link_sgpr || site.return_sgpr!=reference.link_sgpr ||
       site.table_bytes==0u || site.table_bytes>1048576u || site.table_bytes%16u ||
       site.records.size()!=site.table_bytes/16u || site.context_records.size()!=site.records.size() ||
       site.auxiliary_sgpr>104u)
        return reject("body diagnostic call/domain/table ABI mismatch");
    std::unordered_map<uint32_t,const ExternalFunction*> functions;
    std::unordered_map<uint64_t,const ExternalFunction*> addresses;
    const ExternalFunction* matched=nullptr;
    for(const auto& function:held.functions){
        if(function.guest_address==0u || (function.guest_address&3u) ||
           (function.guest_address>>48u) || function.code_prefix.size()!=16384u ||
           !functions.emplace(function.function_id,&function).second ||
           !addresses.emplace(function.guest_address,&function).second)
            return reject("body diagnostic held function catalogue malformed");
        bool equal=true;
        for(const auto& instruction:reference.instructions){
            const size_t first=instruction.offset/4u;
            if(first>function.code_prefix.size() || instruction.words.size()>function.code_prefix.size()-first ||
               !std::equal(instruction.words.begin(),instruction.words.end(),function.code_prefix.begin()+first)){
                equal=false;break;
            }
        }
        if(equal){
            if(matched!=nullptr)return reject("body diagnostic exact native match is ambiguous");
            matched=&function;
        }
    }
    if(matched==nullptr)return reject("body diagnostic exact reachable native match absent");
    std::unordered_set<uint64_t> candidates;
    for(const auto address:site.candidate_addresses){
        if(!addresses.contains(address) || !candidates.insert(address).second)
            return reject("body diagnostic candidate catalogue malformed");
    }
    if(candidates.size()!=held.functions.size())return reject("body diagnostic candidate catalogue incomplete");
    const size_t records=site.records.size();
    std::vector<const ExternalRecord*> by_ordinal(records,nullptr);
    std::vector<const IR::ExternalCallContextRecord*> contexts(records,nullptr);
    for(const auto& record:site.records){
        const auto function=functions.find(record.function_id);
        if(record.ordinal>=records || by_ordinal[record.ordinal]!=nullptr || function==functions.end() ||
           record.function_address!=function->second->guest_address || !candidates.contains(record.function_address))
            return reject("body diagnostic current record provenance malformed");
        by_ordinal[record.ordinal]=&record;
    }
    for(const auto& context:site.context_records){
        if(context.ordinal>=records || contexts[context.ordinal]!=nullptr)
            return reject("body diagnostic current context ordinal malformed");
        contexts[context.ordinal]=&context;
    }
    result.overlay.caller_address=held.caller_address;
    result.overlay.functions.push_back(*matched);
    result.overlay.functions.front().code_prefix.resize(reference.extent_bytes/4u);
    result.overlay.call_sites.push_back(site);
    auto& overlay_site=result.overlay.call_sites.front();
    overlay_site.candidate_addresses={matched->guest_address};
    overlay_site.records.clear();overlay_site.context_records.clear();
    for(size_t ordinal=0;ordinal<records;++ordinal){
        const auto* record=by_ordinal[ordinal];const auto* context=contexts[ordinal];
        if(record==nullptr || context==nullptr || context->function_id!=record->function_id ||
           context->words!=std::array<uint32_t,4>{uint32_t(record->function_address),uint32_t(record->function_address>>32u),uint32_t(record->auxiliary_address),uint32_t(record->auxiliary_address>>32u)})
            return reject("body diagnostic current record/context raw identity mismatch");
        if(record->function_id==matched->function_id){
            if(overlay_site.records.size()>=MaxSelectedBodyContextRecords)
                return reject("body diagnostic matching context record limit exceeded");
            overlay_site.records.push_back(*record);overlay_site.context_records.push_back(*context);
        }
    }
    if(overlay_site.records.empty())return reject("body diagnostic matching current records absent");
    // Only this authored one-body/all-current-matching-context CPU domain is
    // narrowed. The full held dependencies/topology are serialized separately.
    result.overlay.complete=true;
    const auto provenance=BuildExternalCallProbe(caller,result.overlay);
    if(!provenance.success){result.failure="body candidate load/call provenance rejected: "+provenance.failure;return result;}
    const auto linked=LinkExternalProgram(caller,result.overlay);
    if(!linked.success){result.failure="body candidate leaf closure/return rejected: "+linked.failure;return result;}
    if(linked.entries.size()!=1u || linked.program.instructions.size()!=caller.instructions.size()+reference.instructions.size())
        return reject("body candidate reachable instruction closure/count mismatch");
    for(size_t i=0;i<reference.instructions.size();++i){
        const auto& actual=linked.program.instructions[caller.instructions.size()+i];
        const auto& expected=reference.instructions[i];
        if(actual.word_count!=expected.words.size() ||
           !std::equal(expected.words.begin(),expected.words.end(),actual.raw))
            return reject("body candidate linked reachable word identity mismatch");
    }
    result.target=matched->guest_address;result.function_id=matched->function_id;
    result.success=true;return result;
}
} // namespace Libs::Graphics::ShaderRecompiler::Diagnostics
