#include "PageStreaming/PageStoreReader.h"

#include "PageCodec.h"
#include "PageStoreSerialization.h"

#include "AssetCore/SharedFileRead.h"

namespace GameEngine::PageStreaming
{

PageStoreReader::PageStoreReader() = default;
PageStoreReader::~PageStoreReader() = default;

std::string PageStoreReader::Open(const std::filesystem::path& file, PageFieldKind kind, uint64 baseOffset,
                                  uint64 storeBytes)
{
    m_IsOpen = false;
    m_Layout = {};
    auto reader = std::make_unique<SharedFileReader>();
    if (!reader->Open(file))
        return "the page store " + file.string() + " could not be opened";
    const int64 fileBytes = reader->Size();
    if (fileBytes < 0 || baseOffset > static_cast<uint64>(fileBytes))
        return "the page store " + file.string() + " is shorter than its offset";
    uint64 available = static_cast<uint64>(fileBytes) - baseOffset;
    if (storeBytes != 0)
    {
        if (storeBytes > available)
            return "the page store in " + file.string() + " is truncated";
        available = storeBytes;
    }
    PageStoreLayout layout;
    if (std::string reason = ReadPageStoreHead(*reader, kind, baseOffset, available, layout); !reason.empty())
        return reason + " (" + file.string() + ")";

    m_File = file;
    m_BaseOffset = baseOffset;
    m_Codec = MakePageCodec(layout.Header);
    m_Layout = std::move(layout);
    m_Reader = std::move(reader);
    m_IsOpen = true;
    return {};
}

bool PageStoreReader::ReadPageBytes(const PageAddress& address, std::vector<uint8>& outBytes) const
{
    const PageIndexEntry* entry = m_IsOpen ? m_Layout.FindPresent(address) : nullptr;
    if (!entry)
        return false;
    outBytes.resize(m_Layout.StoredBytes(*entry));
    std::lock_guard lock(m_ReadMutex);
    return m_Reader->SeekTo(m_BaseOffset + entry->Offset) &&
           m_Reader->Read(outBytes.data(), outBytes.size()) == static_cast<int64>(outBytes.size());
}

bool PageStoreReader::ReadPageSamples(const PageAddress& address, std::span<uint8> outSamples) const
{
    const uint32 stride = PageStrideTexels(m_Layout.Header.Format);
    std::vector<uint8> bytes;
    if (outSamples.size() != static_cast<std::size_t>(stride) * stride * PageTexelBytes(m_Layout.Header.Format) ||
        !ReadPageBytes(address, bytes))
        return false;
    m_Codec->Decode(address.Level, *m_Layout.FindPresent(address), bytes, outSamples);
    return true;
}

bool PageStoreReader::ReadHeightPage(const PageAddress& address, std::span<float32> outSamples) const
{
    return m_IsOpen && m_Layout.Header.Format == PageFieldFormat::HeightSteps &&
           ReadPageSamples(address, {reinterpret_cast<uint8*>(outSamples.data()), outSamples.size_bytes()});
}

} // namespace GameEngine::PageStreaming
