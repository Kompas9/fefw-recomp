// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone extraction frontend using Lichii's archive readers; no emulation
// is started.
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/string_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static fs::path selected_key_file;
std::filesystem::path FefwArchiveKeyFile() { return selected_key_file; }

constexpr u64 Title = 0x04007EF01F8AE000;

static void Emit(const nlohmann::json &value) {
  std::cout << value.dump() << std::endl;
}
static std::string Path(const fs::path &path) {
  return Common::FS::PathToUTF8String(path);
}
static void Check(bool ok, const char *error) {
  if (!ok)
    throw std::runtime_error(error);
}

struct Input {
  fs::path image, update, keys, out;
  std::set<std::string> builds;
};

static Input Arguments(int argc, char **argv) {
  Input result;
  for (int i = 1; i < argc; ++i) {
    const std::string name = argv[i];
    Check(i + 1 < argc, "Missing option value.");
    const std::string value = argv[++i];
    if (name == "--xci")
      result.image = fs::u8path(value);
    else if (name == "--update")
      result.update = fs::u8path(value);
    else if (name == "--keys")
      result.keys = fs::u8path(value);
    else if (name == "--out")
      result.out = fs::u8path(value);
    else if (name == "--build-id") {
      Check(value.size() == 40 &&
                std::ranges::all_of(
                    value, [](unsigned char c) { return std::isxdigit(c); }),
            "Invalid supported build ID.");
      auto id = value;
      std::ranges::transform(id, id.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
      });
      result.builds.insert(std::move(id));
    } else
      throw std::runtime_error("Unknown option.");
  }
  Check(fs::is_regular_file(result.image) && fs::is_regular_file(result.keys),
        "Choose an existing XCI and prod.keys file.");
  Check(result.update.empty() || fs::is_regular_file(result.update),
        "The selected update file does not exist.");
  Check(!result.out.empty() && fs::is_directory(result.out) &&
            fs::is_empty(result.out),
        "Output must be an existing empty scratch directory.");
  Check(!result.builds.empty(), "No supported executable build IDs supplied.");
  return result;
}

static bool Candidate(const std::shared_ptr<FileSys::NCA> &nca) {
  const auto status = nca->GetStatus();
  return (status == Loader::ResultStatus::Success ||
          status == Loader::ResultStatus::ErrorMissingBKTRBaseRomFS) &&
         nca->GetType() == FileSys::NCAContentType::Program &&
         (nca->GetTitleId() & ~u64{0x800}) == Title;
}

static std::string BuildId(const FileSys::VirtualDir &exefs) {
  if (!exefs)
    return {};
  const auto main = exefs->GetFile("main");
  std::array<u8, 0x54> header{};
  if (!main || main->Read(header.data(), header.size(), 0) != header.size() ||
      !std::equal(header.begin(), header.begin() + 4, "NSO0"))
    return {};
  return Common::HexToString(std::span<const u8>{header}.subspan(0x40, 20));
}

static void SafeName(const std::string &name) {
  Check(!name.empty() && name != "." && name != ".." &&
            name.find_first_of("/\\:\0", 0, 4) == std::string::npos,
        "An archive entry contains an unsafe path component.");
#ifdef _WIN32
  Check(name.back() != '.' && name.back() != ' ',
        "An archive entry ends with an unsafe Windows path character.");
#endif
}

static u64 Measure(const FileSys::VirtualDir &dir, unsigned depth = 0) {
  Check(dir != nullptr && depth < 128,
        "Invalid or excessively nested archive filesystem.");
  u64 total = 0;
  for (const auto &file : dir->GetFiles()) {
    SafeName(file->GetName());
    total += file->GetSize();
  }
  for (const auto &sub : dir->GetSubdirectories()) {
    SafeName(sub->GetName());
    total += Measure(sub, depth + 1);
  }
  return total;
}

static void Copy(const FileSys::VirtualDir &dir, const fs::path &out, u64 total,
                 u64 &done, u64 &reported, unsigned depth = 0) {
  Check(dir != nullptr && depth < 128,
        "Invalid or excessively nested archive filesystem.");
  fs::create_directories(out);
  std::vector<u8> buffer(4 * 1024 * 1024);
  for (const auto &file : dir->GetFiles()) {
    SafeName(file->GetName());
    const auto path = out / fs::u8path(file->GetName());
    Check(!fs::exists(path), "Duplicate or colliding archive output path.");
    std::ofstream target(path, std::ios::binary);
    Check(target.good(),
          "Cannot create an extracted file. Check disk space and permissions.");
    for (u64 offset = 0; offset < file->GetSize();) {
      const auto size = static_cast<size_t>(
          std::min<u64>(buffer.size(), file->GetSize() - offset));
      Check(file->Read(buffer.data(), size, offset) == size,
            "A decrypted archive read was incomplete.");
      target.write(reinterpret_cast<const char *>(buffer.data()), size);
      Check(target.good(), "An extracted write failed. Check free disk space.");
      offset += size;
      done += size;
      if (done == total || done - reported >= 16 * 1024 * 1024) {
        Emit({{"kind", "progress"}, {"done", done}, {"total", total}});
        reported = done;
      }
    }
    target.close();
    Check(!target.fail(), "Cannot finish writing an extracted file.");
  }
  for (const auto &sub : dir->GetSubdirectories()) {
    SafeName(sub->GetName());
    const auto path = out / fs::u8path(sub->GetName());
    Check(!fs::exists(path),
          "Duplicate or colliding archive output directory.");
    Copy(sub, path, total, done, reported, depth + 1);
  }
}

static void Extract(const Input &input) {
  selected_key_file = input.keys;
  (void)Core::Crypto::KeyManager::Instance();
  Emit({{"kind", "stage"},
        {"message", "Reading XCI and decrypting archive metadata..."}});
  FileSys::RealVfsFilesystem vfs;
  const auto image = vfs.OpenFile(Path(input.image), FileSys::OpenMode::Read);
  Check(image != nullptr, "Cannot open the XCI for reading.");
  FileSys::XCI xci{image, Title};
  Check(xci.GetStatus() == Loader::ResultStatus::Success,
        "Cannot read this XCI. Check the archive and your keys, including the "
        "game's key generation.");
  auto programs = xci.GetNCAs();
  std::unique_ptr<FileSys::NSP> update;
  if (!input.update.empty()) {
    const auto file = vfs.OpenFile(Path(input.update), FileSys::OpenMode::Read);
    Check(file != nullptr, "Cannot open the update NSP.");
    update = std::make_unique<FileSys::NSP>(file, Title);
    Check(update->GetStatus() == Loader::ResultStatus::Success,
          "Cannot read the update NSP. Check its keys and ticket.");
    const auto ncas = update->GetNCAsCollapsed();
    Check(std::ranges::any_of(ncas, Candidate),
          "The selected NSP has no readable Fire Emblem update program. Check the title and keys.");
    programs.insert(programs.end(), ncas.begin(), ncas.end());
  }
  std::shared_ptr<FileSys::NCA> base;
  for (const auto &nca : programs) {
    if (Candidate(nca) && !nca->IsUpdate()) {
      Check(!base,
            "Multiple Fire Emblem base programs found. Use a single-game XCI.");
      base = nca;
    }
  }
  Check(base != nullptr, "No readable Fire Emblem base program found. Check "
                         "the game, keys and archive.");
  std::shared_ptr<FileSys::NCA> chosen;
  std::set<std::string> seen;
  for (const auto &nca : programs) {
    if (!Candidate(nca))
      continue;
    auto resolved =
        nca->IsUpdate()
            ? std::make_shared<FileSys::NCA>(nca->GetBaseFile(), base.get())
            : nca;
    if (resolved->GetStatus() != Loader::ResultStatus::Success)
      continue;
    const auto id = BuildId(resolved->GetExeFS());
    if (!input.builds.contains(id) || !seen.insert(id).second)
      continue;
    Check(!chosen, "Multiple supported game versions found. Select a single "
                   "matching update.");
    chosen = std::move(resolved);
  }
  Check(chosen != nullptr, "No supported executable found. Supply the "
                           "supported update NSP and matching keys.");
  const auto exefs = chosen->GetExeFS();
  for (const auto *name :
       {"main", "sdk", "subsdk0", "subsdk1", "subsdk2", "rtld"})
    Check(exefs && exefs->GetFile(name),
          "The supported program is missing an ExeFS module.");
  const auto romfs = FileSys::ExtractRomFS(chosen->GetRomFS());
  Check(romfs != nullptr &&
            (!romfs->GetFiles().empty() || !romfs->GetSubdirectories().empty()),
        "Cannot read the merged RomFS.");
  const u64 total = Measure(exefs) + Measure(romfs);
  Check(fs::space(input.out).available >= total + 256 * 1024 * 1024,
        "Not enough free space for the extracted game.");
  Emit({{"kind", "stage"},
        {"message", "Extracting the supported ExeFS and merged RomFS..."}});
  u64 done = 0, reported = 0;
  Copy(exefs, input.out / "exefs", total, done, reported);
  Copy(romfs, input.out / "romfs", total, done, reported);
  Emit({{"kind", "complete"},
        {"build_id", BuildId(exefs)},
        {"done", done},
        {"total", total}});
}

static int Run(int argc, char **argv) {
  try {
    if (argc == 2 && std::string{argv[1]} == "--version") {
      Emit({{"protocol", 1}, {"backend", "Lichii"}});
      return 0;
    }
    Extract(Arguments(argc, argv));
    return 0;
  } catch (const std::runtime_error &error) {
    Emit({{"kind", "error"}, {"message", error.what()}});
  } catch (...) {
    Emit({{"kind", "error"},
          {"message", "Archive extraction failed. Check your input files, keys "
                      "and free disk space."}});
  }
  return 1;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> strings;
  std::vector<char *> pointers;
  for (int i = 0; i < argc; ++i)
    strings.push_back(Common::UTF16ToUTF8(
        std::u16string_view{reinterpret_cast<const char16_t *>(argv[i])}));
  for (auto &value : strings)
    pointers.push_back(value.data());
  return Run(argc, pointers.data());
}
#else
int main(int argc, char **argv) { return Run(argc, argv); }
#endif
