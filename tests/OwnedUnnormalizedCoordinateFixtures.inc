// Authored native CPU fixtures. No Vulkan instance/device, guest mappings, or game bytes.
namespace Libs::Graphics {
namespace {
namespace UNormFixture {
namespace IR = ShaderRecompiler::IR;
namespace Decoder = ShaderRecompiler::Decoder;

struct Instructions {
  std::map<u32, std::vector<u32>> definitions;
  std::map<u32, u32> constants;
  std::vector<std::vector<u32>> instructions;
  explicit Instructions(const std::vector<u32>& words) {
    for (size_t i = 5; i < words.size();) {
      const u32 size = words[i] >> 16u;
      Require("UNormSpv", "instruction bounds", size != 0 && size <= words.size() - i,
              "invalid SPIR-V instruction extent");
      auto& row = instructions.emplace_back(words.begin() + i, words.begin() + i + size);
      const auto op = static_cast<spv::Op>(row[0] & 0xffffu);
      if (op == spv::OpConstant && size == 4) constants[row[2]] = row[3];
      if (op == spv::OpConstantTrue) constants[row[2]]=1u;
      if (op == spv::OpConstantFalse) constants[row[2]]=0u;
      if (size >= 3 && (op == spv::OpCompositeConstruct || op == spv::OpCompositeExtract ||
          op == spv::OpFDiv || op == spv::OpSelect || op == spv::OpImageQuerySizeLod ||
          op == spv::OpConvertUToF || op == spv::OpLoad || op == spv::OpAccessChain ||
          op == spv::OpBitwiseAnd || op == spv::OpINotEqual || op == spv::OpIEqual ||
          op == spv::OpSampledImage)) definitions[row[2]] = row;
      i += size;
    }
  }
  const std::vector<u32>& Def(u32 id) const {
    const auto it = definitions.find(id);
    Require("UNormSpv", "definition", it != definitions.end(), "missing operand definition");
    return it->second;
  }
  bool Op(u32 id, spv::Op op) const {
    const auto it = definitions.find(id);
    return it != definitions.end() && (it->second[0] & 0xffffu) == static_cast<u32>(op);
  }
  void ConvertedAxis(u32 value, bool native, bool indirect = false) const {
    if (!native) {
      const auto& select = Def(value);
      Require("UNormSpv", "dynamic select", Op(value, spv::OpSelect) && select.size() == 6,
              "normal/ForceUnorm coordinate must select between division and original");
      if (!indirect) {
        const auto& condition = Def(select[3]);
        Require("UNormSpv", "runtime force predicate", Op(select[3], spv::OpINotEqual),
                "sampler ForceUnorm condition lost");
        const auto& bits = Def(condition[3]);
        Require("UNormSpv", "exact original bit15", Op(condition[3], spv::OpBitwiseAnd) &&
                constants.contains(bits[4]) && constants.at(bits[4]) == 0x8000u,
                "coordinate predicate is not runtime original sampler bit15");
      } else {
        Require("UNormSpv", "selected candidate force", Op(select[3], spv::OpSelect),
                "indirect force did not select candidate metadata");
        const auto& force = Def(select[3]);
        Require("UNormSpv", "candidate ordinal comparison", Op(force[3], spv::OpIEqual),
                "indirect force uses key bits instead of selected ordinal");
      }
      value = select[4];
    }
    const auto& divide = Def(value);
    Require("UNormSpv", "spatial divide", Op(value, spv::OpFDiv), "spatial coordinate not divided");
    const auto& converted = Def(divide[4]);
    Require("UNormSpv", "native image extent", Op(divide[4], spv::OpConvertUToF),
            "denominator not the actual image-query extent");
    u32 size = converted[3];
    if (Op(size, spv::OpCompositeExtract)) size = Def(size)[3];
    const auto& query = Def(size);
    Require("UNormSpv", "view relative mip0", Op(size, spv::OpImageQuerySizeLod) &&
            constants.contains(query[4]) && constants.at(query[4]) == 0u,
            "normalization extent did not query native view-relative mip zero");
    const auto& load = Def(query[3]);
    Require("UNormSpv", "bound native descriptor", Op(query[3], spv::OpLoad),
            "normalization used guest descriptor dimensions instead of bound view");
    if (!indirect && Op(load[3], spv::OpAccessChain)) {
      const auto& access = Def(load[3]);
      Require("UNormSpv", "base mip descriptor", access.size() == 5 &&
              constants.contains(access[4]) && constants.at(access[4]) == 0u,
              "dynamic normalization selected a nonzero split-mip view");
    }
  }
  void Coordinate(u32 id, u32 spatial, u32 components, bool native, bool indirect = false) const {
    if (components == 1) { ConvertedAxis(id, native, indirect); return; }
    const auto& vector = Def(id);
    Require("UNormSpv", "coordinate shape", Op(id, spv::OpCompositeConstruct) &&
            vector.size() == 3u + components, "wrong coordinate component count");
    for (u32 axis = 0; axis < spatial; ++axis) ConvertedAxis(vector[3u + axis], native, indirect);
    for (u32 axis = spatial; axis < components; ++axis) {
      Require("UNormSpv", "array layer preserved", !Op(vector[3u + axis], spv::OpFDiv) &&
              !Op(vector[3u + axis], spv::OpSelect), "array layer was normalized");
    }
  }
};

TestCase MakeCase(u32 opcode, u32 dim, Prospero::ImageType type, bool force, bool native,
                  bool gradient = false, bool compare = false, bool mips = false) {
  TestCase test;
  test.name = "AuthoredUnnormalizedCoordinate";
  test.user_data = MakeSampledTextureData(compare ? Prospero::BufferFormat::k32Float :
                                          Prospero::BufferFormat::k32_32_32_32Float);
  test.user_data[1] |= 3u << 30u; // width8 (non-square): low two width-minus-one bits.
  test.user_data[2] = 1u | (3u << 14u); // width8, height4.
  test.user_data[3] = static_cast<u32>(type) << 28u;
  if (mips) test.user_data[3] |= (1u << 12u) | (2u << 16u); // view base1..2.
  test.user_data[4] = dim == 5u ? 2u : 0u; // three 2D-array layers.
  test.user_data[5] = mips ? 2u << 4u : 0u;
  test.user_data[8] = 0x92u | (force ? 1u << 15u : 0u) | (compare ? 3u << 12u : 0u);
  test.user_data[9] = 0xfff000u;
  test.user_data[10] = mips ? 1u << 26u : 0u;
  test.user_data[50] = 16u;
  test.user_data[51] = 3u << 28u;
  test.has_user_data = true;
  test.expected = {0u};
  test.opcodes = {ShaderOpcode::V_MOV_B32, ShaderOpcode::IMAGE_SAMPLE,
                  ShaderOpcode::BUFFER_STORE_DWORD, ShaderOpcode::S_ENDPGM};
  // Arbitrary distinct finite values make array/reference/LOD accidental division observable in SPV.
  const std::array values{0.625f, 0.375f, 2.0f, 0.125f, 0.0f, 0.0f, 0.25f, 1.5f};
  for (u32 i = 0; i < values.size(); ++i)
    AppendVMovLiteral(&test.code, 20u + i, std::bit_cast<u32>(values[i]));
  test.code.push_back(EncodeMimg0(opcode, 1u, 0u, false, dim) | (native ? 1u << 12u : 0u));
  test.code.push_back(EncodeMimg1(0u, 20u, 0u, 2u));
  AppendStoreVgpr(&test.code, 0u, 0u);
  AppendEnd(&test.code);
  if (gradient) test.required_spirv = {"Grad"};
  return test;
}

void CheckModule(const CompiledShader& shader, u32 spatial, u32 components, bool native,
                 bool gradient, bool compare, bool query_lod, bool gather, bool indirect = false) {
  Instructions module(shader.spirv);
  u32 found = 0;
  for (const auto& row : module.instructions) {
    const auto op = static_cast<spv::Op>(row[0] & 0xffffu);
    const bool sample = op == spv::OpImageSampleExplicitLod || op == spv::OpImageSampleImplicitLod ||
                        op == spv::OpImageSampleDrefExplicitLod || op == spv::OpImageSampleDrefImplicitLod;
    if (!(sample || op == spv::OpImageQueryLod || op == spv::OpImageGather)) continue;
    ++found;
    module.Coordinate(row[4], spatial, query_lod ? spatial : components, native, indirect);
    if (compare) Require("UNormSpv", "reference unchanged", module.Op(row[5], spv::OpFDiv) == false,
                        "depth reference divided along with coordinates");
    if (gradient) {
      const u32 mask_index = compare ? 6u : 5u;
      Require("UNormSpv", "original gradient operand mask", row[mask_index] == spv::ImageOperandsGradMask,
              "native explicit gradient changed operand form");
      module.Coordinate(row[mask_index + 1], spatial, spatial, native, indirect);
      module.Coordinate(row[mask_index + 2], spatial, spatial, native, indirect);
    }
  }
  Require("UNormSpv", "live operation", found != 0u, "native live operation was optimized away");
  if (!indirect) Require("UNormSpv", "original sampler userdata live",
          std::ranges::find(shader.program.bindings.user_data_registers, 8u) !=
            shader.program.bindings.user_data_registers.end(), "runtime S#word0 was pruned");
  (void)gather;
}

CompiledShader Fragment(const TestCase& test) {
  GraphicsCase graphics;
  graphics.name = "AuthoredUnnormalizedFragment";
  graphics.fragment_code = test.code;
  const auto mimg = std::find_if(graphics.fragment_code.begin(), graphics.fragment_code.end(),
    [](u32 word) { return (word >> 26u) == 0x3cu; });
  Require(test.name, "authored MIMG", mimg != graphics.fragment_code.end(), "missing operation");
  graphics.fragment_code.resize(static_cast<size_t>(mimg - graphics.fragment_code.begin()) + 2u);
  graphics.fragment_code.push_back(EncodeExp0(0u, 0xfu));
  graphics.fragment_code.push_back(EncodeExp1(0u, 0u, 0u, 0u));
  AppendEnd(&graphics.fragment_code);
  graphics.user_data = test.user_data;
  graphics.has_user_data = true;
  return CompileFragmentCase(graphics);
}

void CheckDecoderAndFill() {
  for (const u32 op : {0x00u, 0x08u, 0x10u}) {
    std::array<u32,2> words{EncodeMimg0(op,1u,0u,false,5u),EncodeMimg1(0u,20u,0u,0u)};
    Decoder::Instruction old{},changed{};
    Decoder::DecodeInstruction(words,0,old);
    words[0] |= 1u<<12u;
    Decoder::DecodeInstruction(words,0,changed);
    Require("UNormInteger", "old nonsampler flags", old.opcode == changed.opcode &&
      old.image_sample_flags == changed.image_sample_flags && old.data_dwords == changed.data_dwords &&
      old.image_dimension == changed.image_dimension &&
      (changed.image_sample_flags & Decoder::ImageSampleFlagUnnormalized) == 0u,
      "native integer UNRM changed old decoder flags");
    if (op != 0x08u) continue;
    IR::Program program;
    program.stage=ShaderType::Compute;
    program.block_storage.push_back(std::make_unique<IR::Block>());
    auto* block=program.block_storage.back().get(); program.blocks.push_back(block);
    program.block_info.push_back({.id=0, .terminator={.kind=ShaderRecompiler::CFG::TerminatorKind::Return}});
    auto input=[&](IR::StageInputKind kind,u32 axis) {
      return IR::Value(&block->AppendNewInst(IR::ValueOpcode::GetBuiltin,
        {IR::Value(static_cast<u32>(kind)),IR::Value(axis)}));
    };
    auto axis=[&](u32 component,u32 stride) {
      auto group=input(IR::StageInputKind::WorkgroupId,component);
      auto local=input(IR::StageInputKind::LocalInvocationId,component);
      auto& mul=block->AppendNewInst(IR::ValueOpcode::IMul32,{group,IR::Value(stride)});
      return IR::Value(&block->AppendNewInst(IR::ValueOpcode::IAdd32,{local,IR::Value(&mul)}));
    };
    std::array<IR::Value,13> coords; coords.fill(IR::Value(0u));
    coords[0]=axis(0u,8u);coords[1]=axis(1u,4u);coords[2]=input(IR::StageInputKind::WorkgroupId,2u);
    auto& address=block->AppendNewInst(IR::ValueOpcode::MakeImageAddress,{coords[0],coords[1],coords[2],coords[3],coords[4],coords[5],coords[6],coords[7],coords[8],coords[9],coords[10],coords[11],coords[12]});
    auto& values=block->AppendNewInst(IR::ValueOpcode::CompositeConstructU32x4,
      {IR::Value(0x3f400000u),IR::Value(0u),IR::Value(0u),IR::Value(0u)});
    auto& image=block->AppendNewInst(IR::ValueOpcode::GetImageResource,
      {IR::Value(0u),IR::Value(0u),IR::Value(0u),IR::Value(0u),IR::Value(0u),IR::Value(0u),IR::Value(0u),IR::Value(0u)});
    auto& write=block->AppendNewInst(IR::ValueOpcode::ImageWrite,
      {IR::Value(&image),IR::Value(&address),IR::Value(&values),IR::Value(true)});
    write.SetFlags<u32>(0u);
    program.info.images.resize(1u);program.info.images[0].written=true;
    program.info.images[0].mip_mode=IR::ImageMipMode::None;
    program.memory_info.push_back({.kind=IR::ResourceKind::Image,.resource=0u,.dmask=1u,
      .data_bits=32u,.image_sample_flags=changed.image_sample_flags,
      .image_dimension=Decoder::ImageDimension::Dim2DArray});
    const auto plan=IR::ExtractResourcePlan(program);
    Require("UNormInteger", "production uniform image fill", plan.uniform_fill.fill.kind==IR::UniformFillKind::Image &&
      plan.uniform_fill.fill.words==1u,"valid UNRM1 integer store lost uniform fill proof");
  }
  std::puts("PASS integer load/store/atomic UNRM1 decoder parity + production uniform image fill");
}

void CheckCubeForceDenied() {
  auto test=ImageCubeGradientsPreserveDerivatives();
  auto user=MakeNativeUserData(&test.user_data); user[8]=1u<<15u;
  // Point the authored native sample to a distinct original S# block.
  for(size_t i=0;i+1<test.code.size();++i) if((test.code[i]>>26u)==0x3cu) {
    test.code[i+1]=(test.code[i+1]&~(0x1fu<<21u))|(2u<<21u);break;
  }
  ShaderComputeInputInfo info{}; info.wave_size=32u;info.host_subgroup_size=32u;
  ShaderRecompiler::CompileOptions options;
  options.stage=ShaderType::Compute;options.input_info.compute=&info;options.user_data=user;
  auto translated=ShaderRecompiler::TranslateProgram(test.code,options);
  auto plan=IR::ExtractResourcePlan(translated.program);
  IR::ResourceSnapshot snapshot;IR::ResourceSpecialization specialization;
  IR::SrtRuntime runtime{.user_data=user};
  Require("UNormCube", "forced cube strict denial",!IR::MaterializeResources(plan,runtime,snapshot,specialization),
          "forced Cube was silently bound to normalized sampler");
  std::puts("PASS real forced Cube materialization denies unsupported coordinate policy");
}

void CheckIndirect(const std::filesystem::path& output, u32 opcode=0x27u) {
  constexpr uint64_t function_address=0x1234500000ull;
  constexpr uint64_t caller_address=0x2345600000ull;
  constexpr uint64_t shader_hash=0x1122334455667788ull;
    TestCase test;
    test.name="ExternalNativeSameCodeTwoAuxSamplers";
    test.has_user_data=true;
    test.has_compute_info=true;
    test.shader_hash=shader_hash;
    test.compute_info.wave_size=32u;
    test.compute_info.host_subgroup_size=32u;
    test.compute_info.threads_num[0]=4u;
    test.compute_info.threads_num[1]=test.compute_info.threads_num[2]=1u;
    test.compute_info.thread_ids_num=1u;
    test.initial.assign(0x600u/4u,0u);
    std::fill_n(test.initial.begin(),16u,0xdeadbeefu);
    test.user_data=MakeNativeUserData(nullptr);
    test.user_data[2]=test.initial.size()*sizeof(u32);
    test.user_data[4]=0x80u;
    test.storage_buffer_offsets={0x80u,0u};
    test.user_data[5]=16u<<16u;
    test.user_data[6]=3u;
    test.user_data[7]=0u;
    test.user_data[8]=0u;
    test.bda_mappings={{0u,0u}};
    for (u32 record=0;record<3u;++record) {
      const u32 table=0x80u/4u+record*4u;
      const u32 aux=0x200u+record*0x100u;
      test.initial[table]=static_cast<u32>(function_address);
      test.initial[table+1u]=static_cast<u32>(function_address>>32u);
      test.initial[table+2u]=aux;
      test.initial[table+3u]=0u;
      const u32 image=aux/4u;
      test.initial[image]=0x20u+record;
      test.initial[image+1u]=(static_cast<u32>(Prospero::BufferFormat::k32_32_32_32Float)<<20u)|(1u<<30u);
      test.initial[image+2u]=1u<<14u;
      test.initial[image+3u]=DstSel(4,5,6,7) |
          (static_cast<u32>(Prospero::ImageType::kColor2D)<<28u);
      // Candidate zero uses the native all-zero default sampler; candidate one linear.
      test.initial[image+10u]=0u;
      test.initial[image+8u]=(record==1u ? 1u<<15u : 0u);
    }
    auto& code=test.code;
    // A true GPU lane input keeps the native record selection off the host evaluator.
    code.push_back(EncodeVop2(0x25,8,8,0));
    code.push_back(EncodeVop1(0x02,8,Vgpr(8)));
    const u32 record_load_pc=code.size()*sizeof(u32);
    code.push_back(EncodeSmem0(0x0a,14,2)); // s_buffer_load_dwordx4 s14, s4, s8.
    code.push_back(EncodeSmem1(0,8));
    const u32 call_pc=code.size()*sizeof(u32);
    code.push_back(EncodeSop1(0x21,20,14)); // distinct return s20:s21; aux stays s16:s17.
    code.push_back(EncodeVop2(0x1a,28,InlineU32(2),0));
    for (u32 component=0;component<4u;++component)
      AppendStoreVgprAtLaneDwordOffset(&code,20u+component,28,component);
    AppendEnd(&code);

    std::vector<u32> body;
    body.push_back(EncodeSmem0(0x03,24,8)); // s_load_dwordx8 s24, s16, 0.
    body.push_back(EncodeSmem1(0));
    body.push_back(EncodeSmem0(0x02,32,8)); // sampler sharp at aux+32.
    body.push_back(EncodeSmem1(32u));
    AppendVMovLiteral(&body,8,std::bit_cast<u32>(0.5f));
    AppendVMovLiteral(&body,9,std::bit_cast<u32>(0.5f));
    body.push_back(EncodeMimg0(opcode,opcode==0x47u?1u:0xfu)); // SampleLz, or original indirect Gather/QueryLod denial control.
    body.push_back(EncodeMimg1(20,8,6,8));
    body.push_back(EncodeSop1(0x20,0,20));
    ShaderRecompiler::ExternalLibraryPlan library;
    library.complete=true;
    library.caller_address=caller_address;
    library.functions.push_back({0u,function_address,std::move(body)});
    ShaderRecompiler::ExternalCallSite site;
    site.caller_pc=call_pc;
    site.target_sgpr=14u;
    site.return_sgpr=20u;
    site.context_domain=11u;
    site.record_load_pc=record_load_pc;
    site.record_sgpr=14u;
    site.auxiliary_sgpr=16u;
    site.table_base=0x80u;
    site.table_bytes=32u;
    site.candidate_addresses={function_address};
    for (u32 record=0;record<2u;++record) {
      const uint64_t aux=0x200u+record*0x100u;
      site.records.push_back({record,0u,function_address,aux});
      site.context_records.push_back({record,0u,
          {static_cast<u32>(function_address),static_cast<u32>(function_address>>32u),
           static_cast<u32>(aux),0u}});
    }
    library.call_sites.push_back(std::move(site));
    test.external_library=&library;

    if(opcode!=0x27u) {
      std::printf("CHECK original indirect operation rejection opcode=0x%x\n",opcode);
      std::fflush(stdout);
      (void)CompileCase(test,32u);
      Require(test.name,"original indirect admission guard",false,
        "unsupported original indirect operation unexpectedly admitted");
    }
    auto compiled=CompileCase(test,32u);
    CheckModule(compiled,2u,2u,false,false,false,false,false,true);
    u32 forced=0u,normal=0u;
    for(const auto& sampler:compiled.program.info.samplers) {
      if(sampler.indirect_root!=IR::SamplerResource::NoIndirectSampler) {
        Require(test.name,"original snapshot candidate force",sampler.snapshot_index<compiled.resources.samplers.size() &&
          sampler.force_unnormalized_coordinates==((compiled.resources.samplers[sampler.snapshot_index].dwords[0]&(1u<<15u))!=0u),
          "candidate fact differs from selected original descriptor");
        (sampler.force_unnormalized_coordinates?forced:normal)++;
      }
    }
    Require(test.name,"selected raw descriptor force facts",forced!=0u && normal!=0u,
      "indirect sampler candidates lost distinct original ForceUnorm facts");
    const auto domains=ShaderRecompiler::ExternalContextDomains(library);
    ShaderRecompiler::CompileOptions options;
    options.stage=ShaderType::Compute;options.wave_size=32u;options.input_info.compute=&test.compute_info;
    options.user_data=test.user_data;options.external_library=&library;options.shader_hash=shader_hash;
    auto translated=ShaderRecompiler::TranslateProgram(test.code,options);
    const auto plan=IR::ExtractResourcePlan(translated.program);
    IR::SrtRuntime runtime{.user_data=test.user_data,.read_memory=ReadTestMemory,
      .userdata=&test.initial,.read_specialization_memory=ReadTestMemory,.external_context_domains=domains};
    IR::ResourceSnapshot snapshot;IR::ResourceSpecialization warm;
    Require(test.name,"initial indirect materialize",IR::MaterializeResources(plan,runtime,snapshot,warm),"initial failed");
    const auto initial=warm;
    test.initial[0x300u/4u+8u]=0u;
    Require(test.name,"warm ForceUnorm flip",IR::MaterializeResources(plan,runtime,snapshot,warm),"warm failed");
    IR::ResourceSnapshot cold_snapshot;IR::ResourceSpecialization cold;
    Require(test.name,"cold ForceUnorm flip",IR::MaterializeResources(plan,runtime,cold_snapshot,cold),"cold failed");
    Require(test.name,"permutation and warm parity",initial!=warm && warm==cold,
      "indirect force change did not key permutation or warm/cold state drifted");
    Instructions words(compiled.spirv);
    u32 selected_samples=0u;
    for(const auto& row:words.instructions) {
      if((row[0]&0xffffu)!=static_cast<u32>(spv::OpImageSampleExplicitLod))continue;
      const auto& coordinate=words.Def(row[4]);
      const auto& axis=words.Def(coordinate[3]);
      const auto& force=words.Def(axis[3]);
      const auto& force_match=words.Def(force[3]);
      const auto& sampled=words.Def(row[3]);
      const auto& sampler_load=words.Def(sampled[4]);
      const auto& sampler_pointer=words.Def(sampler_load[3]);
      const auto& sampler_slot=words.Def(sampler_pointer.back());
      const auto& sampler_match=words.Def(sampler_slot[3]);
      Require(test.name,"one selected sampler ordinal",words.Op(force[3],spv::OpIEqual) &&
        words.Op(sampler_slot[3],spv::OpIEqual) && force_match[3]==sampler_match[3],
        "coordinate force and exact native sampler binding select different context ordinals");
      ++selected_samples;
    }
    Require(test.name,"sampled sampler binding association",selected_samples!=0u,
      "no exact native sampler binding was checked");
    std::ofstream file(output/"indirect-sample.spv",std::ios::binary);
    file.write(reinterpret_cast<const char*>(compiled.spirv.data()),compiled.spirv.size()*sizeof(u32));
    std::puts("PASS native indirect sample: key bit15=0 versus selected descriptor bit15=1; same sampler ordinal; warm/cold permutation parity");
    // Separate child-process compatibility controls prove original Gather/QueryLod denial.
    // No admission is broadened here.

}

void Run(const std::filesystem::path& output) {
  Require("UNormCpu", "fresh output", !std::filesystem::exists(output), "output must be fresh");
  std::filesystem::create_directories(output);
  EnsureConfigInitialized(false);
  size_t cases = 0;
#ifndef UNORM_BASELINE
  for (u32 bits = 0; bits < 4u; ++bits) {
    const bool force = (bits & 1u) != 0, native = (bits & 2u) != 0;
    for (u32 form = 0u; form < 6u; ++form) {
      const bool array = form == 1u || form == 3u;
      const bool gradient = form == 2u || form == 3u;
      const bool query = form == 4u, gather = form == 5u;
      const u32 opcode = gradient ? 0x22u : query ? 0x60u : gather ? 0x44u : 0x27u;
      auto test = MakeCase(opcode, array ? 5u : 1u,
        array ? Prospero::ImageType::kColor2DArray : Prospero::ImageType::kColor2D,
        force, native, gradient, false, gather);
      auto shader = query ? Fragment(test) : CompileCase(test, 32u);
      CheckModule(shader, 2u, array ? 3u : 2u, native, gradient, false, query, gather);
      std::ofstream spv_file(output / ("case-" + std::to_string(cases) + ".spv"), std::ios::binary);
      spv_file.write(reinterpret_cast<const char*>(shader.spirv.data()), shader.spirv.size() * sizeof(u32));
      std::printf("PASS native case=%zu flags=%u form=%u module_words=%zu\n", cases++, bits, form, shader.spirv.size());
    }
    for (bool compare : {false, true}) {
      auto test = MakeCase(compare ? 0x28u : 0x20u, 1u, Prospero::ImageType::kColor2D,
                           force, native, false, compare);
      GraphicsCase graphics;
      graphics.name = "AuthoredImplicitUnnormalizedCoordinate";
      graphics.fragment_code = test.code;
      // Strip the authored compute output store; export the sample directly.
      const auto mimg = std::find_if(graphics.fragment_code.begin(), graphics.fragment_code.end(),
        [](u32 word) { return (word >> 26u) == 0x3cu; });
      Require(test.name, "authored MIMG", mimg != graphics.fragment_code.end(), "missing sample");
      graphics.fragment_code.resize(static_cast<size_t>(mimg - graphics.fragment_code.begin()) + 2u);
      graphics.fragment_code.push_back(EncodeExp0(0u, 0xfu));
      graphics.fragment_code.push_back(EncodeExp1(0u, 0u, 0u, 0u));
      AppendEnd(&graphics.fragment_code);
      graphics.user_data = test.user_data;
      graphics.has_user_data = true;
      auto shader = CompileFragmentCase(graphics);
      CheckModule(shader, 2u, 2u, native, false, compare, false, false);
      std::ofstream spv_file(output / ("case-" + std::to_string(cases) + ".spv"), std::ios::binary);
      spv_file.write(reinterpret_cast<const char*>(shader.spirv.data()), shader.spirv.size() * sizeof(u32));
      std::printf("PASS implicit case=%zu flags=%u compare=%d module_words=%zu\n", cases++, bits, compare, shader.spirv.size());
    }
  }
  for(u32 bits=0u;bits<4u;++bits) {
    auto test=ImageGather2DInstructionWith1DDescriptor();test.user_data[8]=(bits&1u)?1u<<15u:0u;
    for(size_t word=0u;word+1u<test.code.size();++word) if((test.code[word]>>26u)==0x3cu) {
      test.code[word]|=(bits&2u)?1u<<12u:0u;
      test.code[word+1]=(test.code[word+1]&~(0x1fu<<21u))|(2u<<21u);break;
    }
    const auto shader=CompileCase(test,32u);Instructions module(shader.spirv);
    u32 divides=0u,queries=0u;
    for(const auto& row:module.instructions) {
      divides+=((row[0]&0xffffu)==static_cast<u32>(spv::OpFDiv));
      queries+=((row[0]&0xffffu)==static_cast<u32>(spv::OpImageQuerySizeLod));
    }
    Require(test.name,"1D gather converted before texel coordinate",divides>=3u && queries>=2u,
      "1D fallback omitted sample coordinate normalization");
    std::ofstream file(output/("gather-1d-"+std::to_string(bits)+".spv"),std::ios::binary);
    file.write(reinterpret_cast<const char*>(shader.spirv.data()),shader.spirv.size()*sizeof(u32));
    std::printf("PASS native 1D gather flags=%u converted before texel weights\n",bits);
  }
  CheckDecoderAndFill();
  CheckCubeForceDenied();
  CheckIndirect(output);
#endif
  auto cube = ImageCubeGradientsPreserveDerivatives();
  const auto normal_cube = CompileCase(cube, 32u);
  std::ofstream cube_file(output / "normalized-cube.spv", std::ios::binary);
  cube_file.write(reinterpret_cast<const char*>(normal_cube.spirv.data()), normal_cube.spirv.size() * sizeof(u32));
  std::printf("PASS normal cube module_words=%zu\n", normal_cube.spirv.size());
  std::printf("PASS authored CPU modules=%zu plus_normal_cube=1; default Vulkan1.2; no GPU\n", cases);
}
} // namespace UNormFixture
} // namespace
} // namespace Libs::Graphics
