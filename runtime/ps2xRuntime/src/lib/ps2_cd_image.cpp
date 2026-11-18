#include "runtime/ps2_cd_image.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
    constexpr std::string_view kLayoutName = "disc-layout.txt";
    constexpr std::string_view kLayoutVersion = "PS2X-DISC-FILES-1";

    uint32_t le32(const uint8_t *p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    bool seekFile(std::FILE *file, uint64_t offset)
    {
#if defined(_WIN32)
        return offset <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) &&
               _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
        return offset <= static_cast<uint64_t>(std::numeric_limits<off_t>::max()) &&
               fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
    }

    std::FILE *openFile(const std::filesystem::path &path)
    {
#if defined(_WIN32)
        return ::_wfopen(path.c_str(), L"rb");
#else
        return std::fopen(path.c_str(), "rb");
#endif
    }

    bool safeRelative(const std::filesystem::path &path)
    {
        if (path.empty() || path.has_root_path())
            return false;
        for (const auto &part : path)
            if (part == ".." || part == "." || part.empty() || part.string().find(':') != std::string::npos)
                return false;
        return true;
    }

    bool inside(const std::filesystem::path &root, const std::filesystem::path &file)
    {
        const auto relative = file.lexically_relative(root);
        return safeRelative(relative);
    }
}

struct PS2CdImage::InstalledDisc
{
    struct Extent
    {
        uint64_t offset;
        uint64_t size;
        std::filesystem::path path;
        mutable std::unique_ptr<std::FILE, decltype(&std::fclose)> file{nullptr, std::fclose};
    };
    std::vector<Extent> extents;
};

PS2CdImage::PS2CdImage() = default;
PS2CdImage::~PS2CdImage() { close(); }

PS2CdImage &PS2CdImage::instance()
{
    static PS2CdImage image;
    return image;
}

std::string PS2CdImage::normalizeKey(std::string_view ps2Path)
{
    std::string_view path = ps2Path;
    const size_t colon = path.find(':');
    if (colon != std::string_view::npos && colon < 8)
    {
        path.remove_prefix(colon + 1);
    }
    const size_t version = path.rfind(';');
    if (version != std::string_view::npos)
    {
        path = path.substr(0, version);
    }
    std::string key;
    key.reserve(path.size() + 1);
    for (char c : path)
    {
        if (c == '\\')
        {
            c = '/';
        }
        if (c == '/' && (key.empty() || key.back() == '/'))
        {
            continue;
        }
        key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    while (!key.empty() && (key.back() == '/' || key.back() == '.'))
    {
        key.pop_back();
    }
    return key;
}

bool PS2CdImage::open(const std::filesystem::path &imagePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    resetUnlocked();
    m_openAttempted = true;
    std::error_code error;
    if (std::filesystem::is_directory(imagePath, error))
        return openInstalled(imagePath);

    m_file = openFile(imagePath);
    if (!m_file)
    {
        std::cerr << "[CDIMAGE] cannot open " << imagePath.string() << std::endl;
        return false;
    }
    m_size = std::filesystem::file_size(imagePath, error);
    m_path = imagePath;

    std::vector<uint8_t> sector(kSectorSize);
    uint32_t lba = 16;
    for (;; ++lba)
    {
        if (!readUnlocked(static_cast<uint64_t>(lba) * kSectorSize, sector.data(), kSectorSize) ||
            std::memcmp(sector.data() + 1, "CD001", 5) != 0 || sector[0] == 255)
        {
            std::cerr << "[CDIMAGE] " << imagePath.string() << " is not an ISO9660 image" << std::endl;
            std::fclose(m_file);
            m_file = nullptr;
            return false;
        }
        if (sector[0] == 1)
        {
            break;
        }
    }
    const uint8_t *root = sector.data() + 156;
    m_rootEntry = {le32(root + 2), le32(root + 10), true};
    if (!parseDirectory(m_rootEntry.lba, static_cast<uint32_t>(m_rootEntry.size), std::string(), 0))
    {
        std::fclose(m_file);
        m_file = nullptr;
        m_entries.clear();
        return false;
    }
    std::cout << "[CDIMAGE] mounted " << imagePath.string() << " (" << m_entries.size() << " entries)" << std::endl;
    return true;
}

bool PS2CdImage::parseDirectory(uint32_t lba, uint32_t size, const std::string &prefix, int depth)
{
    if (depth > 16 || size > (64u << 20))
    {
        return false;
    }
    std::vector<uint8_t> data(size);
    if (!readUnlocked(static_cast<uint64_t>(lba) * kSectorSize, data.data(), size))
    {
        return false;
    }
    size_t offset = 0;
    while (offset < data.size())
    {
        const uint8_t length = data[offset];
        if (length == 0)
        {
            offset = (offset / kSectorSize + 1) * kSectorSize;
            continue;
        }
        if (offset + length > data.size() || length < 34)
        {
            return false;
        }
        const uint8_t *record = data.data() + offset;
        const uint32_t entryLba = le32(record + 2);
        const uint32_t entrySize = le32(record + 10);
        const bool directory = (record[25] & 2u) != 0u;
        const uint8_t nameLength = record[32];
        if (33u + nameLength > length)
            return false;
        offset += length;
        if (nameLength == 1 && (record[33] == 0 || record[33] == 1))
        {
            continue;
        }
        const std::string name(reinterpret_cast<const char *>(record + 33), nameLength);
        const std::string key = normalizeKey(prefix + "/" + name);
        m_entries[key] = Entry{entryLba, entrySize, directory};
        if (directory && !parseDirectory(entryLba, entrySize, key, depth + 1))
        {
            return false;
        }
    }
    return true;
}

bool PS2CdImage::openInstalled(const std::filesystem::path &directory)
{
    std::error_code ec;
    const auto root = std::filesystem::canonical(directory, ec);
    if (ec)
        return false;
    std::ifstream layout(root / kLayoutName);
    std::string version;
    uint64_t size = 0;
    if (!(layout >> version >> size) || version != kLayoutVersion || size == 0 || size % kSectorSize != 0)
        return false;

    auto installed = std::make_unique<InstalledDisc>();
    std::unordered_map<std::string, Entry> entries;
    char kind;
    while (layout >> kind)
    {
        uint32_t lba;
        uint64_t length;
        std::string key, relative;
        if (!(layout >> lba >> length >> std::quoted(key) >> std::quoted(relative)))
            return false;
        const uint64_t begin = uint64_t(lba) * kSectorSize;
        if (begin > size || length > size - begin || (kind != 'F' && kind != 'D' && kind != 'M'))
            return false;
        const std::filesystem::path local(relative);
        if (!safeRelative(local) || (kind != 'M' && key != normalizeKey(key)) ||
            (kind == 'F' && key != local.generic_string()))
            return false;
        const auto file = std::filesystem::canonical(root / local, ec);
        if (ec || !inside(root, file) || !std::filesystem::is_regular_file(file, ec) || ec ||
            std::filesystem::file_size(file, ec) != length || ec)
            return false;
        if (kind != 'M' && !entries.emplace(key, Entry{lba, length, kind == 'D'}).second)
            return false;
        if (length)
            installed->extents.push_back({begin, length, file});
    }
    if (!layout.eof() || installed->extents.empty())
        return false;
    auto &extents = installed->extents;
    std::sort(extents.begin(), extents.end(), [](const auto &a, const auto &b) { return a.offset < b.offset; });
    for (size_t i = 1; i < extents.size(); ++i)
        if (extents[i - 1].offset + extents[i - 1].size > extents[i].offset)
            return false;
    m_installed = std::move(installed);
    m_entries = std::move(entries);
    m_size = size;
    m_path = root;
    std::cout << "[CDDATA] mounted local files from " << root.string() << " (" << m_entries.size() << " entries)\n";
    return true;
}

bool PS2CdImage::install(const std::filesystem::path &directory, std::string &error) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file)
    {
        error = "No ISO is mounted for installation";
        return false;
    }
    std::ofstream layout(directory / kLayoutName, std::ios::binary | std::ios::trunc);
    if (!layout)
    {
        error = "Cannot write disc layout in " + directory.string();
        return false;
    }
    layout << kLayoutVersion << ' ' << m_size << '\n';
    std::vector<uint8_t> buffer(1024u * 1024u);
    const auto copy = [&](char kind, const std::string &key, const Entry &entry, const std::filesystem::path &relative) {
        if (!safeRelative(relative))
            return false;
        std::error_code ec;
        const auto path = directory / relative;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
            return false;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        uint64_t input = uint64_t(entry.lba) * kSectorSize;
        for (uint64_t left = entry.size; left != 0;)
        {
            const auto chunk = static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
            if (!readUnlocked(input, buffer.data(), chunk))
                return false;
            file.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(chunk));
            if (!file)
                return false;
            input += chunk;
            left -= chunk;
        }
        file.close();
        layout << kind << ' ' << entry.lba << ' ' << entry.size << ' ' << std::quoted(key) << ' '
               << std::quoted(relative.generic_string()) << '\n';
        return file.good() && layout.good();
    };

    // Preserve the ISO filesystem sectors for the IOP's file search and keep the
    // actual payload in one normal host file per ISO9660 entry.
    uint32_t headerEnd = m_rootEntry.lba;
    for (const auto &[key, entry] : m_entries)
        if (entry.size)
            headerEnd = std::min(headerEnd, entry.lba);
    if (headerEnd < 17 ||
        !copy('M', "", Entry{0, uint64_t(headerEnd) * kSectorSize, false}, ".layout/header.bin") ||
        !copy('D', "", m_rootEntry, ".layout/root.bin"))
    {
        error = "Could not install the disc filesystem metadata";
        return false;
    }
    std::vector<std::pair<std::string, Entry>> ordered(m_entries.begin(), m_entries.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.second.lba < b.second.lba; });
    for (const auto &[key, entry] : ordered)
    {
        const auto relative = entry.directory
            ? std::filesystem::path(".layout") / (std::to_string(entry.lba) + ".dir")
            : std::filesystem::path(key);
        if (!copy(entry.directory ? 'D' : 'F', key, entry, relative))
        {
            error = "Could not install " + key;
            return false;
        }
    }
    layout.close();
    if (!layout)
    {
        error = "Could not finish the disc layout";
        return false;
    }
    return true;
}

void PS2CdImage::resetUnlocked()
{
    if (m_file)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }
    m_entries.clear();
    m_installed.reset();
    m_rootEntry = {};
    m_size = 0;
    m_path.clear();
}

void PS2CdImage::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    resetUnlocked();
    m_openAttempted = false;
}

bool PS2CdImage::isOpen() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_file || m_installed;
}

bool PS2CdImage::isInstalled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_installed != nullptr;
}

bool PS2CdImage::ensureOpenFromIoPaths()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file || m_installed)
        {
            return true;
        }
        if (m_openAttempted)
        {
            return false;
        }
    }
    const std::filesystem::path image = PS2Runtime::getIoPaths().cdImage;
    if (image.empty())
    {
        return false;
    }
    return open(image);
}

bool PS2CdImage::find(std::string_view ps2Path, Entry &out) const
{
    const std::string key = normalizeKey(ps2Path);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file && !m_installed)
    {
        return false;
    }
    const auto it = m_entries.find(key);
    if (it == m_entries.end())
    {
        return false;
    }
    out = it->second;
    return true;
}

bool PS2CdImage::readUnlocked(uint64_t offset, void *destination, size_t size) const
{
    if (m_installed)
    {
        if (offset > m_size || size > m_size - offset || (!destination && size))
            return false;
        auto *out = static_cast<uint8_t *>(destination);
        const auto &extents = m_installed->extents;
        while (size != 0)
        {
            const auto next = std::upper_bound(extents.begin(), extents.end(), offset,
                                               [](uint64_t position, const auto &extent) { return position < extent.offset; });
            if (next != extents.begin())
            {
                const auto &extent = *std::prev(next);
                const uint64_t relative = offset - extent.offset;
                if (relative < extent.size)
                {
                    const auto count = static_cast<size_t>(std::min<uint64_t>(size, extent.size - relative));
                    if (!extent.file)
                        extent.file.reset(openFile(extent.path));
                    if (!extent.file || !seekFile(extent.file.get(), relative) ||
                        std::fread(out, 1u, count, extent.file.get()) != count)
                        return false;
                    offset += count;
                    out += count;
                    size -= count;
                    continue;
                }
            }
            const auto gap = next == extents.end() ? size
                : static_cast<size_t>(std::min<uint64_t>(size, next->offset - offset));
            std::memset(out, 0, gap);
            offset += gap;
            out += gap;
            size -= gap;
        }
        return true;
    }
    if (!m_file || !seekFile(m_file, offset))
    {
        return false;
    }
    return size == 0 || std::fread(destination, 1u, size, m_file) == size;
}

bool PS2CdImage::read(uint64_t offset, void *destination, size_t size) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return readUnlocked(offset, destination, size);
}

bool PS2CdImage::readSectors(uint32_t lba, uint32_t count, void *destination) const
{
    return read(static_cast<uint64_t>(lba) * kSectorSize, destination, static_cast<size_t>(count) * kSectorSize);
}

uint64_t PS2CdImage::sizeBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_size;
}

std::filesystem::path PS2CdImage::path() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_path;
}
