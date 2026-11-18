#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

class PS2CdImage
{
public:
    struct Entry
    {
        uint32_t lba = 0;
        uint64_t size = 0;
        bool directory = false;
    };

    static constexpr uint32_t kSectorSize = 2048u;

    static PS2CdImage &instance();

    bool open(const std::filesystem::path &imagePath);
    // The source can be an ISO or an installed directory containing loose files.
    bool isInstalled() const;
    bool install(const std::filesystem::path &directory, std::string &error) const;
    void close();
    bool isOpen() const;
    bool ensureOpenFromIoPaths();

    bool find(std::string_view ps2Path, Entry &out) const;
    bool read(uint64_t offset, void *destination, size_t size) const;
    bool readSectors(uint32_t lba, uint32_t count, void *destination) const;
    uint64_t sizeBytes() const;
    std::filesystem::path path() const;

    static std::string normalizeKey(std::string_view ps2Path);

private:
    struct InstalledDisc;
    PS2CdImage();
    ~PS2CdImage();
    bool openInstalled(const std::filesystem::path &directory);
    void resetUnlocked();
    bool parseDirectory(uint32_t lba, uint32_t size, const std::string &prefix, int depth);
    bool readUnlocked(uint64_t offset, void *destination, size_t size) const;

    mutable std::mutex m_mutex;
    std::FILE *m_file = nullptr;
    std::unique_ptr<InstalledDisc> m_installed;
    std::filesystem::path m_path;
    uint64_t m_size = 0;
    bool m_openAttempted = false;
    std::unordered_map<std::string, Entry> m_entries;
    Entry m_rootEntry;
};
