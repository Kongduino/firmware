#include "graphics/fontpack/StrokeInput.h"

#if HAS_FONTPACKS

#include "DebugConfiguration.h"
#include "concurrency/LockGuard.h"
#include "graphics/fontpack/FileFontStore.h"
#include "graphics/fontpack/strokedict.h"

namespace graphics
{
namespace StrokeInput
{
namespace
{

// The input thread types, the screen thread draws, and XModem closes the dictionary before replacing it
concurrency::Lock &inputLock()
{
    static concurrency::Lock lock;
    return lock;
}

FileFontStore store;
sd_dict dict;
enum class State { Closed, Open, Missing } state = State::Closed;

uint8_t strokes[SD_MAX_STROKES];
int strokeTotal = 0;
uint32_t candidates[SD_MAX_CANDIDATES];
int candidateTotal = 0;
int currentPage = 0;

bool storeRead(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    return static_cast<FileFontStore *>(ctx)->read(off, buf, len);
}

bool openLocked();

// Lock held
void lookUp()
{
    candidateTotal = 0;
    currentPage = 0;
    if (strokeTotal > 0 && openLocked())
        candidateTotal = sd_lookup(&dict, strokes, strokeTotal, 0, candidates, SD_MAX_CANDIDATES, nullptr);
}

// Lock held
bool openLocked()
{
    if (state == State::Open)
        return true;
    if (state == State::Missing)
        return false;
    state = State::Missing;
    if (!store.open(kDictPath))
        return false;
    sd_result r = sd_open(&dict, storeRead, &store, store.size());
    if (r == SD_OK)
        r = sd_verify(&dict);
    if (r != SD_OK) {
        LOG_WARN("StrokeInput: %s refused (%d)", kDictPath, (int)r);
        store.close();
        return false;
    }
    LOG_INFO("StrokeInput: %s, %u characters", kDictPath, (unsigned)dict.entry_count);
    state = State::Open;
    return true;
}

} // namespace

bool available()
{
    concurrency::LockGuard g(&inputLock());
    return openLocked();
}

void close()
{
    concurrency::LockGuard g(&inputLock());
    store.close();
    state = State::Closed;
    candidateTotal = 0;
}

void reset()
{
    concurrency::LockGuard g(&inputLock());
    strokeTotal = 0;
    candidateTotal = 0;
    currentPage = 0;
}

bool addStroke(uint8_t stroke)
{
    concurrency::LockGuard g(&inputLock());
    if (strokeTotal >= SD_MAX_STROKES || stroke < 1 || stroke > kAnyStroke)
        return false;
    strokes[strokeTotal++] = stroke;
    lookUp();
    return true;
}

bool removeStroke()
{
    concurrency::LockGuard g(&inputLock());
    if (strokeTotal == 0)
        return false;
    strokeTotal--;
    lookUp();
    return true;
}

int strokeCount()
{
    concurrency::LockGuard g(&inputLock());
    return strokeTotal;
}

uint8_t strokeAt(int i)
{
    concurrency::LockGuard g(&inputLock());
    return (i >= 0 && i < strokeTotal) ? strokes[i] : 0;
}

namespace
{
// Lock held
int pageCountLocked()
{
    return (candidateTotal + kPageSize - 1) / kPageSize;
}

int pageCandidateCountLocked()
{
    int left = candidateTotal - currentPage * kPageSize;
    return left < 0 ? 0 : (left > kPageSize ? kPageSize : left);
}

uint32_t pageCandidateLocked(int i)
{
    return (i >= 0 && i < pageCandidateCountLocked()) ? candidates[currentPage * kPageSize + i] : 0;
}
} // namespace

int pageCandidateCount()
{
    concurrency::LockGuard g(&inputLock());
    return pageCandidateCountLocked();
}

uint32_t pageCandidate(int i)
{
    concurrency::LockGuard g(&inputLock());
    return pageCandidateLocked(i);
}

int page()
{
    concurrency::LockGuard g(&inputLock());
    return currentPage;
}

int pageCount()
{
    concurrency::LockGuard g(&inputLock());
    return pageCountLocked();
}

void nextPage()
{
    concurrency::LockGuard g(&inputLock());
    if (currentPage + 1 < pageCountLocked())
        currentPage++;
}

void prevPage()
{
    concurrency::LockGuard g(&inputLock());
    if (currentPage > 0)
        currentPage--;
}

uint32_t pick(int i)
{
    concurrency::LockGuard g(&inputLock());
    uint32_t cp = pageCandidateLocked(i);
    if (cp) {
        strokeTotal = 0;
        candidateTotal = 0;
        currentPage = 0;
    }
    return cp;
}

} // namespace StrokeInput
} // namespace graphics

#endif
