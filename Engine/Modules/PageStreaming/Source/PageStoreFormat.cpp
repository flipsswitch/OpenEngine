#include "PageStreaming/PageStoreFormat.h"

#include "PageStoreSerialization.h"

#include <algorithm>

namespace GameEngine::PageStreaming
{
namespace
{

uint32 PagesFor(uint32 samples)
{
    return (samples + kPageOwnedSamples - 1u) / kPageOwnedSamples;
}

bool MortonBefore(const PageIndexEntry& entry, uint64 code)
{
    return entry.Morton < code;
}

bool MortonLess(const PageIndexEntry& a, const PageIndexEntry& b)
{
    return a.Morton < b.Morton;
}

} // namespace

bool PageStoreLayout::Contains(const PageAddress& address) const
{
    if (address.Face >= Header.FaceCount || address.Level >= Levels.size())
        return false;
    const PageStoreLevel& level = Levels[address.Level];
    return address.X < level.PagesX && address.Z < level.PagesZ;
}

std::optional<std::size_t> PageStoreLayout::PresentIndex(const PageAddress& address) const
{
    const std::size_t rangeIndex = static_cast<std::size_t>(address.Face) * Levels.size() + address.Level;
    if (!Contains(address) || rangeIndex >= Ranges.size())
        return std::nullopt;
    const PageStoreRange& range = Ranges[rangeIndex];
    const uint64 code = PageMortonCode(address.X, address.Z);
    const auto first = Entries.begin() + static_cast<std::ptrdiff_t>(range.First);
    const auto last = first + static_cast<std::ptrdiff_t>(range.Count);
    const auto found = std::lower_bound(first, last, code, MortonBefore);
    if (found == last || found->Morton != code)
        return std::nullopt;
    return static_cast<std::size_t>(found - Entries.begin());
}

const PageIndexEntry* PageStoreLayout::FindPresent(const PageAddress& address) const
{
    const std::optional<std::size_t> index = PresentIndex(address);
    return index ? &Entries[*index] : nullptr;
}

std::optional<PageAddress> PageStoreLayout::ResolvePresent(PageAddress address) const
{
    while (Contains(address))
    {
        if (FindPresent(address))
            return address;
        address = address.Parent();
    }
    return std::nullopt;
}

uint32 PageStoreLayout::StoredBytes(const PageIndexEntry& entry) const
{
    return PageStoredBytes(Header.Format, entry.HasWideWords());
}

uint64 PageStoreLayout::DataOffset() const
{
    return kPageStoreHeaderBytes + Levels.size() * kPageStoreLevelBytes + Ranges.size() * kPageStoreRangeBytes +
           Entries.size() * kPageIndexEntryBytes;
}

uint64 PageStoreLayout::TotalBytes() const
{
    uint64 end = DataOffset();
    for (const PageIndexEntry& entry : Entries)
        if (entry.Offset != 0)
            end = std::max(end, entry.Offset + StoredBytes(entry));
    return end;
}

std::vector<PageStoreLevel> BuildPageStoreLevels(uint32 samplesX, uint32 samplesZ)
{
    std::vector<PageStoreLevel> levels;
    if (samplesX == 0 || samplesZ == 0)
        return levels;
    uint64 firstEntry = 0;
    while (levels.size() < kPageStoreMaxLevels)
    {
        PageStoreLevel level;
        level.SamplesX = samplesX;
        level.SamplesZ = samplesZ;
        level.PagesX = PagesFor(samplesX);
        level.PagesZ = PagesFor(samplesZ);
        level.FirstEntry = firstEntry;
        firstEntry += static_cast<uint64>(level.PagesX) * level.PagesZ;
        levels.push_back(level);
        if (level.PagesX == 1 && level.PagesZ == 1)
            break;
        samplesX = samplesX / 2u + 1u;
        samplesZ = samplesZ / 2u + 1u;
    }
    return levels;
}

uint64 DensePageCount(std::span<const PageStoreLevel> levels)
{
    if (levels.empty())
        return 0;
    return levels.back().FirstEntry + static_cast<uint64>(levels.back().PagesX) * levels.back().PagesZ;
}

uint32 PageStrideTexels(PageFieldFormat format)
{
    return format == PageFieldFormat::Bc5 ? kPageOwnedSamples + 2u * kPageBlockTexels : kPageStrideSamples;
}

uint32 PageTexelBytes(PageFieldFormat format)
{
    switch (format)
    {
    case PageFieldFormat::Rg8:
    case PageFieldFormat::Bc5:
        return 2u;
    case PageFieldFormat::HeightSteps:
    case PageFieldFormat::Rgba8:
    case PageFieldFormat::Rgba8Srgb:
        return 4u;
    }
    return 0u;
}

uint32 PageStoredBytes(PageFieldFormat format, bool wide)
{
    const uint32 stride = PageStrideTexels(format);
    switch (format)
    {
    case PageFieldFormat::HeightSteps:
        return stride * stride * (wide ? 4u : 2u);
    case PageFieldFormat::Bc5:
        return (stride / kPageBlockTexels) * (stride / kPageBlockTexels) * kBc5BlockBytes;
    case PageFieldFormat::Rgba8:
    case PageFieldFormat::Rgba8Srgb:
    case PageFieldFormat::Rg8:
        return stride * stride * PageTexelBytes(format);
    }
    return 0u;
}

PageStoreLayout MakePageStoreLayout(const PageStoreHeader& header, std::span<const uint8> present)
{
    PageStoreLayout layout;
    layout.Header = header;
    layout.Levels = BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
    // Steps past the store's levels are not on disk: zero, so a header compares equal to its read.
    std::fill(layout.Header.StepPerLevel.begin() + static_cast<std::ptrdiff_t>(layout.Levels.size()),
              layout.Header.StepPerLevel.end(), 0.0f);
    if (header.Format != PageFieldFormat::HeightSteps)
    {
        // A store that holds no heights has no height units, reference or steps.
        layout.Header.Units = PageHeightUnits{};
        layout.Header.ReferenceRadius = 0.0;
        layout.Header.StepPerLevel.fill(0.0f);
    }
    const uint64 densePerFace = DensePageCount(layout.Levels);
    layout.Ranges.resize(layout.Levels.size() * header.FaceCount);
    if (!present.empty() && present.size() != densePerFace * header.FaceCount)
        return layout; // every page absent: the caller's presence does not fit the header

    for (uint32 face = 0; face < header.FaceCount; ++face)
    {
        for (std::size_t levelIndex = 0; levelIndex < layout.Levels.size(); ++levelIndex)
        {
            const PageStoreLevel& level = layout.Levels[levelIndex];
            const uint64 dense = face * densePerFace + level.FirstEntry;
            PageStoreRange& range = layout.Ranges[face * layout.Levels.size() + levelIndex];
            range.First = layout.Entries.size();
            for (uint32 z = 0; z < level.PagesZ; ++z)
            {
                for (uint32 x = 0; x < level.PagesX; ++x)
                {
                    const uint64 page = dense + static_cast<uint64>(z) * level.PagesX + x;
                    if (present.empty() || present[static_cast<std::size_t>(page)] != 0)
                        layout.Entries.push_back(PageIndexEntry{PageMortonCode(x, z)});
                }
            }
            std::sort(layout.Entries.begin() + static_cast<std::ptrdiff_t>(range.First), layout.Entries.end(),
                      MortonLess);
            range.Count = layout.Entries.size() - range.First;
        }
    }
    return layout;
}

} // namespace GameEngine::PageStreaming
