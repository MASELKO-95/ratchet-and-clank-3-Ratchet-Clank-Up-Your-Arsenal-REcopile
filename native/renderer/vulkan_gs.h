#pragma once
#include "runtime/gs/gs_backend.h"
#include <memory>
#include <string>

class PS2Runtime;
// Owns a Vulkan compute GS and an SDL Vulkan window. No GL context or interop.
class VulkanGS final : public GSRasterBackend {
public:
  explicit VulkanGS(bool window = true, bool fsr = true,
                    bool deinterlace = true);
  ~VulkanGS() override;
  void open(PS2Runtime &, const char *title);
  bool frame(PS2Runtime &);
  void capture(const std::string &filename);
  const std::string &deviceName() const;
  uint64_t drawCount() const;
  void setOutputSize(uint32_t width, uint32_t height);
  void Initialize(uint8_t *, uint32_t) override;
  void Reset() override;
  void Submit(const GSPrimitiveBatch &) override;
  void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override;
  void BeginTransfer(const GSTransferCommand &) override;
  void UploadImage(const uint8_t *, uint32_t) override;
  void Flush() override;
  void TextureFlush() override;
  void Sync(GSSyncReason) override;
  PresentationFrame Present(const GSPresentationRequest &) override;
  bool ClearFramebuffer(const GSContext &, uint32_t) override;
  uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override;
  uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t,
                    uint32_t) const override;
  void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                 uint32_t) override;
  void SnapshotVram(std::vector<uint8_t> &) const override;
  GSTransferSnapshot GetTransferSnapshot() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
