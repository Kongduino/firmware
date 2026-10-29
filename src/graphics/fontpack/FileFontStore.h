#pragma once

#include "FSCommon.h"
#include "graphics/fontpack/FontStore.h"

#ifdef FSCom

namespace graphics
{

/// A font pack kept as a file on the device's filesystem (FSCom). The file stays open while the pack is in use.
class FileFontStore : public FontStore
{
  public:
    ~FileFontStore() override { close(); }

    bool open(const char *path);
    void close();

    uint32_t size() const override { return fileSize; }
    bool read(uint32_t offset, void *buf, uint32_t len) override;

  private:
    File file;
    bool isOpen = false;
    uint32_t fileSize = 0;
    uint32_t position = UINT32_MAX; // where the next read starts without a seek; UINT32_MAX when unknown
};

} // namespace graphics

#endif
