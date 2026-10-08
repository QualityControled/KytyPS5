#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ResourceMaterializationTests: failed: %s\n", text);
    std::abort();
  }
}

bool RejectSpecializationRead(void *userdata, uint64_t, std::span<uint32_t>) {
  ++*static_cast<uint32_t *>(userdata);
  return false;
}

Libs::Graphics::ShaderRecompiler::IR::Block &
AddValueBlock(Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto block = std::make_unique<Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  program.block_info.push_back({.id = 0});
  program.block_storage.push_back(std::move(block));
  return *result;
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan SrtPlan(uint64_t address) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  program.memory_info.push_back(memory);
  const auto low = Value(static_cast<uint32_t>(address));
  const auto high = Value(static_cast<uint32_t>(address >> 32u));
  auto &handle =
      value_block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high});
  auto &raw = value_block.AppendNewInst(
      ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});

  auto &srt = value_block.AppendNewInst(ValueOpcode::GetSrtResource);
  auto &flat = value_block.AppendNewInst(ValueOpcode::ReadConst,
                                         {Value(&srt), Value(0u)});
  DescriptorSource source;
  source.dwords[0] = Value(&flat);
  source.dwords[1] = Value(0u);
  source.dword_count = 2;
  program.descriptor_sources.push_back(source);
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UnbasedFlatPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);
  program.info.uses_dma = true;
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UserDataBufferPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  auto &user_data = value_block.AppendNewInst(
      ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  DescriptorSource source;
  source.dwords[0] = Value(&user_data);
  source.dwords[1] = Value(0u);
  source.dwords[2] = Value(0u);
  source.dwords[3] = Value(0u);
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::Program MixedSamplerProgram() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);

  const auto AddSource = [&program](uint32_t dword_count) {
    DescriptorSource source;
    source.dword_count = dword_count;
    for (uint32_t i = 0; i < dword_count; i++) {
      source.dwords[i] = Value(0u);
    }
    program.descriptor_sources.push_back(source);
    return static_cast<uint32_t>(program.descriptor_sources.size() - 1u);
  };

  const auto image0 = AddSource(8);
  const auto image1 = AddSource(8);
  const auto sampler0 = AddSource(4);
  const auto sampler1 = AddSource(4);
  program.descriptor_sources[image1].dwords[0] = Value(1u);
  program.descriptor_sources[image1].dwords[1] = Value(static_cast<uint32_t>(
      Libs::Graphics::Prospero::BufferFormat::k11_11_10UInt) << 20u);
  program.descriptor_sources[image1].dwords[3] = Value(static_cast<uint32_t>(
      Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  for (uint32_t index = 0; index < 2; ++index) {
    auto &value = block.AppendNewInst(
        ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(index))});
    program.descriptor_sources[sampler0 + index].dwords[0] = Value(&value);
  }
  program.info.images.push_back(
      {.source = image0,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.images.push_back(
      {.source = image1,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.samplers.push_back({.source = sampler0});
  program.info.samplers.push_back({.source = sampler1});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 0});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 1});
  program.info.sampled_pairs.push_back({.image = 1, .sampler = 1});
  return program;
}

void TestMappedSrtUsesDirectReaderByDefault() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const uint32_t dword = 0x12345678;
  auto plan = SrtPlan(reinterpret_cast<uint64_t>(&dword));
  uint32_t specialization_reads = 0;
  const SrtRuntime runtime{.userdata = &specialization_reads,
                           .read_specialization_memory =
                               RejectSpecializationRead};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mapped SRT stage materialization failed");
  Check(specialization_reads == 0,
        "ordinary SRT read used the specialization reader");
  Check(snapshot.flattened_srt.size() == 1 &&
            snapshot.flattened_srt[0] == dword,
        "cache rematerialization did not use the direct reader by default");
}

void TestIntegerRuntimeValueFollowsSrtReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = SrtPlan(0x10000);
  const auto root = plan.descriptor_sources.front().dwords[0];
  Check(ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT read was rejected");

  Block values;
  auto &comparison = values.AppendNewInst(ValueOpcode::FPOrdLessThanEqual32,
                                          {Value::F32(1.f), Value::F32(0.f)});
  auto &selection = values.AppendNewInst(
      ValueOpcode::SelectU32, {Value(&comparison), Value(1u), Value(0u)});
  plan.srt_reads[0].value = Value(&selection);
  Check(ValidateRuntimeValue(plan, root),
        "ordinary SRT validation rejected a floating-point dependency");
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT validation missed a hidden floating-point dependency");

  auto &first =
      values.AppendNewInst(ValueOpcode::ReadFirstLane, {root, Value(true)});
  Check(!ValidateRuntimeValue(plan, Value(&first), RuntimeValueType::Integer),
        "read-first-lane lost integer-only SRT validation");

  auto &active = values.AppendNewInst(ValueOpcode::ReadFirstLane,
                                      {Value(&selection), Value(&comparison)});
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point execution mask was accepted as integer-only");

  auto &lane = values.AppendNewInst(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)),
       Value(0u)});
  auto &mask =
      values.AppendNewInst(ValueOpcode::INotEqual32, {Value(&lane), Value(0u)});
  selection.SetArg(0, Value(&mask));
  active.SetArg(1, Value(&mask));
  Check(ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "nonuniform integer execution mask was rejected");
  auto &float_value =
      values.AppendNewInst(ValueOpcode::BitCastU32F32, {Value::F32(1.f)});
  selection.SetArg(2, Value(&float_value));
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point inactive arm was accepted as integer-only");

  plan.srt_reads[0].value = Value(&first);
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "cyclic SRT read-first-lane dependency was accepted");
}

void TestUniformVectorDescriptorRead() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  auto &handle = block.AppendNewInst(ValueOpcode::GetBufferResource,
      {Value(0x1000u), Value(4u << 16u), Value(1u), Value(0x16204u)});
  program.memory_info.push_back({.kind = ResourceKind::Buffer});
  auto &count = block.AppendNewInst(ValueOpcode::LoadBufferU32,
      {Value(&handle), Value(0u), Value(0u), Value(0u), Value(true)});
  count.SetFlags(MemoryFlags{.index = 0});
  // The captured indirect kernel shares one read across sibling scalar lane reads,
  // enclosed by a different EXEC mask. Its resource plan must share that read too.
  auto &lane = block.AppendNewInst(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
  auto &active = block.AppendNewInst(ValueOpcode::ULessThan32, {Value(&lane), Value(32u)});
  auto &first = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&count), Value(true)});
  auto &next = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&count), Value(1u)});
  auto &second = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&next), Value(true)});
  auto &sum = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&first), Value(&second)});
  auto &small = block.AppendNewInst(ValueOpcode::ULessThanEqual32, {Value(&count), Value(72u)});
  auto &selected = block.AppendNewInst(ValueOpcode::SelectU32, {Value(&small), Value(&sum), Value(&count)});
  auto &masked = block.AppendNewInst(ValueOpcode::SelectU32, {Value(&active), Value(&selected), Value(0u)});
  auto &records = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&masked), Value(&active)});
  DescriptorSource source;
  source.dword_count = 4;
  source.dwords = {Value(0x2000u), Value(4u << 16u), Value(&records), Value(0x16204u)};
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .written = true});
  Check(ValidateRuntimeValue(program, Value(&count)), "uniform DWORD count was rejected");
  struct Reads { uint32_t value = 72; uint32_t strict = 0; uint32_t ordinary = 0; bool clean = true; } reads;
  const SrtRuntime runtime{
      .read_memory = [](void *data, uint64_t, std::span<uint32_t> words) {
        ++static_cast<Reads *>(data)->ordinary;
        words[0] = 999;
        return true;
      },
      .userdata = &reads,
      .read_specialization_memory = [](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.strict;
        if (!reads.clean || address != 0x1000u || words.size() != 1) return false;
        words[0] = reads.value;
        return true;
      }};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const bool written : {false, true}) {
    program.info.buffers[0].written = written;
    auto plan = ExtractResourcePlan(program);
    Check(plan.control_flow.empty() && plan.requires_specialization_memory &&
              plan.capture_specialization_reads,
          "vector descriptor read depended on incidental control-flow capture");
    reads = {};
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.buffers[0].dwords[2] == 145 && reads.strict == 1 && reads.ordinary == 0 &&
              snapshot.specialization_reads ==
                  std::vector<std::pair<uint64_t, uint64_t>>{{0x1000u, 4u}},
          "vector descriptor input was not read and captured exactly once");
    reads.clean = false;
    reads.strict = 0;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
              reads.strict == 1 && reads.ordinary == 0,
          "dirty vector descriptor input fell back to an ordinary memory read");
  }
  count.SetArg(4, Value(false));
  auto &inactive = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&count), Value(false)});
  reads.strict = 0;
  uint32_t result = 99;
  Check(SrtWalker(program, runtime).Evaluate(Value(&inactive), result) &&
            result == 0 && reads.strict == 0 && reads.ordinary == 0,
        "literal false EXEC read vector memory");
  count.SetArg(4, Value(true));
  handle.SetArg(3, Value(0x204u));
  Check(SrtWalker(program, runtime).Evaluate(Value(&count), result) &&
            result == 0 && reads.strict == 0 && reads.ordinary == 0,
        "invalid vector buffer format read memory");
  handle.SetArg(3, Value(0x16204u));
  count.SetArg(1, Value(&lane));
  Check(!ValidateRuntimeValue(program, Value(&count)),
        "varying vector address was treated as a uniform descriptor read");
}

void TestExactReciprocalDescriptorArithmetic() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  auto &block = AddValueBlock(program);
  for (const float divisor : {64.f, 128.f, 256.f, 512.f,
                              std::numeric_limits<float>::min(),
                              std::bit_cast<float>(253u << 23u)}) {
    auto &reciprocal = block.AppendNewInst(ValueOpcode::FPRecipIFlag32, {Value::F32(divisor)});
    uint32_t result = 0;
    Check(SrtWalker(program, {}).Evaluate(Value(&reciprocal), result) &&
              result == std::bit_cast<uint32_t>(1.f / divisor),
          "power-of-two reciprocal was not exact");
  }
  for (const float divisor : {0.f, 3.f, -64.f, std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::denorm_min(),
                              std::bit_cast<float>(254u << 23u)}) {
    auto &reciprocal = block.AppendNewInst(ValueOpcode::FPRecipIFlag32, {Value::F32(divisor)});
    uint32_t result = 0;
    Check(!SrtWalker(program, {}).Evaluate(Value(&reciprocal), result),
          "unsupported reciprocal rounding or exceptional input was accepted");
  }
}

void TestUnbasedFlatCacheHitMaterializes() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UnbasedFlatPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "unbased FLAT stage materialization failed");
  Check(snapshot.buffers.empty() && snapshot.images.empty(),
        "unbased FLAT plan produced unexpected descriptors");
}

void TestWrittenDescriptorUsesStrictReaderOnce() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.user_data_count = 1;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                     {Value(0x1000u), Value(0u)});
  auto &offset = block.AppendNewInst(ValueOpcode::GetUserData,
                                     {Value(static_cast<ScalarReg>(0))});
  auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(&offset), Value(0u), Value(false)});
  read.SetFlags(MemoryFlags{.index = 0});
  DescriptorSource source;
  source.dwords = {Value(&read), Value(0u), Value(4u), Value(0u)};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .written = true});
  // A host-evaluable branch captures resource reads and needs the writable
  // descriptor's clean provenance for the renderer's disjointness proof.
  auto &condition = block.AppendNewInst(ValueOpcode::IEqual32,
                                        {Value(&offset), Value(4u)});
  auto &store_block = AddValueBlock(program);
  AddValueBlock(program);
  program.block_info[0].condition = Value(&condition);
  program.block_info[0].terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
  program.block_info[0].terminator.true_block = 1;
  program.block_info[0].terminator.false_block = 2;
  program.block_info[1].id = 1;
  program.block_info[1].terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
  program.block_info[2].id = 2;
  program.block_info[2].terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
  program.memory_info.push_back({.kind = ResourceKind::Buffer, .resource = 0});
  auto &output = store_block.AppendNewInst(ValueOpcode::GetBufferResource,
      {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]});
  store_block.AppendNewInst(ValueOpcode::StoreBufferU32,
      {Value(&output), Value(0u), Value(0u), Value(0u), Value(1u), Value(true)})
      .SetFlags(MemoryFlags{.index = 1});
  auto plan = ExtractResourcePlan(program);
  Check(plan.capture_specialization_reads,
        "conditional writable descriptor lost its alias proof");
  struct Reads { uint32_t ordinary = 0; uint32_t strict = 0; bool clean = false; } reads;
  const std::array<uint32_t, 1> user_data{4u};
  const SrtRuntime runtime{
      .user_data = user_data,
      .read_memory = +[](void *data, uint64_t, std::span<uint32_t> words) {
        ++static_cast<Reads *>(data)->ordinary;
        words[0] = 0x8000u;
        return true;
      },
      .userdata = &reads,
      .read_specialization_memory = +[](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.strict;
        if (!reads.clean || address != 0x1004u) return false;
        words[0] = 0x8000u;
        return true;
      }};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
            reads.ordinary == 0 && reads.strict == 1,
        "GPU-dirty dynamic writable descriptor bypassed strict provenance");
  reads.clean = true;
  reads.strict = 0;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            reads.ordinary == 0 && reads.strict == 1 && snapshot.buffers[0].dwords[0] == 0x8000u &&
            snapshot.specialization_reads ==
                std::vector<std::pair<uint64_t, uint64_t>>{{0x1004u, 4u}},
        "writable descriptor was evaluated twice or scalar EXEC suppressed its read");
}

void TestFailedMaterializationRejectsStage() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UserDataBufferPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(!MaterializeResources(plan, {}, snapshot, specialization),
        "missing runtime user data did not reject the cached stage");
}

void TestFiniteImageRefreshReusesScalarReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                     {Value(0x1000u), Value(0u)});
  auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
  for (uint32_t index = 0; index < 3; ++index) {
    auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
        {Value(&handle), Value(index * 4u), Value(0u), Value(true)});
    read.SetFlags(MemoryFlags{.index = 0});
    program.srt_reads.push_back({Value(&read), index});
    auto &flat = block.AppendNewInst(ValueOpcode::ReadConst,
                                     {Value(&srt), Value(index)});
    DescriptorSource source;
    source.dword_count = 8;
    source.dwords.fill(Value(0u));
    source.dwords[0] = Value(&flat);
    source.dwords[1] = Value(static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u);
    source.dwords[3] = Value(Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u));
    program.descriptor_sources.push_back(source);
  }
  DescriptorSource root;
  root.dword_count = 8;
  root.dwords.fill(Value(0u));
  root.indirect_descriptor.emplace(DescriptorSource::IndirectDescriptor{}).sources = {0, 1, 2, 1};
  program.descriptor_sources.push_back(root);
  program.info.images.push_back({
      .source = 3,
      .resource_class = ImageResourceClass::Sampled,
      .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
      .dimension = Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  auto plan = ExtractResourcePlan(program);
  struct Reads {
    std::array<uint32_t, 3> words{0x100u, 0x200u, 0x200u};
    std::array<uint32_t, 3> counts{};
    uint32_t ordinary = 0;
  } reads;
  const SrtRuntime runtime{
      .read_memory = +[](void *data, uint64_t, std::span<uint32_t>) {
        ++static_cast<Reads *>(data)->ordinary;
        return false;
      },
      .userdata = &reads,
      .read_specialization_memory = +[](void *data, uint64_t address,
                                        std::span<uint32_t> words) {
        if (words.size() != 1 || address < 0x1000u || address >= 0x100cu ||
            (address & 3u) != 0) return false;
        auto &reads = *static_cast<Reads *>(data);
        const auto index = (address - 0x1000u) / 4u;
        ++reads.counts[index];
        words[0] = reads.words[index];
        return true;
      }};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto capacities = [&] {
    return std::array{snapshot.images.capacity(), snapshot.flattened_srt.capacity(),
                      snapshot.specialization_reads.capacity(), specialization.images.capacity()};
  };
  const auto check_mapping = [&](std::array<uint32_t, 4> ordinals) {
    const auto offset = specialization.images[0].indirect_mapping_offset;
    Check(snapshot.images.size() == 2 && snapshot.flattened_srt[offset] == 4,
          "finite image candidates were not deduplicated");
    for (uint32_t key = 0; key < ordinals.size(); ++key) {
      Check(snapshot.flattened_srt[offset + 1u + key * 2u] == key &&
                snapshot.flattened_srt[offset + 2u + key * 2u] == ordinals[key],
            "finite image selector mapping is stale or incorrect");
    }
  };
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "finite image materialization failed");
  Check(reads.ordinary == 0 && reads.counts == std::array<uint32_t, 3>{1, 1, 1} &&
            snapshot.specialization_reads.size() == 3,
        "finite image candidates repeated scalar reads or bypassed clean provenance");
  check_mapping({0, 1, 1, 1});
  const auto warm_capacities = capacities();
  reads.words = {0x300u, 0x300u, 0x400u};
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "finite image refresh failed after descriptor changes");
  Check(reads.ordinary == 0 && reads.counts == std::array<uint32_t, 3>{2, 2, 2} &&
            snapshot.specialization_reads.size() == 3 &&
            snapshot.images[0].dwords[0] == 0x300u && snapshot.images[1].dwords[0] == 0x400u,
        "finite image refresh retained old scalar values or repeated reads");
  check_mapping({0, 0, 1, 0});
  Check(capacities() == warm_capacities,
        "finite image refresh grew reusable resource storage after warmup");
}

void TestSupportedIndirectImageOperationsSpecialize() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension;
  constexpr std::array operations{ValueOpcode::ImageWrite,
                                  ValueOpcode::ImageRead,
                                  ValueOpcode::ImageQueryDimensions};
  constexpr std::array names{"ImageWrite", "ImageRead", "ImageQueryDimensions"};
  for (uint32_t index = 0; index < operations.size(); ++index) {
    Program program;
    program.stage = Libs::Graphics::ShaderType::Compute;
    program.srt_plan_complete = true;
    program.resource_tracking_complete = true;
    auto &block = AddValueBlock(program);
    auto &selector = block.AppendNewInst(ValueOpcode::LaneId);
    auto &image =
        block.AppendNewInst(ValueOpcode::GetImageResource,
                            {Value(&selector), Value(0u), Value(0u), Value(0u),
                             Value(0u), Value(0u), Value(0u), Value(0u)});
    image.SetFlags<uint32_t>(0u);
    auto &address = block.AppendNewInst(
        ValueOpcode::MakeImageAddress,
        {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
         Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
         Value(0u)});
    program.memory_info.push_back({.kind = ResourceKind::Image,
                                   .resource = 0u,
                                   .image_dimension = ImageDimension::Dim2D});
    Inst *operation = nullptr;
    if (operations[index] == ValueOpcode::ImageWrite) {
      auto &texel =
          block.AppendNewInst(ValueOpcode::CompositeConstructU32x4,
                              {Value(1u), Value(2u), Value(3u), Value(4u)});
      operation = &block.AppendNewInst(
          operations[index],
          {Value(&image), Value(&address), Value(&texel), Value(true)});
    } else if (operations[index] == ValueOpcode::ImageRead) {
      operation = &block.AppendNewInst(
          operations[index], {Value(&image), Value(&address), Value(true)});
    } else {
      operation = &block.AppendNewInst(operations[index],
                                       {Value(&image), Value(&address)});
    }
    operation->SetFlags(MemoryFlags{.index = 0u, .pc = 0x20u});
    ImageResource root;
    root.resource_class = operations[index] == ValueOpcode::ImageWrite
                              ? ImageResourceClass::Storage
                              : ImageResourceClass::Sampled;
    root.numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float;
    root.dimension = ImageDimension::Dim2D;
    root.read = operations[index] != ValueOpcode::ImageWrite;
    root.written = operations[index] == ValueOpcode::ImageWrite;
    program.info.images = {root, root};
    ResourceSpecialization specialization;
    ResourceSpecialization::Image candidate;
    candidate.numeric_class = root.numeric_class;
    candidate.dimension = root.dimension;
    candidate.indirect_root = 0u;
    candidate.indirect_mapping_offset = 3u;
    specialization.images.assign(4u, candidate);
    specialization.images[1].indirect_root = ImageResource::NoIndirectImage;
    specialization.images[2].dimension = ImageDimension::Dim2DArray;
    specialization.images[3].shader_swizzle =
        Libs::Graphics::DstSel(6, 5, 4, 7);
    std::fprintf(stderr,
                 "ResourceMaterializationTests: specializing indirect %s\n",
                 names[index]);
    ApplyResourceSpecialization(program, specialization);
    Check(operation->GetOpcode() == operations[index] &&
              operation->Flags<MemoryFlags>().index == 0u &&
              program.memory_info[0].resource == 0u &&
              image.Flags<uint32_t>() == 0u,
          "supported indirect image instruction lost its root or memory "
          "identity");
    Check(program.info.images.size() == 4u &&
              program.info.images[0].indirect_resources ==
                  std::vector<uint32_t>{0u, 2u, 3u} &&
              program.info.images[1].indirect_root ==
                  ImageResource::NoIndirectImage &&
              program.info.images[2].dimension == ImageDimension::Dim2DArray &&
              program.info.images[3].shader_swizzle ==
                  Libs::Graphics::DstSel(6, 5, 4, 7) &&
              program.info.images[2].resource_class == root.resource_class &&
              program.info.images[2].written == root.written,
          "indirect image specialization lost candidate metadata or included "
          "an unrelated root");
  }
}

void TestMixedSamplerVariantsShareRuntimeDescriptor() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto program = MixedSamplerProgram();
  auto plan = ExtractResourcePlan(program);
  std::array<uint32_t, 2> user_data{0x11111111u, 0x22222222u};
  const SrtRuntime runtime{.user_data = user_data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mixed sampler materialization failed");
  ApplyResourceSpecialization(program, specialization);
  const auto &samplers = program.info.samplers;
  Check(snapshot.samplers.size() == 2 && samplers.size() == 3 &&
            samplers[0].snapshot_index == 0 && samplers[1].snapshot_index == 1 &&
            samplers[2].snapshot_index == 1 &&
            samplers[0].source == plan.info.samplers[0].source &&
            samplers[1].source == plan.info.samplers[1].source &&
            samplers[2].source == samplers[1].source &&
            !samplers[1].force_point_filtering && samplers[2].force_point_filtering &&
            program.info.sampled_pairs[2].sampler == 2,
        "native sampler variants lost their source identity or binding order");
  const auto capacity = snapshot.samplers.capacity();
  user_data[1] = 0x33333333u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.samplers.size() == 2 && snapshot.samplers.capacity() == capacity &&
            snapshot.samplers[samplers[0].snapshot_index].dwords[0] == user_data[0] &&
            snapshot.samplers[samplers[1].snapshot_index].dwords[0] == user_data[1] &&
            snapshot.samplers[samplers[2].snapshot_index].dwords[0] == user_data[1],
        "sampler variants retained stale or duplicated descriptors after refresh");
}

struct ExternalMaterialFixture {
  using Program = Libs::Graphics::ShaderRecompiler::IR::Program;
  using Value = Libs::Graphics::ShaderRecompiler::IR::Value;
  using Inst = Libs::Graphics::ShaderRecompiler::IR::Inst;
  using Record =
      Libs::Graphics::ShaderRecompiler::IR::ExternalCallContextRecord;
  using Domain =
      Libs::Graphics::ShaderRecompiler::IR::ExternalCallContextDomain;
  Program program;
  std::unordered_map<uint64_t, uint32_t> words;
  std::vector<uint64_t> requested;
  std::array<Record, 3> records{{{.ordinal = 3,
                                  .function_id = 7,
                                  .words = {0x12340000u, 0u, 0x2000u, 0u}},
                                 {.ordinal = 9,
                                  .function_id = 7,
                                  .words = {0x12340000u, 0u, 0x3000u, 0u}},
                                 {.ordinal = 21,
                                  .function_id = 7,
                                  .words = {0x12340000u, 0u, 0x2000u, 0u}}}};
  std::array<Domain, 1> domains;
  Inst *low = nullptr;
  Inst *high = nullptr;
  Value lane;
  uint64_t reject_address = UINT64_MAX;

  ExternalMaterialFixture() {
    using namespace Libs::Graphics::ShaderRecompiler::IR;
    program.stage = Libs::Graphics::ShaderType::Compute;
    program.shader_hash = 0x13579bdfu;
    program.srt_plan_complete = true;
    program.resource_tracking_complete = true;
    program.external_context_bindings.push_back({11u, 7u});
    auto &block = AddValueBlock(program);
    lane = Value(&block.AppendNewInst(ValueOpcode::LaneId));
    low = &block.AppendNewInst(ValueOpcode::ExternalCallContextWord,
                               {lane, Value(11u), lane, Value(2u), Value(7u)});
    high = &block.AppendNewInst(ValueOpcode::ExternalCallContextWord,
                                {lane, Value(11u), lane, Value(3u), Value(7u)});
    program.memory_info.push_back(
        {.kind = ResourceKind::ScalarAddress, .planning_only = true});
    const auto Read = [&](Value lo, Value hi, uint32_t offset) {
      auto &address =
          block.AppendNewInst(ValueOpcode::GetAddressResource, {lo, hi});
      auto &read = block.AppendNewInst(
          ValueOpcode::LoadAddressU32,
          {Value(&address), Value(offset), Value(0u), Value(true)});
      read.SetFlags(MemoryFlags{.index = 0u, .pc = 0x40u + offset});
      return Value(&read);
    };
    const auto nested_low = Read(Value(low), Value(high), 0u);
    const auto nested_high = Read(Value(low), Value(high), 4u);
    DescriptorSource expression;
    expression.dword_count = 4u;
    for (uint32_t word = 0; word < 4u; ++word)
      expression.dwords[word] = Read(nested_low, nested_high, word * 4u);
    program.descriptor_sources.push_back(expression);
    DescriptorSource source;
    source.dword_count = 4u;
    source.dwords.fill(Value(0u));
    source.indirect_descriptor.emplace();
    source.indirect_descriptor->external_context.emplace(
        DescriptorSource::IndirectDescriptor::ExternalContext{11u, 7u, 0u});
    program.descriptor_sources.push_back(source);
    program.info.samplers.push_back({.source = 1u});
    domains[0] = {11u, true, records};
    words[0x2000u] = 0x4000u;
    words[0x2004u] = 0u;
    words[0x3000u] = 0x5000u;
    words[0x3004u] = 0u;
    for (uint32_t offset = 0; offset < 16u; offset += 4u) {
      words[0x4000u + offset] = offset == 0u ? 0x1110u : 0u;
      words[0x5000u + offset] = offset == 0u ? 0x2220u : 0u;
    }
  }

  static bool Read(void *userdata, uint64_t address,
                   std::span<uint32_t> output) {
    auto &fixture = *static_cast<ExternalMaterialFixture *>(userdata);
    fixture.requested.push_back(address);
    if (address == fixture.reject_address)
      return false;
    for (size_t i = 0; i < output.size(); ++i) {
      const auto found = fixture.words.find(address + i * sizeof(uint32_t));
      if (found == fixture.words.end())
        return false;
      output[i] = found->second;
    }
    return true;
  }

  Libs::Graphics::ShaderRecompiler::IR::SrtRuntime Runtime() {
    return {.read_memory = Read,
            .userdata = this,
            .read_specialization_memory = Read,
            .external_context_domains = domains};
  }
};

void TestExternalContextNestedDescriptors() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  ExternalMaterialFixture fixture;
  Check(!ValidateRuntimeValue(fixture.program, Value(fixture.low)),
        "external context marker was accepted without an explicit context");
  Check(ValidateRuntimeValue(fixture.program, Value(fixture.low),
                             RuntimeValueType::Integer,
                             ExternalCallContextBinding{11u, 7u}),
        "explicit external context marker was rejected");
  auto plan = ExtractResourcePlan(fixture.program);
  auto runtime = fixture.Runtime();
  DescriptorValue descriptor;
  Check(!SrtWalker(plan, runtime).EvaluateDescriptor(0u, descriptor) &&
            fixture.requested.empty(),
        "default host evaluator read a selected GPU context");
  {
    SrtWalker evaluator(plan, CleanRuntime(runtime), {}, nullptr, {},
                        SrtExternalContext{11u, &fixture.records[1]});
    Check(evaluator.EvaluateDescriptor(0u, descriptor) &&
              descriptor.dwords[0] == 0x2220u,
          "context descriptor did not preserve the selected record auxiliary "
          "pointer");
  }
  Check(
      fixture.low->Arg(0) == fixture.lane &&
          fixture.low->Arg(2) == fixture.lane &&
          fixture.high->Arg(0) == fixture.lane &&
          fixture.high->Arg(2) == fixture.lane,
      "host extraction changed executable raw auxiliary or GPU ordinal values");
  fixture.requested.clear();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "complete external nested descriptor materialization failed");
  Check(snapshot.samplers.size() == 3u &&
            specialization.samplers.size() == 3u &&
            snapshot.samplers[1].dwords[0] == 0x1110u &&
            snapshot.samplers[2].dwords[0] == 0x2220u,
        "same native function lost distinct auxiliary resource payloads");
  const auto &root = specialization.samplers[0];
  const auto offset = root.indirect_mapping_offset;
  Check(
      root.indirect_root == 0u && root.indirect_search_iterations == 2u &&
          snapshot.flattened_srt.size() >= offset + 7u &&
          std::vector<uint32_t>(snapshot.flattened_srt.begin() + offset,
                                snapshot.flattened_srt.begin() + offset + 7u) ==
              std::vector<uint32_t>{3u, 3u, 1u, 9u, 2u, 21u, 1u},
      "sparse record keys were dropped or duplicate payload keys were merged");
  Check(fixture.requested.size() == 18u,
        "nested context descriptor dependencies were read repeatedly within "
        "one context");
  Check(snapshot.external_descriptor_candidate_count == 3u &&
            snapshot.external_descriptor_read_bytes == 72u,
        "external planning counters did not report actual candidate tuples and "
        "requested bytes");
  ApplyResourceSpecialization(fixture.program, specialization);
  Check(fixture.program.info.samplers[0].indirect_resources ==
            std::vector<uint32_t>{0u, 1u, 2u},
        "external sampler specialization did not preserve candidate ordinals");
}

void TestExternalDefaultSamplerRetainsKeyDomain() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  ExternalMaterialFixture fixture;
  for (uint32_t offset = 0; offset < 16u; offset += 4u) {
    fixture.words[0x4000u + offset] = 0u;
    fixture.words[0x5000u + offset] = 0u;
  }
  auto plan = ExtractResourcePlan(fixture.program);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(
      MaterializeResources(plan, fixture.Runtime(), snapshot, specialization) &&
          snapshot.samplers.size() == 1u &&
          specialization.samplers[0].indirect_root == 0u,
      "default sampler discarded its external context mapping");
  const auto offset = specialization.samplers[0].indirect_mapping_offset;
  Check(
      std::vector<uint32_t>(snapshot.flattened_srt.begin() + offset,
                            snapshot.flattened_srt.begin() + offset + 7u) ==
          std::vector<uint32_t>{3u, 3u, 0u, 9u, 0u, 21u, 0u},
      "default sampler lost valid sparse keys or invented a child descriptor");
  ApplyResourceSpecialization(fixture.program, specialization);
  Check(
      fixture.program.info.samplers[0].indirect_resources ==
          std::vector<uint32_t>{0u},
      "default sampler cannot preserve its single-candidate runtime dispatch");
}

void TestExternalContextRejectsIncompleteOrUnprovedReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  for (uint32_t variant = 0; variant < 6u; ++variant) {
    ExternalMaterialFixture fixture;
    if (variant == 0u)
      fixture.domains[0].complete = false;
    if (variant == 1u)
      fixture.domains[0].domain_id = 12u;
    if (variant == 2u)
      fixture.records[2].ordinal = fixture.records[0].ordinal;
    if (variant == 3u)
      fixture.reject_address = 0x5008u;
    if (variant == 4u) {
      fixture.high->SetArg(4u, Value(8u));
      fixture.program.external_context_bindings.push_back({11u, 8u});
    }
    auto plan = ExtractResourcePlan(fixture.program);
    auto runtime = fixture.Runtime();
    if (variant == 5u)
      runtime.read_specialization_memory = nullptr;
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization),
          "incomplete, mixed or unreadable external resource context was "
          "accepted");
    if (variant == 3u) {
      Check(!fixture.requested.empty() && fixture.requested.back() == 0x5008u,
            "external nested read failure was reported at a different guest "
            "address");
    } else {
      Check(fixture.requested.empty(),
            "invalid context or missing strict reader fell back to ordinary "
            "guest reads");
    }
  }
}

void TestExternalTypedNullImageRetainsKeyDomain() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::Prospero::TextureNumericClass;
  using Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension;
  for (const bool concrete_type : {true, false}) {
    ExternalMaterialFixture fixture;
    for (uint32_t offset = 0; offset < 16u; offset += 4u) {
      fixture.words[0x4000u + offset] = 0u;
      fixture.words[0x5000u + offset] = 0u;
    }
    fixture.program.info.samplers.clear();
    auto &expression = fixture.program.descriptor_sources[0];
    expression.dword_count = 8u;
    for (uint32_t word = 4u; word < 8u; ++word)
      expression.dwords[word] = Value(0u);
    fixture.program.descriptor_sources[1].dword_count = 8u;
    fixture.program.info.images.push_back(
        {.source = 1u,
         .resource_class = ImageResourceClass::Sampled,
         .numeric_class = concrete_type ? TextureNumericClass::Uint
                                        : TextureNumericClass::Unsupported,
         .dimension = ImageDimension::Dim3D,
         .read = true});
    auto plan = ExtractResourcePlan(fixture.program);
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    const auto result =
        MaterializeResources(plan, fixture.Runtime(), snapshot, specialization);
    if (!concrete_type) {
      Check(!result,
            "all-null external image invented an unproved numeric format");
      continue;
    }
    Check(result && snapshot.images.size() == 1u &&
              specialization.images[0].indirect_root == 0u &&
              specialization.images[0].numeric_class ==
                  TextureNumericClass::Uint &&
              specialization.images[0].dimension == ImageDimension::Dim3D,
          "typed-null external image lost its proven class or context mapping");
    ApplyResourceSpecialization(fixture.program, specialization);
    Check(fixture.program.info.images[0].indirect_resources ==
              std::vector<uint32_t>{0u},
          "typed-null image did not retain its single-candidate runtime "
          "dispatch");
  }
}

void TestExternalBindingAliasesRemainStructural() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto program = MixedSamplerProgram();
  program.external_context_bindings.push_back({11u, 7u});
  const auto valid = program.descriptor_sources[1];
  program.descriptor_sources[0] = valid;
  auto &block = *program.blocks[0];
  auto &address = block.AppendNewInst(ValueOpcode::GetUserData,
                                      {Value(static_cast<ScalarReg>(2))});
  program.descriptor_sources[1].dwords[0] = Value(&address);
  std::array<uint32_t, 3> user_data{0x1230u, 0x1230u, 1u};
  auto plan = ExtractResourcePlan(program);
  ResourceSnapshot snapshot;
  ResourceSpecialization first, changed;
  const SrtRuntime runtime{.user_data = user_data};
  Check(MaterializeResources(plan, runtime, snapshot, first) &&
            first.images[1].binding_alias == 0u &&
            first.sampler_binding_aliases.size() == 2u &&
            first.sampler_binding_aliases[1] == 0u,
        "equivalent descriptors across distinct roots did not share canonical "
        "bindings");
  user_data[2] = 2u;
  user_data[1] = 0x1240u;
  Check(MaterializeResources(plan, runtime, snapshot, changed) &&
            changed != first && changed.images[1].binding_alias == UINT32_MAX &&
            changed.sampler_binding_aliases[1] == UINT32_MAX,
        "changed runtime descriptor equality did not change cache binding "
        "topology");
  user_data[2] = 1u;
  user_data[1] = 0x1230u;
  Check(MaterializeResources(plan, runtime, snapshot, changed) &&
            changed == first,
        "canonical binding pattern depended on absolute descriptor payload "
        "identity");
  ApplyResourceSpecialization(program, changed);
  Check(program.info.images[1].binding_alias == 0u &&
            program.info.samplers[1].binding_alias == 0u,
        "binding aliases were not carried into executable resource metadata");

  auto mixed = MixedSamplerProgram();
  mixed.external_context_bindings.push_back({11u, 7u});
  auto mixed_plan = ExtractResourcePlan(mixed);
  ResourceSpecialization classes;
  user_data[0] = user_data[1] = 0u;
  Check(
      MaterializeResources(mixed_plan, runtime, snapshot, classes) &&
          classes.sampler_binding_aliases.size() == 3u &&
          classes.sampler_binding_aliases[1] == 0u &&
          classes.sampler_binding_aliases[2] == UINT32_MAX,
      "integer point-filtering sampler class aliased a float sampler binding");
}


void MakeImageMemoryLivenessFixture(
    Libs::Graphics::ShaderRecompiler::IR::Program& program,
    Libs::Graphics::ShaderRecompiler::IR::ResourceSpecialization& specialization,
    bool active_out_of_range = false) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.resource_tracking_complete = true;
  program.info.images.resize(2);
  for (auto& image : program.info.images) {
    image.resource_class = ImageResourceClass::Sampled;
    image.dimension = ImageDimension::Dim2D;
    image.read = true;
  }
  specialization.images.resize(2);
  for (auto& image : specialization.images) {
    image.numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float;
    image.dimension = ImageDimension::Dim2D;
    image.mip_count = 1;
  }
  // Removing a leading FMASK entry proves the live remap still occurs exactly.
  specialization.images[0].fmask = true;
  MemoryInfo live;
  live.kind = ResourceKind::Image;
  live.resource = active_out_of_range ? 7u : 1u;
  live.image_dimension = ImageDimension::Dim2D;
  live.data_dwords = 4u;
  program.memory_info.push_back(live);
  MemoryInfo dead = live;
  dead.resource = 7u;
  program.memory_info.push_back(dead);
  dead.planning_only = true;
  program.memory_info.push_back(dead);
  auto& block = AddValueBlock(program);
  auto& handle = block.AppendNewInst(ValueOpcode::GetImageResource,
      {Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u),Value(0u)});
  handle.SetFlags(1u);
  auto& coordinates = block.AppendNewInst(ValueOpcode::CompositeConstructU32x4,
      {Value(0u),Value(0u),Value(0u),Value(0u)});
  auto& read = block.AppendNewInst(ValueOpcode::ImageRead,
      {Value(&handle),Value(&coordinates),Value(true)});
  read.SetFlags(MemoryFlags{.index=0u,.pc=0xb0u});
}

void TestDeadImageMemoryMetadataDoesNotRemap() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  ResourceSpecialization specialization;
  MakeImageMemoryLivenessFixture(program,specialization);
  const auto dead_before=program.memory_info[1];
  const auto planning_before=program.memory_info[2];
  ApplyResourceSpecialization(program,specialization);
  Check(program.info.images.size()==1u && program.memory_info[0].resource==0u,
        "surviving image metadata lost the leading-FMASK index permutation");
  Check(program.memory_info[1]==dead_before,
        "unused image metadata was clamped, assigned a guessed image, or otherwise changed");
  Check(program.memory_info[2]==planning_before && program.memory_info[2].planning_only,
        "planning-only metadata semantics changed during executable image remap");
  Check(program.blocks[0]->Instructions().front().Flags<uint32_t>()==0u,
        "live GetImageResource handle did not receive the same image permutation");
}

void TestLiveOutOfRangeImageMemoryStillRejects() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  ResourceSpecialization specialization;
  MakeImageMemoryLivenessFixture(program,specialization,true);
  std::puts("Expected strict active image-resource bound rejection");
  std::fflush(stdout);
  ApplyResourceSpecialization(program,specialization);
  Check(false,"out-of-range executable image metadata was ignored or clamped");
}

} // namespace

namespace Common {

int DbgExitHandler(const char *, int, std::string_view) { std::abort(); }

int DbgExitHandler(const char *, int, fmt::text_style, std::string_view) {
  std::abort();
}

int DbgExitIfHandler(const char *, const char *, int) { return 1; }

void DbgExit(int) { std::abort(); }

} // namespace Common

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--dead-image-metadata-only") == 0) {
    TestDeadImageMemoryMetadataDoesNotRemap();
    std::puts("ResourceMaterializationTests: dead/planning/live image metadata passed");
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--live-image-metadata-out-of-range-only") == 0) {
    TestLiveOutOfRangeImageMemoryStillRejects();
    return 1;
  }

  if (argc == 2 && std::strcmp(argv[1], "--indirect-image-operations-only") == 0) {
    TestSupportedIndirectImageOperationsSpecialize();
    return 0;
  }
  TestMappedSrtUsesDirectReaderByDefault();
  TestIntegerRuntimeValueFollowsSrtReads();
  TestUniformVectorDescriptorRead();
  TestExactReciprocalDescriptorArithmetic();
  TestUnbasedFlatCacheHitMaterializes();
  TestWrittenDescriptorUsesStrictReaderOnce();
  TestFailedMaterializationRejectsStage();
  TestFiniteImageRefreshReusesScalarReads();
  TestSupportedIndirectImageOperationsSpecialize();
  TestMixedSamplerVariantsShareRuntimeDescriptor();
  TestExternalContextNestedDescriptors();
  TestExternalDefaultSamplerRetainsKeyDomain();
  TestExternalContextRejectsIncompleteOrUnprovedReads();
  TestExternalTypedNullImageRetainsKeyDomain();
  TestExternalBindingAliasesRemainStructural();
  TestDeadImageMemoryMetadataDoesNotRemap();
  std::puts("ResourceMaterializationTests: all cases passed");
  return 0;
}

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
