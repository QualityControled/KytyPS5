// Private WORK fixture. Only authored pixels and guest instruction words; no game payload.
// Manual Vulkan shader/descriptor ABI proof, not production TextureCache import proof.
namespace {
using namespace Libs::Graphics;

bool OwnedCompareBoolean(float reference,float texel,u32 function,bool unorm=true) {
  // Independent expected-value table. Endpoint texels avoid UNORM equality uncertainty.
  if(unorm)reference=std::clamp(reference,0.0f,1.0f);
  switch(function) {
    case 0:return false;case 1:return reference<texel;case 2:return reference==texel;
    case 3:return reference<=texel;case 4:return reference>texel;case 5:return reference!=texel;
    case 6:return reference>=texel;case 7:return true;
    default:Fail("OwnedR8Comparison","oracle","invalid comparison function");
  }
  return false;
}

float OwnedR8Expected(bool linear,float u,float v,float reference,u32 cmp,u32 red,
                      bool border,bool white,bool unorm=true,bool repeat=false) {
  const auto compare_texel=[&](int x,int y) {
    if(border && (x<0 || y<0 || x>=2 || y>=2))
      return float(OwnedCompareBoolean(reference,white?1.0f:0.0f,cmp,unorm));
    if(repeat){x=(x%2+2)%2;y=(y%2+2)%2;}
    else{x=std::clamp(x,0,1);y=std::clamp(y,0,1);}
    constexpr float pixels[4]={0.0f,1.0f,1.0f,0.0f};
    return float(OwnedCompareBoolean(reference,pixels[y*2+x],cmp,unorm));
  };
  float value=0.0f;
  if(linear) {
    const float x=u*2.0f-0.5f,y=v*2.0f-0.5f;
    const int ix=int(std::floor(x)),iy=int(std::floor(y));
    const float fx=x-std::floor(x),fy=y-std::floor(y);
    // Native gather components are i0j1,i1j1,i1j0,i0j0. Oracle uses row-major cells.
    const float top=(1.0f-fx)*compare_texel(ix,iy)+fx*compare_texel(ix+1,iy);
    const float bottom=(1.0f-fx)*compare_texel(ix,iy+1)+fx*compare_texel(ix+1,iy+1);
    value=(1.0f-fy)*top+fy*bottom;
  } else value=compare_texel(int(std::floor(u*2.0f)),int(std::floor(v*2.0f)));
  if(red==0u)return 0.0f;if(red==1u)return 1.0f;
  Require("OwnedR8Comparison","oracle",red==4u,"unsupported projection in authored oracle");
  return value;
}

TestCase MakeOwnedR8Compare(bool linear,u32 cmp,u32 swizzle,bool border,bool white,
                            u32 wave,u32 dmask,Prospero::BufferFormat format=Prospero::BufferFormat::k8UNorm,bool repeat=false) {
  TestCase test;
  test.name="OwnedR8Comparison";test.has_user_data=true;
  test.owned_software_color_dref=true;
  test.user_data=MakeSampledTextureData(format);
  test.image_width=test.image_height=2u;
  test.user_data[1]|=1u<<30u;test.user_data[2]=1u<<14u;
  test.image_descriptor_swizzle=swizzle;
  const auto clamp=repeat?0u:border?6u:2u;
  test.user_data[8]=clamp|(clamp<<3u)|(clamp<<6u)|(cmp<<12u);
  test.user_data[10]=(1u<<24u)|(linear?((1u<<20u)|(1u<<22u)):0u);
  test.user_data[11]=white?(2u<<30u):0u;
  test.compute_info.threads_num[0]=test.compute_info.threads_num[1]=test.compute_info.threads_num[2]=1u;
  test.compute_info.wave_size=wave;test.has_compute_info=true;
  test.opcodes={ShaderOpcode::V_MOV_B32,ShaderOpcode::IMAGE_SAMPLE,
                ShaderOpcode::BUFFER_STORE_DWORD,ShaderOpcode::S_ENDPGM};
  // No NaN expected value is invented. Infinity has a defined UNORM clamp endpoint.
  const std::array<float,7> references={-std::numeric_limits<float>::infinity(),-.5f,0.0f,.75f,1.0f,1.5f,std::numeric_limits<float>::infinity()};
  const std::array<std::array<float,2>,5> positions={{{.5f,.5f},{.375f,.625f},{0.0f,.5f},{-.5f,.5f},{-.125f,.25f}}};
  const u32 components=dmask==15u?4u:1u;
  for(const auto& position:positions)for(const auto reference:references) {
    AppendVMovLiteral(&test.code,20,std::bit_cast<u32>(reference));
    AppendVMovLiteral(&test.code,21,std::bit_cast<u32>(position[0]));
    AppendVMovLiteral(&test.code,22,std::bit_cast<u32>(position[1]));
    test.code.push_back(EncodeMimg0(0x2fu,dmask,0,false,1));
    test.code.push_back(EncodeMimg1(4u,20u,0u,2u));
    const float expected=OwnedR8Expected(linear,position[0],position[1],reference,cmp,swizzle&7u,border,white,format!=Prospero::BufferFormat::k32Float,repeat);
    for(u32 component=0;component<components;++component) {
      AppendStoreVgpr(&test.code,4u+component,test.expected.size());
      test.expected.push_back(std::bit_cast<u32>(expected));
    }
  }
  AppendEnd(&test.code);
  test.expected.insert(test.expected.end(),4u,0x9172ac45u);
  test.initial.assign(test.expected.size(),0x9172ac45u);
  test.user_data[50]=test.expected.size()*sizeof(u32);test.user_data[51]=3u<<28u;
  test.expected_float_tolerance=0.00001f;
  if(format==Prospero::BufferFormat::k8UNorm) {
    test.required_spirv={linear?"OpImageGather":"OpImageSampleExplicitLod"};
    test.forbidden_spirv={"OpImageSampleDref","OpImageDrefGather"};
  } else test.required_spirv={"OpImageSampleDrefExplicitLod"};
  return test;
}

void DumpOwnedR8Identity(const char* prefix) {
  u32 id=0;
  for(const auto format:{Prospero::BufferFormat::k8UNorm,Prospero::BufferFormat::k16UNorm,Prospero::BufferFormat::k32Float})
    for(const bool linear:{false,true}) {
      auto test=MakeOwnedR8Compare(linear,3u,format==Prospero::BufferFormat::k8UNorm?DstSel(4,5,6,7):DstSel(4,4,4,4),false,false,32u,1u,format);
      test.owned_software_color_dref=false;test.required_spirv.clear();test.forbidden_spirv.clear();
      const auto compiled=CompileCase(test,32u);
      const std::string path=std::string(prefix)+"-"+std::to_string(id++)+".spv";
      auto* file=std::fopen(path.c_str(),"wb");Require(test.name,"identity artifact",file!=nullptr,"cannot create fresh output module");
      const auto written=std::fwrite(compiled.spirv.data(),sizeof(u32),compiled.spirv.size(),file);
      const int closed=std::fclose(file);Require(test.name,"identity artifact",written==compiled.spirv.size() && closed==0,"module write failed");
      std::printf("OwnedR8Identity: format=%u linear=%u words=%zu optin=0 default_spirv_validated=1 path=%s\n",u32(format),linear,compiled.spirv.size(),path.c_str());
    }
  #ifndef OWNED_R8_BASELINE
  // Enabling the production classifier for supported native depth must preserve exact words too.
  for(const auto format:{Prospero::BufferFormat::k16UNorm,Prospero::BufferFormat::k32Float})
    for(const bool linear:{false,true}) {
      auto test=MakeOwnedR8Compare(linear,3u,DstSel(4,4,4,4),false,false,32u,1u,format);
      test.required_spirv.clear();test.forbidden_spirv.clear();
      const auto enabled=CompileCase(test,32u);test.owned_software_color_dref=false;
      const auto disabled=CompileCase(test,32u);
      Require(test.name,"native depth optin isolation",enabled.spirv==disabled.spirv,"native depth changed under enabled classifier");
    }
  #endif
}

#ifndef OWNED_R8_BASELINE
void CheckOwnedR8Comparison(VulkanHarness* vulkan) {
  Require("OwnedR8Comparison","compare-before-filter discriminator",
          OwnedR8Expected(true,.5f,.5f,.75f,3u,4u,false,false)==.5f &&
          OwnedR8Expected(true,.375f,.625f,.75f,3u,4u,false,false)==.625f &&
          !OwnedCompareBoolean(.75f,.5f,3u),"fixture cannot distinguish compare-before-filter from compare-filtered-depth");
  Require("OwnedR8Comparison","fractional border discriminator",
          OwnedR8Expected(true,0,.5f,.75f,3u,4u,true,false)==.25f &&
          OwnedR8Expected(true,0,.5f,.75f,3u,4u,true,true)==.75f,"authored border weights differ");
  Require("OwnedR8Comparison","repeat-before-filter discriminator",
      OwnedR8Expected(false,-.125f,.25f,.75f,3u,4u,false,false,true,true)==1.0f &&
      OwnedR8Expected(false,-.125f,.25f,.75f,3u,4u,false,false)==0.0f &&
      OwnedR8Expected(true,-.125f,.25f,.75f,3u,4u,false,false,true,true)==.75f,
      "repeat test cannot distinguish wrapping from clamping");
  u32 modules=0,dispatches=0;
  const bool native_r8=vulkan!=nullptr && vulkan->OwnedR8NativeComparisonSupported();
  Require("OwnedR8Comparison","physical format comparison capability",!native_r8,
          "owned software-path GPU fixture requires the actual R8 format to lack native depth comparison");
  const auto run=[&](bool linear,u32 cmp,u32 swizzle,bool border,bool white,u32 wave,u32 dmask,
                     Prospero::BufferFormat format=Prospero::BufferFormat::k8UNorm,bool repeat=false) {
    auto test=MakeOwnedR8Compare(linear,cmp,swizzle,border,white,wave,dmask,format,repeat);
    test.owned_r8_native_comparison_supported=native_r8;
    auto compiled=CompileCase(test,vulkan?vulkan->SubgroupSize():32u);
    const bool r8=format==Prospero::BufferFormat::k8UNorm;
    const u32 expected_mode=r8?(linear?2u:1u):0u;
    Require(test.name,"production comparison mode",compiled.program.info.images.size()==1u &&
            static_cast<u32>(compiled.program.info.images[0].comparison_mode)==expected_mode,
            "production configuration failed to retain exact comparison mode");
    Require(test.name,"production sampler class",compiled.program.info.samplers.size()==1u &&
            compiled.program.info.samplers[0].depth_compare==!r8,
            "software source sampler or native depth comparison class was conflated");
    if(r8 && linear && cmp==3u && swizzle==DstSel(4,5,6,7) && !border && !repeat && wave==32u && dmask==1u) {
      Require(test.name,"live C# CPU operand",std::ranges::find(compiled.program.bindings.user_data_registers,8u)!=compiled.program.bindings.user_data_registers.end(),
          "software comparator does not consume runtime raw sampler first word");
    }
    ++modules;
    if(vulkan) {
      // R8 four authored bytes are copied tightly by VkBufferImageCopy, despite padded u32 staging.
      std::vector<u32> pixels=r8?std::vector<u32>{0x00ffff00u,0u,0u,0u}:
          format==Prospero::BufferFormat::k16UNorm?std::vector<u32>{0xffff0000u,0x0000ffffu,0u,0u}:
          std::vector<u32>{0u,0x3f800000u,0x3f800000u,0u};
      const auto vk_format=r8?vk::Format::eR8Unorm:
          format==Prospero::BufferFormat::k16UNorm?vk::Format::eD16Unorm:vk::Format::eD32Sfloat;
      auto image=vulkan->CreateImageMips(test.name,2u,2u,vk_format,vk::ImageUsageFlagBits::eSampled,
          {pixels},1u,vk::ImageLayout::eShaderReadOnlyOptimal,vk::ImageType::e2D,vk::ImageViewType::e2D,
          1u,0u,1u,vk::SampleCountFlagBits::e1,vk::Format::eUndefined,0u,
          r8?vk::ImageAspectFlagBits::eColor:vk::ImageAspectFlagBits::eDepth);
      const auto sampler=vulkan->OwnedColorComparisonSampler(test.name,linear,!r8,cmp,border,white,repeat);
      auto output=vulkan->CreateStorageBuffer(test.name,test.initial,test.initial.size());
      vulkan->Dispatch(test,compiled,output,nullptr,&image,nullptr,nullptr,sampler);
      const auto actual=vulkan->ReadBuffer(test.name,output,test.expected.size());
      Require(test.name,"exact output guards",actual.size()==test.expected.size() &&
          std::equal(actual.end()-4,actual.end(),test.expected.end()-4),
          "shader changed output sentinels outside authored writes");
      CompareWords(test,"actual format/native C_LZ comparison output",test.expected,actual);
      vulkan->DestroyBuffer(&output);vulkan->Device().destroySampler(sampler);vulkan->DestroyImage(&image);
      ++dispatches;

      // Same compiled SPIR-V words consume a changed runtime raw comparison function.
      // Dispatch builds a new owned VkPipeline each time: no warm-driver-cache claim.
      if(r8 && linear && cmp==3u && swizzle==DstSel(4,5,6,7) && !border && !repeat && wave==32u && dmask==1u) {
        const auto found=std::ranges::find(compiled.program.bindings.user_data_registers,8u);
        Require(test.name,"live raw sampler first word",found!=compiled.program.bindings.user_data_registers.end(),
                "comparison function was not retained as live input user-data SGPR8");
        const auto packed=found-compiled.program.bindings.user_data_registers.begin();
        const auto original=compiled.packed_user_data[packed];
        const auto words=compiled.spirv;
        for(const u32 runtime_cmp:{1u,4u}) {
          compiled.packed_user_data[packed]=(original&~(7u<<12u))|(runtime_cmp<<12u);
          auto changed=MakeOwnedR8Compare(true,runtime_cmp,swizzle,false,false,32u,1u);
          auto texture=vulkan->CreateImageMips(test.name,2u,2u,vk::Format::eR8Unorm,
              vk::ImageUsageFlagBits::eSampled,{{0x00ffff00u,0u,0u,0u}},1u,
              vk::ImageLayout::eShaderReadOnlyOptimal,vk::ImageType::e2D,vk::ImageViewType::e2D,1u);
          const auto ordinary=vulkan->OwnedColorComparisonSampler(test.name,true,false,runtime_cmp,false,false);
          auto changed_output=vulkan->CreateStorageBuffer(test.name,changed.initial,changed.initial.size());
          vulkan->Dispatch(changed,compiled,changed_output,nullptr,&texture,nullptr,nullptr,ordinary);
          const auto values=vulkan->ReadBuffer(test.name,changed_output,changed.expected.size());
          Require(test.name,"runtime CMP exact guards",std::equal(values.end()-4,values.end(),changed.expected.end()-4),
                  "runtime function update touched output guards");
          CompareWords(changed,"changed runtime raw C# with byte-identical compiled module",changed.expected,values);
          Require(test.name,"reused compiled module",compiled.spirv==words,"runtime C# update changed module words");
          vulkan->DestroyBuffer(&changed_output);vulkan->Device().destroySampler(ordinary);vulkan->DestroyImage(&texture);
          ++dispatches;
          std::printf("OwnedR8ComparisonRuntimeCmp: initial=3 changed=%u packed_register=8 compiled_words_identical=1 new_owned_pipeline_per_dispatch=1\n",runtime_cmp);
        }
        compiled.packed_user_data[packed]=original;
      }
    }
    std::printf("OwnedR8ComparisonCase: linear=%u cmp=%u swizzle=%03x border=%u white=%u wave=%u dmask=%u format=%u words=%zu scope=%s\n",
      linear,cmp,swizzle,border,white,wave,dmask,u32(format),test.expected.size(),vulkan?"owned_manual_gpu_abi":"normal_production_cpu_emit_only");
  };
  for(const bool linear:{false,true})for(u32 cmp=0;cmp<8u;++cmp)
    run(linear,cmp,DstSel(4,5,6,7),false,false,32u,1u);
  for(const bool linear:{false,true}) {
    run(linear,3u,DstSel(0,5,6,7),false,false,32u,15u);
    run(linear,4u,DstSel(1,0,0,4),false,false,32u,15u);
    run(linear,3u,DstSel(4,1,0,4),false,false,64u,15u);
    run(linear,3u,DstSel(4,5,6,7),true,false,32u,1u);
    run(linear,3u,DstSel(4,5,6,7),true,true,32u,1u);
    run(linear,3u,DstSel(4,5,6,7),false,false,32u,1u,Prospero::BufferFormat::k8UNorm,true);
  }
  // Supported native depth formats stay Dref/compare-enabled with identical authored raw C# function.
  for(const auto format:{Prospero::BufferFormat::k16UNorm,Prospero::BufferFormat::k32Float})
    run(false,3u,DstSel(4,4,4,4),false,false,32u,1u,format);

  // One raw S# is consumed by BOTH a software R8 and native D32 sample in one module.
  // This is production specialization/binding splitting, not a policy-enum mirror.
  {
    auto test=MakeOwnedR8Compare(true,3u,DstSel(4,5,6,7),false,false,32u,1u);
    std::copy_n(test.user_data.begin(),8u,test.user_data.begin()+16u);
    test.user_data[17]=(test.user_data[17]&~(0x1ffu<<20u))|
        (static_cast<u32>(Prospero::BufferFormat::k32Float)<<20u);
    Require(test.name,"authored native program",test.code.back()==0xbf810000u,"missing original end");
    test.code.pop_back();
    AppendVMovLiteral(&test.code,20,std::bit_cast<u32>(.5f));
    AppendVMovLiteral(&test.code,21,std::bit_cast<u32>(.75f));
    AppendVMovLiteral(&test.code,22,std::bit_cast<u32>(.25f));
    test.code.push_back(EncodeMimg0(0x2fu,1u,0,false,1));
    test.code.push_back(EncodeMimg1(4u,20u,4u,2u)); // Different T#, the identical original S#8.
    AppendStoreVgpr(&test.code,4u,test.expected.size()-4u);
    AppendEnd(&test.code);
    test.expected.insert(test.expected.end()-4,0x3f800000u);
    test.initial.assign(test.expected.size(),0x9172ac45u);
    test.user_data[50]=test.expected.size()*sizeof(u32);
    test.required_spirv={"OpImageGather","OpImageSampleDrefExplicitLod"};
    test.forbidden_spirv.clear();
    auto compiled=CompileCase(test,vulkan?vulkan->SubgroupSize():32u);
    Require(test.name,"mixed sampled images",compiled.program.info.images.size()==2u &&
        static_cast<u32>(compiled.program.info.images[0].comparison_mode)==2u &&
        static_cast<u32>(compiled.program.info.images[1].comparison_mode)==0u,
        "shared sampler conflated R8 and D32 image types");
    Require(test.name,"mixed original raw sampler variants",compiled.program.info.samplers.size()==2u &&
        compiled.program.info.samplers[0].snapshot_index==compiled.program.info.samplers[1].snapshot_index &&
        compiled.program.info.samplers[0].depth_compare!=compiled.program.info.samplers[1].depth_compare,
        "one raw C# was not split into ordinary and native comparison variants");
    ++modules;
    if(vulkan) {
      std::vector<VulkanHarness::Image> images;
      images.push_back(vulkan->CreateImageMips(test.name,2u,2u,vk::Format::eR8Unorm,
          vk::ImageUsageFlagBits::eSampled,{{0x00ffff00u,0u,0u,0u}},1u,
          vk::ImageLayout::eShaderReadOnlyOptimal,vk::ImageType::e2D,vk::ImageViewType::e2D,1u));
      images.push_back(vulkan->CreateImageMips(test.name,2u,2u,vk::Format::eD32Sfloat,
          vk::ImageUsageFlagBits::eSampled,{{0u,0x3f800000u,0x3f800000u,0u}},1u,
          vk::ImageLayout::eShaderReadOnlyOptimal,vk::ImageType::e2D,vk::ImageViewType::e2D,1u,
          0u,1u,vk::SampleCountFlagBits::e1,vk::Format::eUndefined,0u,vk::ImageAspectFlagBits::eDepth));
      std::vector<vk::Sampler> samplers;
      for(const auto& sampler:compiled.program.info.samplers)
        samplers.push_back(vulkan->OwnedColorComparisonSampler(test.name,true,sampler.depth_compare,3u,false,false));
      auto output=vulkan->CreateStorageBuffer(test.name,test.initial,test.initial.size());
      vulkan->Dispatch(test,compiled,output,nullptr,nullptr,nullptr,nullptr,samplers.front(),images,{},nullptr,samplers);
      const auto actual=vulkan->ReadBuffer(test.name,output,test.expected.size());
      Require(test.name,"mixed exact guards",std::equal(actual.end()-4,actual.end(),test.expected.end()-4),
              "mixed image shader overwrote a guard");
      CompareWords(test,"same raw S# native and software samples",test.expected,actual);
      vulkan->DestroyBuffer(&output);
      for(const auto sampler:samplers)vulkan->Device().destroySampler(sampler);
      for(auto& image:images)vulkan->DestroyImage(&image);
      ++dispatches;
    }
    std::puts("OwnedR8ComparisonSharedSampler: same_raw=1 native_D32=1 software_linear_R8=1 expected_native_tail=1");
  }

  std::printf("OwnedR8Comparison PASS: modules=%u dispatches=%u authored_source_only=1 no_guest_backing_init=1 no_production_texture_import_proof=1\n",modules,dispatches);
}
#endif
} // namespace
