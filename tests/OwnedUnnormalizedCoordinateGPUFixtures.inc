// Owned native instructions and texels only. No guest mapping, game data, or TextureCache.
// Include after OwnedUnnormalizedCoordinateFixtures.inc and after RunCase is defined.
namespace Libs::Graphics {
namespace {
namespace UNormGpuFixture {
u32 FloatWord(float value) { return std::bit_cast<u32>(value); }
float Texel(u32 mip, u32 layer, u32 x, u32 y, u32 component) {
  // Every axis, mip, layer, and component has a distinct exactly representable value.
  return static_cast<float>(10000u * mip + 1000u * layer + 100u * y + 10u * x + component + 1u);
}
std::vector<std::vector<u32>> Pixels(u32 width, u32 height, u32 levels, u32 layers) {
  std::vector<std::vector<u32>> result;
  for (u32 mip = 0; mip < levels; ++mip) {
    const u32 w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
    auto& words = result.emplace_back();
    for (u32 layer = 0; layer < layers; ++layer)
      for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
          for (u32 c = 0; c < 4u; ++c) words.push_back(FloatWord(Texel(mip, layer, x, y, c)));
  }
  return result;
}
std::vector<u32> GuardedExpected(u32 mip, u32 layer, u32 x, u32 y) {
  std::vector<u32> words{0x91a2b3c4u, 0xe5f60718u, 0x2b3c4d5eu, 0x6f708192u,
                         0xa3b4c5d6u, 0xe7f8091au, 0x13579bdfu, 0x2468ace0u};
  for (u32 c = 0; c < 4u; ++c) words[2u + c] = FloatWord(Texel(mip, layer, x, y, c));
  return words;
}
TestCase MakeCase(u32 form, u32 flags, bool runtime_texel_control = false) {
  Require("UNormGpuFactory", "form", form < 6u && flags < 4u, "invalid authored form/flag");
  // Flags use bit0=S# ForceUnnormalized, bit1=native instruction UNRM.
  const bool force = (flags & 1u) != 0u, native = (flags & 2u) != 0u;
  const bool gradient = form >= 3u;
  const bool base_mip = form == 1u || form == 4u;
  const bool array = form == 2u || form == 5u;
  const bool texel_units = force || native || runtime_texel_control;
  TestCase test;
  test.name = "OwnedUnnormalizedReturnedTexels";
  test.image_width = base_mip ? 16u : 8u;
  test.image_height = base_mip ? 8u : 4u;
  test.sampled_image_view_base_mip = base_mip ? 1u : 0u;
  test.sampled_image_layers = array ? 4u : 1u;
  test.sampled_image_view_base_layer = array ? 1u : 0u;
  test.sampled_image_view_layers = array ? 3u : 1u;
  test.sampled_image_view_type = array ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
  const u32 levels = base_mip ? 4u : 3u;
  test.sampled_image_rgba_mips = Pixels(test.image_width, test.image_height, levels, test.sampled_image_layers);
  test.sampled_image_format = vk::Format::eR32G32B32A32Sfloat;
  test.sampled_image_dwords_per_pixel = 4u;
  test.sampler_filter = vk::Filter::eNearest;
  test.initial = GuardedExpected(0u, 0u, 0u, 0u);
  // Output positions 2..5 start with unrelated values; 0,1,6,7 are never written.
  test.initial[2] = 0x2b3c4d5eu; test.initial[3] = 0x6f708192u;
  test.initial[4] = 0xa3b4c5d6u; test.initial[5] = 0xe7f8091au;
  test.expected = GuardedExpected(test.sampled_image_view_base_mip + (gradient ? 1u : 0u),
                                 array ? 3u : 0u, gradient ? 2u : 5u, 1u);
  test.user_data = MakeSampledTextureData(Prospero::BufferFormat::k32_32_32_32Float);
  const u32 width_minus_one = test.image_width - 1u;
  test.user_data[1] |= (width_minus_one & 3u) << 30u;
  test.user_data[2] = (width_minus_one >> 2u) | ((test.image_height - 1u) << 14u);
  test.user_data[3] = (static_cast<u32>(array ? Prospero::ImageType::kColor2DArray :
                                      Prospero::ImageType::kColor2D) << 28u) |
                      (test.sampled_image_view_base_mip << 12u) | ((levels - 1u) << 16u);
  test.user_data[4] = array ? 1u | (3u << 16u) : 0u; // Native view covers guest layers 1..3.
  test.user_data[5] = (levels - 1u) << 4u;
  test.user_data[8] = 0x92u | (force ? 1u << 15u : 0u); // clamp-to-edge, no Dref.
  test.user_data[9] = 0xfff000u;
  test.user_data[10] = 1u << 26u; // Point mip selection, no bias or min-LOD adjustment.
  test.user_data[50] = static_cast<u32>(test.initial.size() * sizeof(u32));
  test.user_data[51] = 3u << 28u;
  test.has_user_data = true;
  test.has_compute_info = true;
  test.compute_info.threads_num[0] = test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1u;
  test.compute_info.thread_ids_num = 0u;
  std::vector<float> address;
  if (gradient) {
    // At native view size8x4, rho=max(2,1)=2, so the nearest mip is view-relative1.
    address = texel_units ? std::vector<float>{2.0f, 0.0f, 0.0f, 1.0f} :
                            std::vector<float>{0.25f, 0.0f, 0.0f, 0.25f};
  }
  address.push_back(texel_units ? (gradient ? 5.0f : 5.5f) : (gradient ? 0.625f : 0.6875f));
  address.push_back(texel_units ? (gradient ? 3.0f : 1.5f) : (gradient ? 0.75f : 0.375f));
  if (array) address.push_back(2.0f); // Relative layer2 means allocated layer3; division by3 would select layer2.
  for (u32 i = 0; i < address.size(); ++i) AppendVMovLiteral(&test.code, 20u + i, FloatWord(address[i]));
  test.code.push_back(EncodeMimg0(gradient ? 0x22u : 0x27u, 0xfu, 0u, false, array ? 5u : 1u) |
                      (native ? 1u << 12u : 0u));
  test.code.push_back(EncodeMimg1(0u, 20u, 0u, 2u));
  for (u32 c = 0; c < 4u; ++c) AppendStoreVgpr(&test.code, c, 2u + c);
  AppendEnd(&test.code);
  test.opcodes = {ShaderOpcode::V_MOV_B32, ShaderOpcode::IMAGE_SAMPLE,
                  ShaderOpcode::BUFFER_STORE_DWORD, ShaderOpcode::S_ENDPGM};
  test.required_spirv = gradient ? std::vector<std::string>{"OpImageSampleExplicitLod", "Grad"} :
                                  std::vector<std::string>{"OpImageSampleExplicitLod", "Lod"};
  return test;
}
void RuntimeSamplerBit(VulkanHarness& vulkan) {
  auto test = MakeCase(0u, 0u, true); // Same texel-unit constants while only the live S# word changes.
  auto compiled = CompileCase(test, vulkan.SubgroupSize());
  const auto original_words = compiled.spirv;
  const auto& regs = compiled.program.bindings.user_data_registers;
  const auto reg = std::ranges::find(regs, 8u);
  Require(test.name, "runtime original S# word", reg != regs.end(), "sampler word0 not retained");
  const size_t slot = static_cast<size_t>(reg - regs.begin());
  Require(test.name, "runtime packed S# word", slot < compiled.packed_user_data.size(), "packed sampler slot absent");
  auto image = vulkan.CreateImageMips(test.name, 8u, 4u, test.sampled_image_format,
      vk::ImageUsageFlagBits::eSampled, test.sampled_image_rgba_mips, 4u,
      vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageType::e2D, vk::ImageViewType::e2D, 1u);
  auto sampler = vulkan.CreateSampler(test.name, image.mip_levels, vk::Filter::eNearest);
  for (const bool force : {false, true, false}) {
    compiled.packed_user_data[slot] = (test.user_data[8] & ~(1u << 15u)) | (force ? 1u << 15u : 0u);
    auto buffer = vulkan.CreateStorageBuffer(test.name, test.initial, test.initial.size());
    vulkan.Dispatch(test, compiled, buffer, nullptr, &image, nullptr, nullptr, sampler);
    const auto actual = vulkan.ReadBuffer(test.name, buffer, test.initial.size());
    const auto expected = GuardedExpected(0u, 0u, force ? 5u : 7u, force ? 1u : 3u);
    CompareWords(test, "same module live sampler bit + four guards", expected, actual);
    Require(test.name, "module unchanged", compiled.spirv == original_words, "runtime control changed emitted module");
    vulkan.DestroyBuffer(&buffer);
    std::printf("PASS UNormGPU same-module force=%u four_guard_words=4\n", force ? 1u : 0u);
  }
  vulkan.Device().destroySampler(sampler, nullptr);
  vulkan.DestroyImage(&image);
}
void Run(VulkanHarness* vulkan) {
  size_t cases = 0;
  for (u32 flags = 0; flags < 4u; ++flags) for (u32 form = 0; form < 6u; ++form) {
    auto test = MakeCase(form, flags);
    if (vulkan == nullptr) {
      const auto shader = CompileCase(test, 32u);
      UNormFixture::CheckModule(shader, 2u, form == 2u || form == 5u ? 3u : 2u,
                                (flags & 2u) != 0u, form >= 3u, false, false, false);
    } else RunCase(vulkan, test);
    std::printf("PASS UNorm%s flags=%u form=%u expectedRGBA=%u,%u,%u,%u four_guard_words=4\n",
                vulkan == nullptr ? "Prepare" : "GPU", flags, form,
                test.expected[2], test.expected[3], test.expected[4], test.expected[5]);
    ++cases;
  }
  if (vulkan != nullptr) RuntimeSamplerBit(*vulkan);
  std::printf("PASS UNorm%s modules=%zu dispatches=%zu; owned native sample_lz/sample_d manual image ABI only\n",
              vulkan == nullptr ? "Prepare" : "GPU", cases + (vulkan != nullptr ? 1u : 0u),
              vulkan == nullptr ? 0u : cases + 3u);
}
} // namespace UNormGpuFixture
} // namespace
} // namespace Libs::Graphics
