#include "godhand/game_disc.h"
#include "godhand/log.h"
#include "godhand/sha256.h"
#include "runtime/ps2_cd_image.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <chrono>

namespace godhand
{
    namespace
    {
        bool readWholeFile(const PS2CdImage &image, std::string_view ps2Path, std::vector<uint8_t> &out)
        {
            PS2CdImage::Entry entry{};
            if (!image.find(ps2Path, entry) || entry.directory)
                return false;
            out.resize(static_cast<size_t>(entry.size));
            return image.read(static_cast<uint64_t>(entry.lba) * PS2CdImage::kSectorSize, out.data(), out.size());
        }

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        bool cnfBootsGodHand(const std::vector<uint8_t> &cnf)
        {
            const std::string text(cnf.begin(), cnf.end());
            return text.find("BOOT2") != std::string::npos && text.find(kSystemCnfBoot) != std::string::npos;
        }

        // Any ISO whose SYSTEM.CNF boots cdrom0:\SLUS_215.03;1 counts here; the boot ELF hash is checked afterwards.
        bool bootsGodHand(const std::filesystem::path &path)
        {
            // PS2CdImage is a singleton; verifyDiscAndPrepare reopens the chosen image afterwards.
            PS2CdImage &probe = PS2CdImage::instance();
            if (!probe.open(path))
                return false;
            std::vector<uint8_t> cnf;
            return readWholeFile(probe, "SYSTEM.CNF", cnf) && cnfBootsGodHand(cnf);
        }

        // The data files the game opens by name. A missing required file is an error; a different size is only
        // reported (the recompiled code depends on the boot ELF alone, which is hash-checked).
        bool checkDiscFiles(const PS2CdImage &image, std::string &error)
        {
            for (const DiscFile &file : kDiscFiles)
            {
                PS2CdImage::Entry entry{};
                if (!image.find(file.path, entry) || entry.directory)
                {
                    if (file.required)
                    {
                        error = std::string(file.path) + " (" + file.what + ") not found on the disc; expected " + kSerial;
                        return false;
                    }
                    GH_WARN(Boot, file.path << " (" << file.what << ") not found on the disc");
                    continue;
                }
                if (entry.size != file.size)
                    GH_WARN(Boot, file.path << " is " << entry.size << " bytes; the " << kSerial << " disc has " << file.size);
                else
                    GH_INFO(Boot, file.path << " " << entry.size << " bytes, lba " << entry.lba << " (" << file.what << ")");
            }
            // _DATA.AFS: "AFS\0" and its entry count.
            PS2CdImage::Entry afs{};
            uint8_t header[8]{};
            if (image.find("_DATA.AFS", afs) &&
                image.read(static_cast<uint64_t>(afs.lba) * PS2CdImage::kSectorSize, header, sizeof(header)))
            {
                uint32_t entries = 0;
                std::memcpy(&entries, header + 4, 4);
                if (std::memcmp(header, "AFS\0", 4) != 0)
                    GH_WARN(Boot, "_DATA.AFS does not start with \"AFS\\0\"");
                else if (entries != kDataAfsEntries)
                    GH_WARN(Boot, "_DATA.AFS has " << entries << " entries; the " << kSerial << " disc has " << kDataAfsEntries);
            }
            return true;
        }

        bool checkContent(const PS2CdImage &image, std::vector<uint8_t> &bootElf, std::string &error)
        {
            std::vector<uint8_t> systemCnf;
            if (!readWholeFile(image, "SYSTEM.CNF", systemCnf) || !cnfBootsGodHand(systemCnf))
            {
                error = std::string("SYSTEM.CNF does not boot ") + kSystemCnfBoot + "; expected " + kSerial;
                return false;
            }
            if (!checkDiscFiles(image, error))
                return false;
            if (!readWholeFile(image, kBootElfPath, bootElf))
            {
                error = std::string("boot executable ") + kBootElfPath + " missing";
                return false;
            }
            const std::string bootHash = Sha256::hex(bootElf.data(), bootElf.size());
            if (bootHash != kBootElfSha256)
            {
                error = std::string("boot executable hash mismatch (") + bootHash +
                        "); the recompiled code was built from " + kSerial + " " + kBootElfSha256;
                return false;
            }
            GH_INFO(Boot, kBootElfPath << " sha256 " << bootHash << " (matches " << kSerial << ")");
            return true;
        }
    }

    std::filesystem::path findDiscImage(const std::vector<std::filesystem::path> &searchRoots)
    {
        for (const auto &root : searchRoots)
        {
            std::error_code error;
            if (!std::filesystem::is_directory(root, error))
                continue;
            for (const auto &entry : std::filesystem::directory_iterator(root, error))
            {
                if (entry.is_regular_file(error) && lower(entry.path().extension().string()) == ".iso" && bootsGodHand(entry.path()))
                    return entry.path();
            }
        }
        return {};
    }

    DiscCheckResult verifyDiscAndPrepare(const std::filesystem::path &imagePath, const std::filesystem::path &cacheDir)
    {
        DiscCheckResult result;
        PS2CdImage &image = PS2CdImage::instance();
        if (!image.open(imagePath))
        {
            result.error = "cannot mount " + imagePath.string() + " as an ISO9660 disc image";
            return result;
        }

        std::vector<uint8_t> bootElf;
        if (!checkContent(image, bootElf, result.error))
            return result;

        std::error_code error;
        std::filesystem::create_directories(cacheDir, error);
        result.bootElfPath = cacheDir / kBootElfPath;
        std::ofstream out(result.bootElfPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bootElf.data()), static_cast<std::streamsize>(bootElf.size()));
        if (!out)
        {
            result.error = "cannot write " + result.bootElfPath.string();
            return result;
        }
        result.ok = true;
        return result;
    }

    DiscCheckResult openInstalledData(const std::filesystem::path &directory)
    {
        DiscCheckResult result;
        PS2CdImage &source = PS2CdImage::instance();
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec) || !source.open(directory) || !source.isInstalled())
        {
            result.error = "Cannot open installed game files at " + directory.string();
            return result;
        }
        std::vector<uint8_t> elf;
        if (!checkContent(source, elf, result.error))
            return result;
        result.bootElfPath = source.path() / kBootElfPath;
        result.ok = true;
        return result;
    }

    DiscCheckResult installGameData(const std::filesystem::path &imagePath, const std::filesystem::path &directory)
    {
        std::error_code ec;
        if (std::filesystem::exists(directory, ec))
            return openInstalledData(directory);
        auto &source = PS2CdImage::instance();
        DiscCheckResult result;
        if (!source.open(imagePath) || source.isInstalled())
        {
            result.error = "Cannot open source ISO " + imagePath.string();
            return result;
        }
        std::vector<uint8_t> elf;
        if (!checkContent(source, elf, result.error))
            return result;

        std::filesystem::create_directories(directory.parent_path(), ec);
        if (ec)
        {
            result.error = "Cannot create the parent game data directory: " + ec.message();
            return result;
        }
        auto staging = directory;
        staging += ".installing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        if (!std::filesystem::create_directory(staging, ec))
        {
            result.error = "Cannot create the game data staging directory: " + ec.message();
            return result;
        }
        struct RemoveStaging
        {
            std::filesystem::path path;
            ~RemoveStaging() { std::error_code ignored; if (!path.empty()) std::filesystem::remove_all(path, ignored); }
        } cleanup{staging};

        GH_INFO(Boot, "Installing local game files in " << directory.string());
        if (!source.install(staging, result.error))
            return result;
        // Validate the extracted files before publishing. Close the mounted data
        // first so Windows can rename the directory even if files were read.
        const DiscCheckResult verification = openInstalledData(staging);
        source.close();
        if (!verification.ok)
            return verification;
        std::filesystem::rename(staging, directory, ec);
        if (ec)
        {
            std::error_code existsError;
            if (!std::filesystem::exists(directory, existsError))
            {
                result.error = "Cannot publish the game data directory: " + ec.message();
                return result;
            }
        }
        else
            cleanup.path.clear();
        return openInstalledData(directory);
    }
}
