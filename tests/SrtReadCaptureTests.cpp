#include "graphics/shader/recompiler/ir/passes/SrtReadCapture.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "common/assert.h"
#include "common/logging/log.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>

namespace Common {
int DbgExitHandler(const char*,int,std::string_view s){std::fprintf(stderr,"fatal:%.*s\n",int(s.size()),s.data());std::exit(2);}
int DbgExitHandler(const char* f,int l,fmt::text_style,std::string_view s){return DbgExitHandler(f,l,s);}
int DbgExitIfHandler(const char* s,const char*,int){std::fprintf(stderr,"assert:%s\n",s);std::exit(2);}
int DbgNotImplementedHandler(const char* s,const char*,int){std::fprintf(stderr,"unsupported:%s\n",s);std::exit(2);}
void DbgExit(int s){std::exit(s);}
}
namespace Log {
bool IsSilent(){return true;}
void Write(std::string_view s){std::fprintf(stderr,"%.*s",int(s.size()),s.data());}
void Write(fmt::text_style,std::string_view s){Write(s);}
void WriteToConsoleAndLog(std::string_view s){Write(s);}
}
namespace {
namespace IR=Libs::Graphics::ShaderRecompiler::IR;
using namespace IR;
void Check(bool okay,const char* text){if(!okay){std::fprintf(stderr,"FAIL:%s\n",text);std::exit(1);}}
SrtReadCaptureIdentity Identity(){return {
 .stage=Libs::Graphics::ShaderType::Compute,.shader_hash=0xcfbc46ff1e1ea34aull,
 .shader_base=0x120aed7f00ull,.pass_id=71,.user_data_base=0,
 .user_data={0xda7c00u,0x10u},.control_schema="authored-compute-controls-v1",
 .control_words={64,32,64,1,1},.caller_identity="fixture-exact-caller-artifact",
 .library_identity="fixture-exact-library-dependency-inventory",
 .input_identity="fixture-exact-user-control-artifact"};}
void Send(SrtReadCapture& capture,uint64_t address,std::span<const uint32_t> words,
          std::optional<SrtReadContext> context={}){
 SrtRuntime runtime{.post_read_observer=SrtReadCapture::Observe,.post_read_userdata=&capture};
 ObserveSrtRead(runtime,SrtReadKind::Scalar,address,words.size_bytes(),words,true,context,false,0x7c);
}
Block& AddBlock(Program& p){auto b=std::make_unique<Block>();auto* r=b.get();p.blocks.push_back(r);p.block_info.push_back({.id=uint32_t(p.blocks.size()-1)});p.block_storage.push_back(std::move(b));return *r;}
struct Reads {
 std::vector<std::pair<char,uint64_t>> order;
 bool fail=false;
 static bool Read(void* data,uint64_t address,std::span<uint32_t> words,char kind){
  auto& r=*static_cast<Reads*>(data);r.order.emplace_back(kind,address);
  for(size_t i=0;i<words.size();++i)words[i]=uint32_t(address+i*4)^uint32_t(kind);
  return !r.fail;
 }
 static bool Ordinary(void* d,uint64_t a,std::span<uint32_t> w){return Read(d,a,w,'o');}
 static bool Strict(void* d,uint64_t a,std::span<uint32_t> w){return Read(d,a,w,'s');}
};
struct ScalarFixture {
 Program program;std::vector<Inst*> values;
 explicit ScalarFixture(uint64_t address,uint32_t count=3){
  program.stage=Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete=true;program.resource_tracking_complete=true;
  program.info.uses_external_call_probe=true;
  program.memory_info.push_back({.kind=ResourceKind::ScalarAddress});
  auto& b=AddBlock(program);auto& h=b.AppendNewInst(ValueOpcode::GetAddressResource,
     {Value(uint32_t(address)),Value(uint32_t(address>>32))});
  for(uint32_t i=0;i<count;++i){auto& v=b.AppendNewInst(ValueOpcode::LoadAddressU32,
     {Value(&h),Value(i*4),Value(0u),Value(true)});
   v.SetFlags(MemoryFlags{.index=0,.pc=0x7c+i*8});values.push_back(&v);
   program.srt_reads.push_back({Value(&v),i});}
 }
};
void TestReaderOrderAndResults(){
 ScalarFixture fixture(0x1000);
 auto plan=ExtractResourcePlan(fixture.program);
 plan.capture_specialization_reads=true;
 std::vector<std::pair<char,uint64_t>> baseline;std::vector<uint32_t> flat;
 for(bool observed:{false,true}){
  Reads reads;SrtReadCapture capture(Identity());
  SrtRuntime runtime{.read_memory=Reads::Ordinary,.userdata=&reads,.read_specialization_memory=Reads::Strict,
    .post_read_observer=observed?SrtReadCapture::Observe:nullptr,.post_read_userdata=&capture};
  ResourceSnapshot snapshot;ResourceSpecialization specialization;
  const bool okay=MaterializeResources(plan,runtime,snapshot,specialization);capture.Finalize(okay);
  Check(okay,"ordinary materialization changed result");
  if(!observed){baseline=reads.order;flat=snapshot.flattened_srt;Check(capture.Counters().hooks==0&&capture.Words().empty(),"flagoff observer ran");}
  else {Check(reads.order==baseline&&snapshot.flattened_srt==flat,"observer changed read order/count/value");
   Check(capture.Complete()&&capture.Words().size()==3&&capture.Observations().size()==3,"ordinary returned words absent");
   for(size_t i=0;i<3;++i)Check(!capture.Observations()[i].specialization_read&&capture.Observations()[i].native_pc==0x7c+i*8,"ordinary native read metadata lost");}
 }
}
void TestNullOrdinaryReader(){
 std::array<uint32_t,3> backing{0x12345678,0xabcdef00,0x80000000};
 ScalarFixture fixture(reinterpret_cast<uint64_t>(backing.data()));auto plan=ExtractResourcePlan(fixture.program);
 plan.capture_specialization_reads=true;
 Reads reads;SrtReadCapture capture(Identity());
 SrtRuntime runtime{.userdata=&reads,.read_specialization_memory=Reads::Strict,
    .post_read_observer=SrtReadCapture::Observe,.post_read_userdata=&capture};
 ResourceSnapshot snapshot;ResourceSpecialization specialization;
 const bool okay=MaterializeResources(plan,runtime,snapshot,specialization);capture.Finalize(okay);
 Check(okay&&capture.Complete()&&reads.order.empty(),"ordinary memcpy invoked clean reader");
 Check(snapshot.flattened_srt==std::vector<uint32_t>(backing.begin(),backing.end()),"ordinary memcpy value changed");
 for(size_t i=0;i<3;++i)Check(capture.Words().at(reinterpret_cast<uint64_t>(backing.data())+i*4)==backing[i],"copied ordinary value not captured");
}
void TestCleanAndContext(){
 ScalarFixture fixture(0x2000,1);Reads reads;SrtReadCapture capture(Identity());
 ExternalCallContextRecord record{.ordinal=9,.function_id=37,.words={0xaabbccdd,0x44,0x11223344,0x55}};
 const std::array<ExternalCallContextDomain,1> domains{{{11,true,{&record,1}}}};
 SrtRuntime runtime{.read_memory=Reads::Ordinary,.userdata=&reads,.read_specialization_memory=Reads::Strict,
   .external_context_domains=domains,
   .post_read_observer=SrtReadCapture::Observe,.post_read_userdata=&capture};
 uint32_t word=0;SrtWalker walker(fixture.program,CleanRuntime(runtime),{},nullptr,{},SrtExternalContext{11,&record});
 Check(walker.Evaluate(Value(fixture.values[0]),word)&&word==(0x2000u^uint32_t('s')),"clean result changed");
 Check(walker.Evaluate(Value(fixture.values[0]),word)&&reads.order.size()==1,"observer defeated evaluation memo");
 capture.Finalize(true);const auto& event=capture.Observations().at(0);
 Check(capture.Complete()&&event.specialization_read&&event.context&&event.context->domain_id==11&&event.context->function_id==37&&event.context->record_ordinal==9&&event.context->record_words==record.words&&!event.native_pc,"actual context/clean association or synthetic-PC omission lost");
}
void TestFailureAndNoCaptureAfterInvalidation(){
 ScalarFixture fixture(0x3000,1);Reads reads;reads.fail=true;SrtReadCapture capture(Identity());
 SrtRuntime runtime{.read_memory=Reads::Ordinary,.userdata=&reads,.post_read_observer=SrtReadCapture::Observe,.post_read_userdata=&capture};
 uint32_t word=0x7777;Check(!SrtWalker(fixture.program,runtime).Evaluate(Value(fixture.values[0]),word),"observer changed failed reader result");
 Check(capture.Status()==SrtReadCaptureStatus::ReadFailed&&capture.Words().empty()&&capture.Observations().size()==1&&!capture.Observations()[0].succeeded,"failed reader stored partial garbage");
 const std::span<const uint32_t> poison(reinterpret_cast<const uint32_t*>(1),1);
 ObserveSrtRead(runtime,SrtReadKind::Scalar,0x4000,4,poison,true);
 Check(capture.Counters().ignored_after_invalidation==1&&capture.Words().empty(),"invalid capture inspected poison span");
 capture.Finalize(false);const auto hooks=capture.Counters().hooks;
 ObserveSrtRead(runtime,SrtReadKind::Scalar,0x4000,4,poison,true);
 Check(!capture.Complete()&&capture.Counters().hooks==hooks,"finalized snapshot changed");
}
void TestOverlapConflict(){
 SrtReadCapture capture(Identity());const std::array<uint32_t,3> a{1,2,3};const std::array<uint32_t,3> b{2,3,4};
 Send(capture,0x1000,a);Send(capture,0x1004,b);Send(capture,0x1000,a);
 Check(capture.Words().size()==4&&capture.Counters().observed_bytes==36,"overlap dedupe failed");
 const std::array<uint32_t,2> wrong{3,99};Send(capture,0x1008,wrong);
 Check(capture.Status()==SrtReadCaptureStatus::ConflictingWord&&capture.Failure().address==0x100c&&capture.Failure().previous_word==4&&capture.Failure().conflicting_word==99&&capture.Words().at(0x100c)==4,"conflict evidence/first value lost");
 const auto observations=capture.Observations().size();Send(capture,0x2000,a);capture.Finalize(true);
 Check(!capture.Complete()&&capture.Observations().size()==observations&&capture.Counters().ignored_after_invalidation==1,"conflict completion/storage accepted");
}
void TestCaps(){
 const std::array<uint32_t,2> words{1,2};
 {SrtReadCapture c(Identity(),{.unique_bytes=4,.observation_bytes=32,.observations=8});Send(c,0x1000,words);Check(c.Status()==SrtReadCaptureStatus::UniqueWordLimit&&c.Words().empty(),"unique cap partially inserted span");}
 {SrtReadCapture c(Identity(),{.unique_bytes=16,.observation_bytes=8,.observations=8});Send(c,0x1000,words);Send(c,0x1000,std::span(words).first(1));Check(c.Status()==SrtReadCaptureStatus::ObservationByteLimit&&c.Counters().observed_bytes==8,"observation byte budget exceeded");}
 {SrtReadCapture c(Identity(),{.unique_bytes=16,.observation_bytes=64,.observations=1});Send(c,0x1000,words);Send(c,0x1000,words);Check(c.Status()==SrtReadCaptureStatus::ObservationCountLimit&&c.Observations().size()==1,"observation count exceeded");}
 {SrtReadCapture c(Identity(),{UINT64_MAX,UINT64_MAX,UINT64_MAX});Check(c.Limits().unique_bytes==262144&&c.Limits().observation_bytes==1048576&&c.Limits().observations==65536,"caller widened hard bounds");
  std::vector<uint32_t> full(65536,0xface);for(int i=0;i<4;++i)Send(c,0x100000,full);Send(c,0x100000,std::span(full).first(1));Check(c.Status()==SrtReadCaptureStatus::ObservationByteLimit&&c.Counters().observed_bytes==1048576&&c.Words().size()==65536,"exact full budget not enforced");}
}
void TestInvalidObserverDoesNotStopOriginalReads(){
 ScalarFixture fixture(0x5000);auto plan=ExtractResourcePlan(fixture.program);plan.capture_specialization_reads=true;
 Reads reads;SrtReadCapture capture(Identity(),{.unique_bytes=4,.observation_bytes=64,.observations=8});
 SrtRuntime r{.read_memory=Reads::Ordinary,.userdata=&reads,.read_specialization_memory=Reads::Strict,
  .post_read_observer=SrtReadCapture::Observe,.post_read_userdata=&capture};
 ResourceSnapshot snapshot;ResourceSpecialization specialization;
 const bool okay=MaterializeResources(plan,r,snapshot,specialization);capture.Finalize(okay);
 Check(okay&&reads.order==std::vector<std::pair<char,uint64_t>>{{'o',0x5000},{'o',0x5004},{'o',0x5008}}&&
  snapshot.flattened_srt==std::vector<uint32_t>{0x5000u^uint32_t('o'),0x5004u^uint32_t('o'),0x5008u^uint32_t('o')},"capture invalidation changed later original reads");
 Check(!capture.Complete()&&capture.Status()==SrtReadCaptureStatus::UniqueWordLimit&&capture.Counters().ignored_after_invalidation==1&&capture.Words().size()==1,"invalid observer kept copying later payload");
}
void TestMalformedAndIdentity(){
 const std::array<uint32_t,1> words{0x1234};
 for(auto address:{uint64_t{1},UINT64_MAX-1,UINT64_MAX-3}){SrtReadCapture c(Identity());Send(c,address,words);Check(c.Status()==SrtReadCaptureStatus::InvalidSpan&&c.Words().empty(),"misalignment/exclusive-end overflow repaired");}
 {SrtReadCapture c(Identity());SrtRuntime r{.post_read_observer=SrtReadCapture::Observe,.post_read_userdata=&c};ObserveSrtRead(r,SrtReadKind::Scalar,0x1000,8,words,true);Check(c.Status()==SrtReadCaptureStatus::InvalidSpan,"mismatched payload accepted");}
 {SrtReadCapture c(Identity());const std::span<const uint32_t> huge(reinterpret_cast<const uint32_t*>(1),size_t(UINT64_MAX/4+1));SrtRuntime r{.post_read_observer=SrtReadCapture::Observe,.post_read_userdata=&c};ObserveSrtRead(r,SrtReadKind::Scalar,0x1000,4,huge,true);Check(c.Status()==SrtReadCaptureStatus::InvalidSpan,"overflowed span size reached payload");}
 {auto identity=Identity();identity.library_identity.clear();SrtReadCapture c(identity);Send(c,0x1000,words);c.Finalize(true);Check(c.Status()==SrtReadCaptureStatus::MissingIdentity&&!c.Complete()&&c.Words().empty(),"missing library identity accepted");}
 {auto identity=Identity();identity.pass_id=0;SrtReadCapture c(identity);c.Finalize(true);Check(c.Status()==SrtReadCaptureStatus::MissingIdentity&&!c.Complete(),"missing pass identity accepted");}
 {auto identity=Identity();SrtReadCapture c(identity);identity.user_data[0]=0;c.Finalize(false);Check(c.Identity().user_data[0]==0xda7c00&&c.Status()==SrtReadCaptureStatus::MaterializationFailed,"identity not owned/failed finalization accepted");}
 {SrtReadCapture c(Identity());Send(c,0x1000,words);c.Finalize(true);c.Finalize(false);Send(c,0x2000,words);Check(c.Complete()&&c.Words().size()==1&&c.Counters().hooks==1,"finalized immutable snapshot modified");}
 {SrtReadCapture c(Identity());Send(c,0x1234567812345000ull,words);c.Finalize(true);Check(c.Complete()&&c.Words().begin()->first==0x1234567812345000ull,"full64 observed address masked");}
}
void TestScalarTablePrefix(){
 Program p;p.stage=Libs::Graphics::ShaderType::Compute;p.srt_plan_complete=true;p.resource_tracking_complete=true;
 DescriptorSource table;table.dword_count=2;table.dwords={Value(0x9000u),Value(0u)};p.descriptor_sources.push_back(table);
 DescriptorSource root;root.dword_count=8;root.indirect_descriptor.emplace();root.indirect_descriptor->table_source=0;
 root.indirect_descriptor->table_stride=32;root.indirect_descriptor->key_count=Value(1u);p.descriptor_sources.push_back(root);
 ImageResource image;image.source=1;image.dimension=Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D;
 image.numeric_class=Libs::Graphics::Prospero::TextureNumericClass::Float;image.resource_class=ImageResourceClass::Sampled;
 p.info.images.push_back(image);
 auto plan=ExtractResourcePlan(p);std::vector<std::pair<char,uint64_t>> baseline;ResourceSnapshot previous;
 for(bool observe:{false,true}){
  Reads reads;SrtReadCapture capture(Identity());
  SrtRuntime r{.read_memory=Reads::Ordinary,.userdata=&reads,.read_specialization_memory=Reads::Strict,
   .post_read_observer=observe?SrtReadCapture::Observe:nullptr,.post_read_userdata=&capture};
  ResourceSnapshot snapshot;ResourceSpecialization specialization;
  const bool okay=MaterializeResources(plan,r,snapshot,specialization);capture.Finalize(okay);
  Check(okay,"scalar table fixture failed materialization");
  if(!observe){baseline=reads.order;previous=std::move(snapshot);Check(capture.Counters().hooks==0,"scalar table flagoff captured");}
  else {Check(reads.order==baseline&&snapshot.images==previous.images,"table observer changed callbacks/payloads");Check(capture.Complete()&&capture.Words().size()==8&&capture.Observations().size()==1&&capture.Observations()[0].kind==SrtReadKind::ScalarTable&&capture.Observations()[0].requested_bytes==32&&capture.Observations()[0].specialization_read&&!capture.Observations()[0].native_pc&&!capture.Observations()[0].context,"table exact prefix/unknown context changed");}
 }
}
void TestVectorActiveAndInactive(){
 Program p;auto& b=AddBlock(p);p.memory_info.push_back({.kind=ResourceKind::Buffer});
 auto& h=b.AppendNewInst(ValueOpcode::GetBufferResource,{Value(0x8004u),Value(4u<<16),Value(4u),Value(1u<<12)});
 auto& active=b.AppendNewInst(ValueOpcode::LoadBufferU32,{Value(&h),Value(0u),Value(0u),Value(0u),Value(true)});active.SetFlags(MemoryFlags{.index=0,.pc=0x900});
 auto& inactive=b.AppendNewInst(ValueOpcode::LoadBufferU32,{Value(&h),Value(0u),Value(0u),Value(0u),Value(false)});inactive.SetFlags(MemoryFlags{.index=0,.pc=0x908});
 Reads reads;SrtReadCapture capture(Identity());SrtRuntime r{.read_memory=Reads::Ordinary,.userdata=&reads,.read_specialization_memory=Reads::Strict,.post_read_observer=SrtReadCapture::Observe,.post_read_userdata=&capture};
 uint32_t result=99;SrtWalker evaluator(p,r);Check(evaluator.Evaluate(Value(&inactive),result)&&result==0&&reads.order.empty()&&capture.Counters().hooks==0,"inactive vector created invented read");
 Check(evaluator.Evaluate(Value(&active),result)&&result==(0x8004u^uint32_t('s'))&&reads.order==std::vector<std::pair<char,uint64_t>>{{'s',0x8004}},"active vector reader changed");
 capture.Finalize(true);Check(capture.Complete()&&capture.Observations()[0].kind==SrtReadKind::VectorBuffer&&capture.Observations()[0].specialization_read,"vector association missing");
}
void TestScalarBufferAndUnknownPc(){
 Program p;auto& b=AddBlock(p);p.memory_info.push_back({.kind=ResourceKind::ScalarBuffer,.offset=4});
 auto& h=b.AppendNewInst(ValueOpcode::GetBufferResource,{Value(0x7000u),Value(4u<<16),Value(4u),Value(1u<<12)});
 auto& value=b.AppendNewInst(ValueOpcode::ReadConstBuffer,{Value(&h),Value(0u)});value.SetFlags(MemoryFlags{.index=0,.pc=0x910});
 Reads reads;SrtReadCapture capture(Identity());SrtRuntime r{.read_memory=Reads::Ordinary,.userdata=&reads,.read_specialization_memory=Reads::Strict,.post_read_observer=SrtReadCapture::Observe,.post_read_userdata=&capture};
 uint32_t result=0;Check(SrtWalker(p,r).Evaluate(Value(&value),result)&&result==(0x7004u^uint32_t('o')),"scalar buffer changed value");
 Check(capture.Observations()[0].kind==SrtReadKind::ScalarBuffer&&!capture.Observations()[0].native_pc,"nonprobe possibly relocated PC claimed native");
 p.info.uses_external_call_probe=true;value.SetFlags(MemoryFlags{.index=0,.pc=0});
 Check(SrtWalker(p,r).Evaluate(Value(&value),result)&&!capture.Observations()[1].native_pc,"default/zero PC claimed explicit native location");
 capture.Finalize(true);Check(capture.Complete(),"valid scalar-buffer capture rejected");
}
}
int main(){
 const std::pair<const char*,void(*)()> tests[]={{"reader-order-result",TestReaderOrderAndResults},{"ordinary-memcpy",TestNullOrdinaryReader},{"clean-context-memo",TestCleanAndContext},{"failed-read-invalidation",TestFailureAndNoCaptureAfterInvalidation},{"overlap-conflict",TestOverlapConflict},{"hard-caps",TestCaps},{"invalidation-original-reads",TestInvalidObserverDoesNotStopOriginalReads},{"malformed-identity-finalization",TestMalformedAndIdentity},{"scalar-table-prefix",TestScalarTablePrefix},{"vector-active-inactive",TestVectorActiveAndInactive},{"scalar-buffer-unknown-PC",TestScalarBufferAndUnknownPc}};
 for(const auto& [name,test]:tests){test();std::printf("PASS:%s\n",name);std::fflush(stdout);}
 std::puts("PASS:11 groups; authored pure CPU reads only; no guest process/Vulkan/GPU/files in collector");
}
