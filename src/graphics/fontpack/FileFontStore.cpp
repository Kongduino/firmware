#include "graphics/fontpack/FileFontStore.h"

#ifdef FSCom

#include "SPILock.h"

namespace graphics
{

bool FileFontStore::open(const char *path)
{
    close();
    concurrency::LockGuard g(spiLock);
    file = FSCom.open(path, FILE_O_READ);
    if (!file)
        return false;
    if (file.isDirectory()) {
        file.close();
        return false;
    }
    fileSize = file.size();
    position = 0;
    isOpen = true;
    return true;
}

void FileFontStore::close()
{
    if (!isOpen)
        return;
    concurrency::LockGuard g(spiLock);
    file.close();
    isOpen = false;
    fileSize = 0;
    position = UINT32_MAX;
}

bool FileFontStore::read(uint32_t offset, void *buf, uint32_t len)
{
    if (!isOpen || offset > fileSize || len > fileSize - offset)
        return false;
    if (len == 0)
        return true;
    concurrency::LockGuard g(spiLock);
    if (position != offset && !file.seek(offset)) {
        position = UINT32_MAX;
        return false;
    }
    const size_t got = file.read(static_cast<uint8_t *>(buf), len);
    position = (got == len) ? offset + len : UINT32_MAX;
    return got == len;
}

} // namespace graphics

#endif
