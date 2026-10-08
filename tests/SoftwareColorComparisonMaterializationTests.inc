using namespace Libs::Graphics::ShaderRecompiler;
using namespace Libs::Graphics::ShaderRecompiler::IR;

static Program ColorCompareProgram(bool linear = true, bool live_color = true,
                                   uint32_t extra_flags = 0u, bool status = false) {
  Program program;
  program.stage=Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete=program.resource_tracking_complete=true;
  auto& block=AddValueBlock(program);
  const auto AddSource=[&](const std::vector<uint32_t>& words) {
    DescriptorSource source; source.dword_count=words.size();
    for(size_t i=0;i<words.size();++i) source.dwords[i]=Value(words[i]);
    program.descriptor_sources.push_back(source);
    return uint32_t(program.descriptor_sources.size()-1u);
  };
  constexpr auto color=Libs::Graphics::Prospero::BufferFormat::k8UNorm;
  constexpr auto depth=Libs::Graphics::Prospero::BufferFormat::k16UNorm;
  constexpr auto ordinary=Libs::Graphics::Prospero::BufferFormat::k32Float;
  const auto SamplerWords=std::vector<uint32_t>{6u|(6u<<3u)|(3u<<12u),0x00fff000u,
      linear ? ((1u<<20u)|(1u<<22u)|(2u<<24u)) : 0u, 2u<<30u};
  const uint32_t sampler_source=AddSource(SamplerWords);
  program.info.samplers.push_back({.source=sampler_source,.depth_compare=true});
  auto& sampler=block.AppendNewInst(ValueOpcode::GetSamplerResource,
      {Value(SamplerWords[0]),Value(SamplerWords[1]),Value(SamplerWords[2]),Value(SamplerWords[3])});
  const std::array formats{color,depth,ordinary};
  for(uint32_t index=0;index<3u;++index) {
    const auto words=std::vector<uint32_t>{0x10000u+index*0x100u,
      uint32_t(formats[index])<<20u,0u,
      uint32_t(Libs::Graphics::Prospero::ImageType::kColor2D)<<28u,0u,0u,0u,0u};
    const auto source=AddSource(words);
    program.info.images.push_back({.source=source,.resource_class=ImageResourceClass::Sampled,
      .numeric_class=Libs::Graphics::Prospero::TextureNumericClass::Float,
      .dimension=Decoder::ImageDimension::Dim2D,.read=true,.depth_compare=index<2u});
    program.info.sampled_pairs.push_back({.image=index,.sampler=0u,.first_use_pc=index*16u});
    if(index==0u&&!live_color) continue;
    std::array<Value,8> args;for(size_t i=0;i<args.size();++i)args[i]=Value(words[i]);
    auto& image=block.AppendNewInst(ValueOpcode::GetImageResource,
      {args[0],args[1],args[2],args[3],args[4],args[5],args[6],args[7]});
    image.SetFlags(index);
    auto& address=block.AppendNewInst(ValueOpcode::MakeImageAddress,
      {Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),
       Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u)});
    MemoryInfo memory;
    memory.kind=ResourceKind::Image;memory.resource=index;memory.sampler=0u;
    memory.dmask=1u;memory.data_bits=32u;memory.image_address_components=index<2u?3u:2u;
    memory.image_dimension=Decoder::ImageDimension::Dim2D;
    memory.image_sample_flags=index<2u?Decoder::ImageSampleFlagCompare|Decoder::ImageSampleFlagLevelZero:0u;
    if(index==0u) {memory.image_sample_flags|=extra_flags;memory.image_sample_status=status;}
    const auto memory_index=uint32_t(program.memory_info.size());
    program.memory_info.push_back(memory);
    auto& sample=block.AppendNewInst(ValueOpcode::ImageSampleRaw,
      {Value(&image),Value(&sampler),Value(&address)});
    sample.SetFlags(MemoryFlags{.index=memory_index,.pc=index*16u});
  }
  // Unused malformed image metadata must never change the active consumer proof.
  MemoryInfo dead; dead.kind=ResourceKind::Image;dead.resource=99u;dead.image_sample_flags=~0u;
  program.memory_info.push_back(dead);
  return program;
}

static unsigned color_policy_checks=0;
static void ColorCheck(bool condition,const char* reason) {++color_policy_checks;Check(condition,reason);}
static void TestSoftwareColorComparisonMaterialization() {
  for(const bool linear: {false,true}) {
    auto program=ColorCompareProgram(linear);
    auto plan=ExtractResourcePlan(program);
    ResourceSnapshot snapshot;ResourceSpecialization specialization;
    ColorCheck(MaterializeResources(plan,{},snapshot,specialization),"color policy authored materialization");
    const auto original=specialization;
    const auto descriptors=snapshot.images;
    const auto samplers=snapshot.samplers;
    ColorCheck(ConfigureSoftwareColorComparison(plan,snapshot,specialization,false,false)&&specialization==original,
               "disabled policy specialization exact equality");
    ColorCheck(ConfigureSoftwareColorComparison(plan,snapshot,specialization,true,true)&&specialization==original,
               "supported-device R8 remains original specialization");
    ColorCheck(ConfigureSoftwareColorComparison(plan,snapshot,specialization,true,false),"strict R8 fallback admission");
    ColorCheck(specialization.images[0].comparison_mode==(linear?ImageComparisonMode::SoftwareLinearR8:ImageComparisonMode::SoftwarePointR8),
               "actual filter mode retained");
    ColorCheck(specialization.images[1].comparison_mode==ImageComparisonMode::Native&&specialization.images[2].comparison_mode==ImageComparisonMode::Native,
               "native D16 and ordinary float images unchanged");
    ColorCheck(snapshot.images==descriptors&&snapshot.samplers==samplers,"configuration does not mutate raw snapshots");
    ApplyResourceSpecialization(program,specialization);
    ColorCheck(program.info.samplers.size()==2u&&program.info.samplers[0].snapshot_index==0u&&program.info.samplers[1].snapshot_index==0u,
               "two variants retain exact one original C# source");
    ColorCheck(!program.info.samplers[program.memory_info[0].sampler].depth_compare,
               "software ordinary sample uses noncomparison sampler");
    ColorCheck(program.info.samplers[program.memory_info[1].sampler].depth_compare,
               "shared native D16 sampler stays comparison enabled");
    ColorCheck(!program.info.samplers[program.memory_info[2].sampler].depth_compare,
               "shared ordinary image uses noncomparison variant");
    ColorCheck(program.memory_info[0].sampler==program.memory_info[2].sampler&&
        program.memory_info[0].sampler!=program.memory_info[1].sampler,"consumer remap agrees with source identity");
  }
  auto dead_program=ColorCompareProgram(true,false);auto dead_plan=ExtractResourcePlan(dead_program);
  ResourceSnapshot dead_snapshot;ResourceSpecialization dead_specialization;
  ColorCheck(MaterializeResources(dead_plan,{},dead_snapshot,dead_specialization),"dead comparison metadata materializes normally");
  const auto before=dead_specialization;
  ColorCheck(ConfigureSoftwareColorComparison(dead_plan,dead_snapshot,dead_specialization,true,false)&&dead_specialization==before,
             "unused R8 metadata ignored without inventing a consumer");
  for(uint32_t flags:{Decoder::ImageSampleFlagOffset,Decoder::ImageSampleFlagLodClamp,Decoder::ImageSampleFlagA16,Decoder::ImageSampleFlagCd}) {
    auto invalid=ColorCompareProgram(true,true,flags);auto plan=ExtractResourcePlan(invalid);
    ResourceSnapshot snapshot;ResourceSpecialization specialization;
    ColorCheck(MaterializeResources(plan,{},snapshot,specialization),"unsupported sample metadata still materializes");
    ColorCheck(!ConfigureSoftwareColorComparison(plan,snapshot,specialization,true,false),"unsupported live sample policy rejects");
  }
  auto status=ColorCompareProgram(true,true,0u,true);auto status_plan=ExtractResourcePlan(status);
  ResourceSnapshot status_snapshot;ResourceSpecialization status_specialization;
  ColorCheck(MaterializeResources(status_plan,{},status_snapshot,status_specialization),"authored status resource materializes");
  ColorCheck(!ConfigureSoftwareColorComparison(status_plan,status_snapshot,status_specialization,true,false),"TFE/LWE policy rejects");
  // One retained plan and one reused output across changing descriptor values.
  for(const auto native_format: {Libs::Graphics::Prospero::BufferFormat::k16UNorm,
                                  Libs::Graphics::Prospero::BufferFormat::k32Float}) {
  auto warm_program=ColorCompareProgram(true);auto warm_plan=ExtractResourcePlan(warm_program);
  ResourceSnapshot warm_snapshot;ResourceSpecialization warm_specialization;
  ColorCheck(MaterializeResources(warm_plan,{},warm_snapshot,warm_specialization)&&
      ConfigureSoftwareColorComparison(warm_plan,warm_snapshot,warm_specialization,true,false)&&
      warm_specialization.software_color_dref_enabled,"initial warm R8 software mode");
  const auto image_source=warm_plan.info.images[0].source;
  const auto original_word=warm_plan.descriptor_sources[image_source].dwords[1];
  warm_plan.descriptor_sources[image_source].dwords[1]=Value(
      uint32_t(native_format)<<20u);
  ColorCheck(MaterializeResources(warm_plan,{},warm_snapshot,warm_specialization),"warm R8 to native rematerializes");
  ColorCheck(!warm_specialization.software_color_dref_enabled,
      "native materialization resets software mode before initial alias construction");
  ColorCheck(ConfigureSoftwareColorComparison(warm_plan,warm_snapshot,warm_specialization,true,false)&&
      !warm_specialization.software_color_dref_enabled&&
      warm_specialization.images[0].comparison_mode==ImageComparisonMode::Native,"warm native mode remains native");
  ResourceSnapshot cold_snapshot;ResourceSpecialization cold_specialization;
  ColorCheck(MaterializeResources(warm_plan,{},cold_snapshot,cold_specialization)&&
      ConfigureSoftwareColorComparison(warm_plan,cold_snapshot,cold_specialization,true,false),"cold native control");
  ColorCheck(warm_specialization==cold_specialization,"warm native specialization equals cold native including aliases");
  warm_plan.descriptor_sources[image_source].dwords[1]=original_word;
  ColorCheck(MaterializeResources(warm_plan,{},warm_snapshot,warm_specialization)&&
      ConfigureSoftwareColorComparison(warm_plan,warm_snapshot,warm_specialization,true,false),"warm native to R8 rematerializes");
  cold_specialization={};cold_snapshot={};
  ColorCheck(MaterializeResources(warm_plan,{},cold_snapshot,cold_specialization)&&
      ConfigureSoftwareColorComparison(warm_plan,cold_snapshot,cold_specialization,true,false),"cold R8 control");
  ColorCheck(warm_specialization==cold_specialization&&warm_specialization.software_color_dref_enabled&&
      warm_specialization.images[0].comparison_mode==ImageComparisonMode::SoftwareLinearR8,
      "warm R8 mode and all aliases equal cold R8 after roundtrip");
  }
  auto failed_program=ColorCompareProgram(true);auto failed_plan=ExtractResourcePlan(failed_program);
  failed_plan.info.images[0].source=UINT32_MAX;
  ResourceSnapshot failed_snapshot;ResourceSpecialization failed_specialization;
  failed_specialization.software_color_dref_enabled=true;
  ColorCheck(!MaterializeResources(failed_plan,{},failed_snapshot,failed_specialization)&&
      !failed_specialization.software_color_dref_enabled,"early materialization failure preserves failure and resets policy state");
  std::printf("SoftwareColorComparison production materialization PASS checks=%u GPU=0\n",color_policy_checks);
}
