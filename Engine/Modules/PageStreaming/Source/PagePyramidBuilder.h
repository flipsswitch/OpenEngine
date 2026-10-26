#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::PageStreaming
{

class PageCodec;
class PageStoreWriter;
struct EncodedPage;
struct PageFootprint;

/// Builds a store's pyramid from its level-0 rows, pushed in order, with memory bounded by a few
/// page rows per level, for any of the store's formats: the store's codec encodes each page and its
/// filter makes each coarser level. Each level keeps only the rows its next page row and the next
/// level's next rows still read: a page row is written as soon as its rows and apron are in, and a
/// coarser row is made with the filter as soon as its three source rows are. A coarser level is the
/// filter of the samples of the level below, so a recook of unchanged input reproduces every page.
/// On a Bc5 store those samples are the rows before their BC5 encode, not what the level below
/// stores: a coarser level is refiltered from the baker's rows, never from decoded pages.
/// A height store quantizes every value to its level's step before anything reads it, so its
/// coarser level is the filter of what the level below stores, and bounds each page by the level-0
/// values under its footprint.
class PagePyramidBuilder
{
public:
    /// Writes into `writer`, whose layout is the store's. When `patching`, a page whose encoded
    /// bytes hash to the entry the writer already holds is not written.
    PagePyramidBuilder(PageStoreWriter& writer, bool patching, JobSystem::WorkStealingThreadPool* pool);
    ~PagePyramidBuilder();

    /// Appends the next `rowCount` level-0 rows: SamplesX samples of PageTexelBytes(format) bytes
    /// each (a height store's samples are float32 values in the source's units).
    void PushLevel0Rows(const uint8* rows, uint32 rowCount);

    /// True when every row was pushed and every page written.
    bool Finish() const;

    uint64 PagesWritten() const { return m_PagesWritten.load(); }
    uint64 PagesUnchanged() const { return m_PagesUnchanged.load(); }

private:
    struct LevelState
    {
        PageStoreLevel Shape;
        uint32 RowsReceived = 0;
        uint32 FirstKeptRow = 0;
        std::deque<std::vector<uint8>> Rows; // rows [FirstKeptRow, RowsReceived)
        uint32 NextPageRow = 0;
        uint32 NextChildRow = 0; // rows of the next coarser level made so far
        std::vector<float32> PageMin; // a height store's, per page: level-0 truth and the page's own footprint
        std::vector<float32> PageMax;
    };

    bool IsHeight() const;
    const uint8* Row(const LevelState& level, int64 row) const;
    void Advance(uint32 level);
    void EmitPageRow(uint32 level, uint32 pageRow);
    /// Encodes a page; false when a patch already holds exactly these bytes.
    bool EncodePage(uint32 level, uint32 pageX, uint32 pageZ, EncodedPage& encoded);
    /// The bounds of a height page and the level-0 values under its footprint, kept for its parent.
    PageFootprint BoundHeightPage(uint32 level, uint32 pageX, uint32 pageZ, std::span<const uint8> samples);
    void MakeCoarserRows(uint32 level, uint32 firstRow, uint32 rowCount);
    void MakeCoarserRow(uint32 level, uint32 row, std::vector<uint8>& out) const;
    void Quantize(uint32 level, float32* values, std::size_t count) const;
    void Discard(uint32 level);
    void RunParallel(uint32 count, const std::function<void(uint32)>& body) const;

    PageStoreWriter& m_Writer;
    bool m_Patching = false;
    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    std::unique_ptr<PageCodec> m_Codec;
    PageFieldFormat m_Format = PageFieldFormat::HeightSteps;
    PageFieldFilter m_Filter = PageFieldFilter::HeightTent;
    uint32 m_TexelBytes = 0;
    uint32 m_Stride = 0; // samples a page stores per axis
    uint32 m_Apron = 0;  // apron samples on each side
    std::vector<LevelState> m_Levels;
    std::atomic<uint64> m_PagesWritten{0};
    std::atomic<uint64> m_PagesUnchanged{0};
    std::atomic<bool> m_Failed{false};
};

} // namespace GameEngine::PageStreaming
