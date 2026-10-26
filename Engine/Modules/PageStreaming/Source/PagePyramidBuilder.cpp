#include "PagePyramidBuilder.h"

#include "HeightPageCodec.h"
#include "PageCodec.h"
#include "PageStreaming/PageStoreWriter.h"
#include "PageStoreSerialization.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

namespace GameEngine::PageStreaming
{
namespace
{

// Pages of a row encoded at once before they are written: enough work for the pool, a bounded
// buffer (64 pages of at most 67.6 KB).
constexpr uint32 kPagesEncodedPerChunk = 64u;

// The 3 x 3 footprint of a coarser sample: the finer samples 2i - 1, 2i and 2i + 1 along both axes,
// edge-clamped, row-major, with the [1 2 1] x [1 2 1] tent's weights, which sum to 16.
constexpr uint32 kTaps = 9u;
constexpr std::array<uint32, kTaps> kTapWeights{1u, 2u, 1u, 2u, 4u, 2u, 1u, 2u, 1u};
constexpr uint32 kTapWeightSum = 16u;
constexpr float32 kByteMax = 255.0f;
// A splat texel's four weights sum to this.
constexpr uint32 kWeightChannels = 4u;
constexpr uint32 kWeightSum = 255u;

using FinerRows = std::array<const uint8*, 3>;
using Taps = std::array<const uint8*, kTaps>;

uint32 ClampIndex(int64 index, uint32 count)
{
    return static_cast<uint32>(std::clamp<int64>(index, 0, static_cast<int64>(count) - 1));
}

const float32* Heights(const uint8* row)
{
    return reinterpret_cast<const float32*>(row);
}

// The tent across a row at coarser sample `i`: [1 2 1] / 4 over the finer samples 2i - 1, 2i, 2i + 1.
float32 TentAcross(const float32* row, uint32 i, uint32 width)
{
    const int64 center = 2 * static_cast<int64>(i);
    return 0.25f * row[ClampIndex(center - 1, width)] + 0.5f * row[ClampIndex(center, width)] +
           0.25f * row[ClampIndex(center + 1, width)];
}

void HeightTentRow(const FinerRows& rows, uint32 finerWidth, float32* out, uint32 width)
{
    for (uint32 i = 0; i < width; ++i)
        out[i] = 0.25f * TentAcross(Heights(rows[0]), i, finerWidth) +
                 0.5f * TentAcross(Heights(rows[1]), i, finerWidth) +
                 0.25f * TentAcross(Heights(rows[2]), i, finerWidth);
}

Taps GatherTaps(const FinerRows& rows, uint32 i, uint32 finerWidth, uint32 texelBytes)
{
    const int64 center = 2 * static_cast<int64>(i);
    Taps taps{};
    for (uint32 r = 0; r < 3u; ++r)
        for (uint32 c = 0; c < 3u; ++c)
            taps[r * 3u + c] = rows[r] + static_cast<std::size_t>(ClampIndex(center - 1 + c, finerWidth)) * texelBytes;
    return taps;
}

uint8 ToByte(float32 unit)
{
    return static_cast<uint8>(std::lround(Math::Clamp01(unit) * kByteMax));
}

// The tent per channel, rounded to nearest.
void TentTexel(const Taps& taps, uint32 channels, uint8* out)
{
    for (uint32 channel = 0; channel < channels; ++channel)
    {
        uint32 sum = 0;
        for (uint32 t = 0; t < kTaps; ++t)
            sum += kTapWeights[t] * taps[t][channel];
        out[channel] = static_cast<uint8>((sum + kTapWeightSum / 2u) / kTapWeightSum);
    }
}

// The tent per weight, renormalized to sum to 255: each weight takes the floor of its share and the
// remainder goes to the largest (the lowest channel on a tie). A footprint with no weight stays zero.
void WeightsTentTexel(const Taps& taps, uint8* out)
{
    std::array<uint32, kWeightChannels> sums{};
    uint32 total = 0;
    for (uint32 channel = 0; channel < kWeightChannels; ++channel)
    {
        for (uint32 t = 0; t < kTaps; ++t)
            sums[channel] += kTapWeights[t] * taps[t][channel];
        total += sums[channel];
    }
    if (total == 0)
    {
        std::fill(out, out + kWeightChannels, uint8{0});
        return;
    }
    uint32 assigned = 0;
    uint32 largest = 0;
    for (uint32 channel = 0; channel < kWeightChannels; ++channel)
    {
        out[channel] = static_cast<uint8>(static_cast<uint64>(sums[channel]) * kWeightSum / total);
        assigned += out[channel];
        if (sums[channel] > sums[largest])
            largest = channel;
    }
    out[largest] = static_cast<uint8>(out[largest] + (kWeightSum - assigned));
}

// A unit normal stored as (x, z) in unsigned normalized bytes; y is the non-negative rest.
Mathematics::Vector3 DecodeNormal(const uint8* texel)
{
    const float32 x = static_cast<float32>(texel[0]) / kByteMax * 2.0f - 1.0f;
    const float32 z = static_cast<float32>(texel[1]) / kByteMax * 2.0f - 1.0f;
    return {x, std::sqrt(std::max(0.0f, 1.0f - x * x - z * z)), z};
}

// The tent on the unit normals, renormalized; a footprint whose normals cancel faces up.
void NormalTentTexel(const Taps& taps, uint8* out)
{
    Mathematics::Vector3 sum;
    for (uint32 t = 0; t < kTaps; ++t)
        sum = sum + DecodeNormal(taps[t]) * static_cast<float32>(kTapWeights[t]);
    Mathematics::Vector3 normal = sum.NormalizeOrZero();
    if (normal.LengthSquared() == 0.0f)
        normal = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
    out[0] = ToByte(normal.x * 0.5f + 0.5f);
    out[1] = ToByte(normal.z * 0.5f + 0.5f);
}

// The tent in linear light on premultiplied color: rgb decoded from sRGB, a linear, rgb encoded back.
void PremultipliedTentTexel(const Taps& taps, uint8* out)
{
    ColorLinear sum(0.0f, 0.0f, 0.0f, 0.0f);
    for (uint32 t = 0; t < kTaps; ++t)
    {
        const ColorLinear tap = ColorSRGB::FromSRGB255(taps[t][0], taps[t][1], taps[t][2], taps[t][3]).ToLinear();
        const float32 weight = static_cast<float32>(kTapWeights[t]) / static_cast<float32>(kTapWeightSum);
        sum.r += weight * tap.r;
        sum.g += weight * tap.g;
        sum.b += weight * tap.b;
        sum.a += weight * tap.a;
    }
    const ColorSRGB encoded = sum.ToSRGB();
    out[0] = ToByte(encoded.r);
    out[1] = ToByte(encoded.g);
    out[2] = ToByte(encoded.b);
    out[3] = ToByte(encoded.a);
}

} // namespace

PagePyramidBuilder::PagePyramidBuilder(PageStoreWriter& writer, bool patching, JobSystem::WorkStealingThreadPool* pool)
    : m_Writer(writer)
    , m_Patching(patching)
    , m_Pool(pool)
{
    const PageStoreHeader& header = writer.Layout().Header;
    m_Codec = MakePageCodec(header);
    m_Format = header.Format;
    m_Filter = header.Filter;
    m_TexelBytes = PageTexelBytes(m_Format);
    m_Stride = PageStrideTexels(m_Format);
    m_Apron = (m_Stride - kPageOwnedSamples) / 2u;
    // A format and filter that no kind uses have no samples to make: the build fails at Finish.
    if (!m_Codec || !PageEncodingKind(m_Format, m_Filter))
        m_Failed.store(true);
    for (const PageStoreLevel& shape : writer.Layout().Levels)
    {
        LevelState state;
        state.Shape = shape;
        if (IsHeight())
        {
            const std::size_t pages = static_cast<std::size_t>(shape.PagesX) * shape.PagesZ;
            state.PageMin.assign(pages, std::numeric_limits<float32>::max());
            state.PageMax.assign(pages, std::numeric_limits<float32>::lowest());
        }
        m_Levels.push_back(std::move(state));
    }
}

PagePyramidBuilder::~PagePyramidBuilder() = default;

void PagePyramidBuilder::PushLevel0Rows(const uint8* rows, uint32 rowCount)
{
    if (m_Failed.load())
        return;
    LevelState& level0 = m_Levels.front();
    const std::size_t rowBytes = static_cast<std::size_t>(level0.Shape.SamplesX) * m_TexelBytes;
    assert(level0.RowsReceived + rowCount <= level0.Shape.SamplesZ);
    const std::size_t firstNew = level0.Rows.size();
    for (uint32 r = 0; r < rowCount; ++r)
        level0.Rows.emplace_back(rows + r * rowBytes, rows + (r + 1u) * rowBytes);
    if (IsHeight())
        RunParallel(rowCount, [&](uint32 r) {
            Quantize(0, reinterpret_cast<float32*>(level0.Rows[firstNew + r].data()), level0.Shape.SamplesX);
        });
    level0.RowsReceived += rowCount;
    Advance(0);
}

bool PagePyramidBuilder::Finish() const
{
    if (m_Failed.load())
        return false;
    for (const LevelState& level : m_Levels)
        if (level.RowsReceived != level.Shape.SamplesZ || level.NextPageRow != level.Shape.PagesZ)
            return false;
    return true;
}

bool PagePyramidBuilder::IsHeight() const
{
    return m_Format == PageFieldFormat::HeightSteps;
}

const uint8* PagePyramidBuilder::Row(const LevelState& level, int64 row) const
{
    const uint32 clamped = ClampIndex(row, level.Shape.SamplesZ);
    assert(clamped >= level.FirstKeptRow && clamped < level.RowsReceived);
    return level.Rows[clamped - level.FirstKeptRow].data();
}

void PagePyramidBuilder::Advance(uint32 levelIndex)
{
    LevelState& level = m_Levels[levelIndex];
    const uint32 lastRow = level.Shape.SamplesZ - 1u;
    // A page row is complete once its last row (the far apron, or the field's last row) is in.
    while (level.NextPageRow < level.Shape.PagesZ)
    {
        const uint32 needed =
            std::min(level.NextPageRow * kPageOwnedSamples + kPageOwnedSamples - 1u + m_Apron, lastRow);
        if (level.RowsReceived <= needed)
            break;
        EmitPageRow(levelIndex, level.NextPageRow++);
    }
    if (levelIndex + 1u < m_Levels.size())
    {
        // A coarser row j reads rows 2j - 1 to 2j + 1 of this level, clamped to the field.
        const uint32 coarserRows = m_Levels[levelIndex + 1u].Shape.SamplesZ;
        uint32 end = level.NextChildRow;
        while (end < coarserRows && std::min(2u * end + 1u, lastRow) < level.RowsReceived)
            ++end;
        if (end > level.NextChildRow)
        {
            const uint32 first = level.NextChildRow;
            MakeCoarserRows(levelIndex, first, end - first);
            level.NextChildRow = end;
            Advance(levelIndex + 1u);
        }
    }
    Discard(levelIndex);
}

void PagePyramidBuilder::EmitPageRow(uint32 levelIndex, uint32 pageRow)
{
    // A chunk of the row is encoded in parallel, then written in X order: the writer places pages
    // in the order they arrive, so the store's bytes do not depend on the workers.
    const uint32 pagesX = m_Levels[levelIndex].Shape.PagesX;
    std::vector<EncodedPage> encoded(std::min(pagesX, kPagesEncodedPerChunk));
    std::vector<uint8> changed(encoded.size());
    for (uint32 first = 0; first < pagesX; first += kPagesEncodedPerChunk)
    {
        const uint32 count = std::min(kPagesEncodedPerChunk, pagesX - first);
        RunParallel(count, [&](uint32 i) {
            changed[i] = EncodePage(levelIndex, first + i, pageRow, encoded[i]) ? 1u : 0u;
        });
        for (uint32 i = 0; i < count; ++i)
        {
            if (changed[i] == 0)
            {
                m_PagesUnchanged.fetch_add(1);
                continue;
            }
            if (!m_Writer.WritePage(PageAddress{0, static_cast<uint8>(levelIndex), first + i, pageRow}, encoded[i]))
            {
                m_Failed.store(true);
                return;
            }
            m_PagesWritten.fetch_add(1);
        }
    }
}

bool PagePyramidBuilder::EncodePage(uint32 levelIndex, uint32 pageX, uint32 pageZ, EncodedPage& encoded)
{
    const LevelState& level = m_Levels[levelIndex];
    const uint32 width = level.Shape.SamplesX;
    std::vector<uint8> samples(static_cast<std::size_t>(m_Stride) * m_Stride * m_TexelBytes);
    const int64 firstX = static_cast<int64>(pageX) * kPageOwnedSamples - m_Apron;
    const int64 firstZ = static_cast<int64>(pageZ) * kPageOwnedSamples - m_Apron;
    for (uint32 r = 0; r < m_Stride; ++r)
    {
        const uint8* source = Row(level, firstZ + r);
        uint8* out = samples.data() + static_cast<std::size_t>(r) * m_Stride * m_TexelBytes;
        for (uint32 c = 0; c < m_Stride; ++c)
            std::memcpy(out + static_cast<std::size_t>(c) * m_TexelBytes,
                        source + static_cast<std::size_t>(ClampIndex(firstX + c, width)) * m_TexelBytes, m_TexelBytes);
    }

    PageFootprint footprint;
    if (IsHeight())
        footprint = BoundHeightPage(levelIndex, pageX, pageZ, samples);
    m_Codec->Encode(levelIndex, samples, footprint, encoded);
    const PageAddress address{0, static_cast<uint8>(levelIndex), pageX, pageZ};
    const PageIndexEntry* current = m_Writer.Layout().FindPresent(address);
    return !(m_Patching && current && current->Hash == encoded.Hash && current->MinWord == encoded.MinWord &&
             current->MaxWord == encoded.MaxWord && current->Flags == encoded.Flags);
}

PageFootprint PagePyramidBuilder::BoundHeightPage(uint32 levelIndex, uint32 pageX, uint32 pageZ,
                                                  std::span<const uint8> samples)
{
    LevelState& level = m_Levels[levelIndex];
    const float32* heights = Heights(samples.data());
    // The footprint: the owned samples and the shared edge after them (stride indices 1 to 129).
    float32 lowest = std::numeric_limits<float32>::max();
    float32 highest = std::numeric_limits<float32>::lowest();
    for (uint32 r = kPageApronSamples; r < kPageStrideSamples; ++r)
    {
        for (uint32 c = kPageApronSamples; c < kPageStrideSamples; ++c)
        {
            const float32 value = heights[static_cast<std::size_t>(r) * kPageStrideSamples + c];
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
        }
    }
    if (levelIndex > 0)
    {
        // The level-0 truth under the footprint: the finer pages it covers (clamped to the finer
        // level, whose last page also holds the samples past the field's edge).
        const LevelState& finer = m_Levels[levelIndex - 1u];
        for (uint32 dz = 0; dz < 2u; ++dz)
        {
            for (uint32 dx = 0; dx < 2u; ++dx)
            {
                const uint32 childX = std::min(2u * pageX + dx, finer.Shape.PagesX - 1u);
                const uint32 childZ = std::min(2u * pageZ + dz, finer.Shape.PagesZ - 1u);
                const std::size_t child = static_cast<std::size_t>(childZ) * finer.Shape.PagesX + childX;
                lowest = std::min(lowest, finer.PageMin[child]);
                highest = std::max(highest, finer.PageMax[child]);
            }
        }
    }
    const std::size_t pageIndex = static_cast<std::size_t>(pageZ) * level.Shape.PagesX + pageX;
    level.PageMin[pageIndex] = lowest;
    level.PageMax[pageIndex] = highest;
    return {lowest, highest};
}

void PagePyramidBuilder::MakeCoarserRows(uint32 levelIndex, uint32 firstRow, uint32 rowCount)
{
    LevelState& coarser = m_Levels[levelIndex + 1u];
    std::vector<std::vector<uint8>> rows(rowCount);
    RunParallel(rowCount, [&](uint32 r) { MakeCoarserRow(levelIndex, firstRow + r, rows[r]); });
    for (std::vector<uint8>& row : rows)
        coarser.Rows.push_back(std::move(row));
    coarser.RowsReceived += rowCount;
}

void PagePyramidBuilder::MakeCoarserRow(uint32 levelIndex, uint32 row, std::vector<uint8>& out) const
{
    const LevelState& finer = m_Levels[levelIndex];
    const uint32 finerWidth = finer.Shape.SamplesX;
    const uint32 width = m_Levels[levelIndex + 1u].Shape.SamplesX;
    const int64 center = 2 * static_cast<int64>(row);
    const FinerRows rows{Row(finer, center - 1), Row(finer, center), Row(finer, center + 1)};
    out.resize(static_cast<std::size_t>(width) * m_TexelBytes);
    if (m_Filter == PageFieldFilter::HeightTent)
    {
        float32* heights = reinterpret_cast<float32*>(out.data());
        HeightTentRow(rows, finerWidth, heights, width);
        Quantize(levelIndex + 1u, heights, width);
        return;
    }
    for (uint32 i = 0; i < width; ++i)
    {
        const Taps taps = GatherTaps(rows, i, finerWidth, m_TexelBytes);
        uint8* texel = out.data() + static_cast<std::size_t>(i) * m_TexelBytes;
        switch (m_Filter)
        {
        case PageFieldFilter::WeightsTent:
            WeightsTentTexel(taps, texel);
            break;
        case PageFieldFilter::NormalTent:
            NormalTentTexel(taps, texel);
            break;
        case PageFieldFilter::Tent:
            TentTexel(taps, m_TexelBytes, texel);
            break;
        case PageFieldFilter::PremultipliedTent:
            PremultipliedTentTexel(taps, texel);
            break;
        case PageFieldFilter::HeightTent:
            break;
        }
    }
}

void PagePyramidBuilder::Quantize(uint32 levelIndex, float32* values, std::size_t count) const
{
    const float32 step = m_Writer.Layout().Header.StepPerLevel[levelIndex];
    for (std::size_t i = 0; i < count; ++i)
        values[i] = QuantizeHeight(values[i], step);
}

void PagePyramidBuilder::Discard(uint32 levelIndex)
{
    LevelState& level = m_Levels[levelIndex];
    const int64 rows = level.Shape.SamplesZ;
    int64 keepFrom = level.NextPageRow < level.Shape.PagesZ
                         ? static_cast<int64>(level.NextPageRow) * kPageOwnedSamples - m_Apron
                         : rows;
    if (levelIndex + 1u < m_Levels.size() && level.NextChildRow < m_Levels[levelIndex + 1u].Shape.SamplesZ)
        keepFrom = std::min<int64>(keepFrom, 2 * static_cast<int64>(level.NextChildRow) - 1);
    // A clamped read of a row past the field's end reads the last row, so it stays until the end.
    keepFrom = std::clamp<int64>(keepFrom, 0, std::min<int64>(level.RowsReceived, rows - 1));
    if (level.NextPageRow == level.Shape.PagesZ &&
        (levelIndex + 1u == m_Levels.size() || level.NextChildRow == m_Levels[levelIndex + 1u].Shape.SamplesZ))
        keepFrom = level.RowsReceived;
    while (level.FirstKeptRow < keepFrom && !level.Rows.empty())
    {
        level.Rows.pop_front();
        ++level.FirstKeptRow;
    }
}

void PagePyramidBuilder::RunParallel(uint32 count, const std::function<void(uint32)>& body) const
{
    JobSystem::ParallelFor(m_Pool, count, [&body](size_t i) { body(static_cast<uint32>(i)); });
}

} // namespace GameEngine::PageStreaming
