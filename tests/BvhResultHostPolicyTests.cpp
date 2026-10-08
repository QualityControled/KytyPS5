#include "graphics/shader/recompiler/BvhResultHostPolicy.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
using namespace Libs::Graphics::ShaderRecompiler::Diagnostics;
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}

// This executable is a separate CTest child; only its own process environment
// is changed. Neither branch mutates a parent environment or persisted setting.
static int SetChildEnvironment(const char* key, const char* value) {
#if defined(_WIN32)
    return _putenv_s(key, value ? value : "");
#else
    return value ? setenv(key, value, 1) : unsetenv(key);
#endif
}

int main(int argc,char** argv){try{
    if(argc==2&&std::strcmp(argv[1],"--frozen-mode")==0){
        for(const char* key:{"KYTY_PROBE_EXTERNAL_BEFORE_BVH","KYTY_EXTERNAL_UNWRITTEN_VGPR","KYTY_CAPTURE_EXTERNAL_RESOURCE_READS","KYTY_CAPTURE_EXTERNAL_INPUTS_ONLY","KYTY_PROBE_EXTERNAL_STRUCTURED"})
            Check(SetChildEnvironment(key,nullptr)==0,"failed to clear own child environment");
        Check(SetChildEnvironment("KYTY_PROBE_EXTERNAL_AFTER_BVH","1")==0&&SetChildEnvironment("KYTY_PROBE_EXTERNAL_CALL_TARGET","1")==0,"failed to set own child environment");
        const auto& mode=FrozenAfterBvhMode();Check(mode.enabled&&mode.error==AfterBvhModeError::None,"initial frozen mode failed");
        Check(SetChildEnvironment("KYTY_PROBE_EXTERNAL_AFTER_BVH",nullptr)==0,"failed to mutate own child environment");
        Check(FrozenAfterBvhMode().enabled,"frozen mode changed after initialization");
        const auto current=ParseAfterBvhMode({std::getenv("KYTY_PROBE_EXTERNAL_AFTER_BVH"),std::getenv("KYTY_PROBE_EXTERNAL_CALL_TARGET")});
        Check(current.enabled!=mode.enabled,"mode mutation was undetectable");
        std::printf("HostPolicyTests: PASS frozen_mode_once=true environment_change_detectable=true noGPU=true\n");return 0;
    }
    unsigned groups=0;
    Check(!ParseAfterBvhMode({}).enabled,"default mode changed");++groups;
    Check(ParseAfterBvhMode({"1","1"}).enabled,"after target rejected");++groups;
    Check(ParseAfterBvhMode({"1","1",nullptr,nullptr,nullptr,nullptr,"1"}).structured,"structured composition rejected");++groups;
    Check(ParseAfterBvhMode({"0","1"}).error==AfterBvhModeError::InvalidValue,"bad after accepted");++groups;
    Check(ParseAfterBvhMode({"1",nullptr}).error==AfterBvhModeError::MissingTargetProbe,"absent target accepted");++groups;
    Check(ParseAfterBvhMode({"1","0"}).error==AfterBvhModeError::MissingTargetProbe,"bad target accepted");++groups;
    Check(ParseAfterBvhMode({"1","1","1"}).error==AfterBvhModeError::BeforeConflict,"before conflict accepted");++groups;
    Check(ParseAfterBvhMode({"1","1",nullptr,"27"}).error==AfterBvhModeError::CheckedConflict,"checked conflict accepted");++groups;
    Check(ParseAfterBvhMode({"1","1",nullptr,nullptr,"1"}).error==AfterBvhModeError::ResourceCaptureConflict,"read epoch conflict accepted");++groups;
    Check(ParseAfterBvhMode({"1","1",nullptr,nullptr,nullptr,"1"}).error==AfterBvhModeError::InputCaptureConflict,"input-only conflict accepted");++groups;
    Check(ParseAfterBvhMode({"1","1",nullptr,nullptr,nullptr,nullptr,"0"}).error==AfterBvhModeError::InvalidStructuredValue,"bad structured accepted");++groups;
    Check(FaultRecordBytes(false)==128&&FaultRecordBytes(true)==136,"logical sizes changed");++groups;
    for(auto atom:{uint64_t{1},uint64_t{4},uint64_t{16},uint64_t{64},uint64_t{128},uint64_t{256},uint64_t{4096}}){
        uint64_t stride=0;Check(FaultAreaStride(136,atom,8,stride),"valid stride rejected");
        Check(stride%atom==0&&stride>=136,"stride not atom aligned");
        for(uint64_t i=0;i<7;++i){auto begin=i*stride;auto invalidated_end=((begin+136+atom-1)/atom)*atom;
            Check(invalidated_end<=(i+1)*stride,"two pending invalidations overlap");}
    }++groups;
    uint64_t stride=777;Check(FaultAreaStride(136,128,8,stride)&&stride==256,"136/128 alignment wrong");++groups;
    Check(FaultAreaStride(136,16,8,stride)&&stride==144,"136/16 alignment wrong");++groups;
    Check(!FaultAreaStride(136,0,8,stride)&&!FaultAreaStride(136,128,0,stride)&&!FaultAreaStride(135,128,8,stride),"invalid alignment contract accepted");++groups;
    stride=777;Check(!FaultAreaStride(136,UINT64_MAX,8,stride)&&stride==777,"stride overflow changed output");++groups;
    std::array<uint32_t,34> record{};Check(ClassifyBvhHostRecord(record,true)==BvhHostRecordKind::Empty,"empty result not empty");++groups;
    record[0]=1;record[1]=8;record[26]=2;record[27]=32;record[28]=BvhSchemaMagic;record[29]=2;record[15]=1;
    record[30]=0xffffffff;record[31]=0x01234567;record[32]=0x89abcdef;record[33]=0xa5a5a5a5;
    Check(ClassifyBvhHostRecord(record,true)==BvhHostRecordKind::AfterBvh,"valid kind8 rejected");++groups;
    Check(ClassifyBvhHostRecord(std::span(record).first(32),false)==BvhHostRecordKind::Invalid,"kind8 truncated accepted");++groups;
    auto encoded=EncodeBvhResultLittleEndian(record);Check(encoded.size()==136&&encoded[124]==0x67&&encoded[125]==0x45&&encoded[128]==0xef&&encoded[135]==0xa5,"result serialization/order changed");++groups;
    auto invalid=record;invalid[29]=1;Check(ClassifyBvhHostRecord(invalid,true)==BvhHostRecordKind::Invalid,"wrong schema accepted");++groups;
    invalid=record;invalid[15]=0;Check(ClassifyBvhHostRecord(invalid,true)==BvhHostRecordKind::Invalid,"inactive winner accepted");++groups;
    invalid=record;invalid[26]=1;invalid[13]=4;Check(ClassifyBvhHostRecord(invalid,true)==BvhHostRecordKind::Invalid,"narrow high bits accepted");++groups;
    std::array<uint32_t,32> before{};before[0]=1;before[1]=7;before[26]=2;before[27]=32;before[28]=BvhSchemaMagic;before[29]=1;before[15]=1;
    Check(ClassifyBvhHostRecord(before,false)==BvhHostRecordKind::BeforeBvh,"old kind7 ABI changed");++groups;
    std::copy(before.begin(),before.end(),record.begin());record[32]=0xaaa;record[33]=0xbbb;
    Check(ClassifyBvhHostRecord(record,true)==BvhHostRecordKind::BeforeBvh,"old kind7 exact slice rejected in larger storage");++groups;
    std::printf("HostPolicyTests: PASS groups=%u noGPU=true\n",groups);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"HostPolicyTests: FAIL %s\n",e.what());return 1;}}
