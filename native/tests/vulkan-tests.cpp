#include "raylib.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "vulkan_gs.h"
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *what) {
  if (!condition)
    throw std::runtime_error(what);
}
void equal(GSCpuBackend &cpu, VulkanGS &gpu, const char *label) {
  std::vector<uint8_t> a, b;
  cpu.SnapshotVram(a);
  gpu.SnapshotVram(b);
  if (a != b) {
    size_t i = 0;
    while (i < a.size() && a[i] == b[i])
      ++i;
    throw std::runtime_error(
        std::string(label) + ": VRAM differs at " + std::to_string(i) +
        " CPU=" + std::to_string(a[i]) + " GPU=" + std::to_string(b[i]));
  }
}
GSPrimitiveBatch sprite() {
  GSPrimitiveBatch b{};
  b.vertexCount = 2;
  auto &s = b.state;
  s.prim.type = GS_PRIM_SPRITE;
  s.colclamp = 1;
  s.context.frame = {16, 2, GS_PSM_CT32, 0};
  s.context.zbuf = {32, GS_PSM_Z32, true};
  s.context.scissor = {0, 127, 0, 127};
  s.context.test = 1u << 17;
  b.vertices[0].x = 3;
  b.vertices[0].y = 5;
  b.vertices[1].x = 61;
  b.vertices[1].y = 29;
  b.vertices[1].r = 64;
  b.vertices[1].g = 120;
  b.vertices[1].b = 200;
  b.vertices[1].a = 128;
  return b;
}
} // namespace
int main() {
  try {
    VulkanGS gpu(false, true, true);
    GSCpuBackend cpu;
    std::vector<uint8_t> a(4u * 1024u * 1024u), b(a.size());
    cpu.Initialize(a.data(), a.size());
    gpu.Initialize(b.data(), b.size());
    for (uint32_t format :
         {0u, 1u, 2u, 10u, 19u, 20u, 27u, 36u, 44u, 48u, 49u, 50u, 58u}) {
      std::cout << "PSM " << format << '\n' << std::flush;
      cpu.Reset();
      gpu.Reset();
      // Cross page/block boundaries, including non-aligned texture bases.
      for (uint32_t y : {0u, 7u, 16u, 31u, 63u, 64u, 129u})
        for (uint32_t x : {0u, 3u, 16u, 31u, 64u, 127u, 135u}) {
          uint32_t c = 0xa59d7b31u ^ (x * 1234567u + y * 13579u);
          cpu.WriteVram(format, 37, 4, x, y, c);
          gpu.WriteVram(format, 37, 4, x, y, c);
        }
      equal(cpu, gpu, "addressing");
      GSTransferCommand upload{};
      upload.direction = 0;
      upload.bitbltbuf = {0, 0, 0, 513, 2, uint8_t(format)};
      upload.trxpos.dsax = 9;
      upload.trxpos.dsay = 17;
      upload.trxreg = {24, 12};
      cpu.BeginTransfer(upload);
      gpu.BeginTransfer(upload);
      std::vector<uint8_t> data(1152);
      for (size_t i = 0; i < data.size(); i++)
        data[i] = i * 37 + 11;
      // Arbitrary GIF IMAGE chunk boundaries, with retained partial pixels.
      cpu.UploadImage(data.data(), data.size());
      for (size_t off = 0; off < data.size(); off += 16)
        gpu.UploadImage(data.data() + off, 16);
      equal(cpu, gpu, "image transfer");
    }
    cpu.Reset();
    gpu.Reset();
    auto batch = sprite();
    cpu.Submit(batch);
    gpu.Submit(batch);
    equal(cpu, gpu, "solid sprite");
    batch.state.prim.abe = true;
    batch.state.context.alpha = 0x44;
    batch.vertices[1].a = 64;
    batch.vertices[1].r = 240;
    cpu.Submit(batch);
    gpu.Submit(batch);
    equal(cpu, gpu, "alpha blending");
    batch.state.context.frame.fbmsk = 0xff00ff00;
    cpu.Submit(batch);
    gpu.Submit(batch);
    equal(cpu, gpu, "frame mask");
    for (uint32_t fmt : {0u, 1u, 2u, 10u}) {
      auto c = batch.state.context;
      c.frame.psm = fmt;
      c.fba = 1;
      c.frame.fbmsk = 0xff0000f0;
      cpu.ClearFramebuffer(c, 0x404080ff);
      gpu.ClearFramebuffer(c, 0x404080ff);
      equal(cpu, gpu, "masked clear");
    }
    for (uint32_t fail = 0; fail < 4; fail++) {
      batch.state.context.test = (1u << 17) | 1u | (fail << 12);
      cpu.Submit(batch);
      gpu.Submit(batch);
      equal(cpu, gpu, "alpha fail mode");
    }
    cpu.Reset();
    gpu.Reset();
    batch = sprite();
    batch.state.prim.tme = true;
    batch.state.prim.fst = true;
    batch.state.textureWidth = 32;
    batch.state.textureHeight = 32;
    batch.state.context.tex0 = {1536, 1,    GS_PSM_T8,   5, 5, 1,
                                1,    1600, GS_PSM_CT32, 0, 0, 1};
    batch.state.texa.ta0 = 128;
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t sw = (i & ~24u) | ((i & 8u) << 1u) | ((i & 16u) >> 1u);
      uint32_t color = 0x80000000u | (i * 98765 & 0xffffffu);
      cpu.WriteVram(0, 1600, 1, sw & 15, sw >> 4, color);
      gpu.WriteVram(0, 1600, 1, sw & 15, sw >> 4, color);
    }
    for (uint32_t y = 0; y < 32; y++)
      for (uint32_t x = 0; x < 32; x++) {
        cpu.WriteVram(19, 1536, 1, x, y, (x + y * 3) & 255);
        gpu.WriteVram(19, 1536, 1, x, y, (x + y * 3) & 255);
      }
    cpu.LoadClut(batch.state.context.tex0, {});
    gpu.LoadClut(batch.state.context.tex0, {});
    batch.vertices[1].u = 32 * 16;
    batch.vertices[1].v = 32 * 16;
    cpu.Submit(batch);
    gpu.Submit(batch);
    equal(cpu, gpu, "indexed sprite");
    batch.state.linearFilter = true;
    cpu.Submit(batch);
    gpu.Submit(batch);
    equal(cpu, gpu, "bilinear indexed sprite");
    // The 3D path interpolates S/T/Q rather than already-divided UVs.
    batch.vertexCount = 3;
    batch.state.prim.type = GS_PRIM_TRIANGLE;
    batch.state.prim.iip = false;
    batch.state.prim.fst = false;
    batch.vertices[0].x = 3;
    batch.vertices[0].y = 5;
    batch.vertices[1].x = 61;
    batch.vertices[1].y = 5;
    batch.vertices[2].x = 3;
    batch.vertices[2].y = 53;
    batch.vertices[0].s = 0.5f;
    batch.vertices[0].t = 0.25f;
    batch.vertices[0].q = 1;
    batch.vertices[1].s = 1;
    batch.vertices[1].t = 0.25f;
    batch.vertices[1].q = 2;
    batch.vertices[2].s = 0.5f;
    batch.vertices[2].t = 0.25f;
    batch.vertices[2].q = 1;
    batch.vertices[2].a = 128;
    cpu.Submit(batch);
    gpu.Submit(batch);
    equal(cpu, gpu, "perspective textured triangle");
    GSTransferCommand copy{};
    copy.direction = 2;
    copy.bitbltbuf = {512, 2, 0, 512, 2, 0};
    copy.trxpos = {3, 5, 11, 7, 3};
    copy.trxreg = {50, 24};
    cpu.BeginTransfer(copy);
    gpu.BeginTransfer(copy);
    equal(cpu, gpu, "overlapping copy");
    // A synthetic interlaced buffer has green even fields, red odd fields.
    // The GPU must reconstruct the current field without alternating bars.
    gpu.Reset();
    GSTransferCommand field{};
    field.direction = 0;
    field.bitbltbuf.dbw = 1;
    field.trxreg = {64, 64};
    gpu.BeginTransfer(field);
    std::vector<uint32_t> pixels(64 * 64);
    for (uint32_t y = 0; y < 64; y++)
      for (uint32_t x = 0; x < 64; x++)
        pixels[y * 64 + x] = y % 2 ? 0x800000ff : 0x8000ff00;
    gpu.UploadImage(reinterpret_cast<const uint8_t *>(pixels.data()),
                    pixels.size() * 4);
    GSPresentationRequest request{};
    request.pmode = 1;
    request.smode2 = 3;
    request.dispfb1 = 1u << 9;
    request.display1 = (uint64_t(63) << 32) | (uint64_t(63) << 44);
    gpu.setOutputSize(128, 128);
    auto frame = gpu.Present(request);
    require(bool(frame), "Vulkan scanout is empty");
    auto path = std::filesystem::temp_directory_path() /
                "ratchet-vulkan-field-test.png";
    gpu.capture(path.string());
    Image image = LoadImage(path.c_str());
    require(image.width == 128 && image.height == 128,
            "Unexpected FSR output size");
    auto *rgba = static_cast<uint8_t *>(image.data);
    for (int y = 0; y < 128; y++)
      for (int x = 0; x < 128; x++)
        require(rgba[(y * 128 + x) * 4] < 3 &&
                    rgba[(y * 128 + x) * 4 + 1] > 252,
                "Interlace stripes survive GPU deinterlacing/FSR");
    UnloadImage(image);
    request.vsyncTick = 1;
    gpu.Present(request);
    gpu.capture(path.string());
    image = LoadImage(path.c_str());
    rgba = static_cast<uint8_t *>(image.data);
    for (int y = 0; y < 128; y++)
      require(rgba[y * 128 * 4] > 252 && rgba[y * 128 * 4 + 1] < 3,
              "Odd field parity is incorrect");
    UnloadImage(image);
    require(gpu.drawCount() > 0, "No raster work reached the GPU");
    std::cout
        << "Vulkan GS on " << gpu.deviceName()
        << ": addressing/IMAGE/CLUT/blend/mask/copy/deinterlace/FSR passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
