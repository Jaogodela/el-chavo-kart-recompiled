#include "xiso.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <set>
#include <system_error>

namespace eck::xiso {
namespace {

constexpr uint64_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kMagicSize = sizeof(kMagic) - 1;
// Where the game partition starts on the disc formats an Xbox 360 image can hold (XGD1, XGD2, XGD3, a bare partition).
constexpr std::array<uint64_t, 4> kPartitionBases = {0x18300000ull, 0xFD90000ull, 0x2080000ull, 0};
constexpr uint8_t kDirectoryAttribute = 0x10;
constexpr int kMaxDepth = 32;
constexpr size_t kMaxEntries = 1 << 20;
constexpr uint32_t kMaxDirectoryBytes = 64u << 20;
constexpr size_t kChunk = 1 << 20;

uint16_t Le16(const uint8_t* p) {
  return uint16_t(p[0] | (p[1] << 8));
}

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool SafeName(std::string_view name) {
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  return std::none_of(name.begin(), name.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
           c == '>' || c == '|';
  });
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

std::string TopFolder(std::string_view path) {
  const size_t slash = path.find('/');
  return std::string(slash == std::string_view::npos ? std::string_view() : path.substr(0, slash));
}

Result Fail(Status status, std::string detail) {
  return {status, std::move(detail)};
}

}  // namespace

Result Image::Open(const std::filesystem::path& path) {
  entries_.clear();
  total_bytes_ = 0;
  file_count_ = 0;
  file_.open(path, std::ios::binary);
  if (!file_) {
    return Fail(Status::kOpenFailed, "cannot open the image");
  }
  std::error_code ec;
  image_size_ = std::filesystem::file_size(path, ec);
  if (ec) {
    return Fail(Status::kOpenFailed, ec.message());
  }
  for (const uint64_t base : kPartitionBases) {
    const uint64_t header = base + 32 * kSector;
    if (header + 0x800 > image_size_) {
      continue;
    }
    uint8_t descriptor[0x800];
    file_.clear();
    file_.seekg(std::streamoff(header));
    if (!file_.read(reinterpret_cast<char*>(descriptor), sizeof(descriptor))) {
      continue;
    }
    if (std::memcmp(descriptor, kMagic, kMagicSize) != 0 ||
        std::memcmp(descriptor + 0x7EC, kMagic, kMagicSize) != 0) {
      continue;
    }
    base_ = base;
    const Result read = ReadDirectory(Le32(descriptor + 20), Le32(descriptor + 24), "", 0);
    if (!read.ok()) {
      return read;
    }
    return entries_.empty() ? Fail(Status::kCorrupt, "the image has no files") : Result{};
  }
  return Fail(Status::kNotImage, "no XDVDFS volume descriptor");
}

Result Image::ReadDirectory(uint64_t sector, uint32_t size, const std::string& prefix, int depth) {
  if (size == 0) {
    return {};
  }
  if (depth > kMaxDepth || size > kMaxDirectoryBytes || base_ + sector * kSector + size > image_size_) {
    return Fail(Status::kCorrupt, "directory table out of range below '" + prefix + "'");
  }
  std::vector<uint8_t> table(size);
  file_.clear();
  file_.seekg(std::streamoff(base_ + sector * kSector));
  if (!file_.read(reinterpret_cast<char*>(table.data()), std::streamsize(size))) {
    return Fail(Status::kCorrupt, "cannot read a directory table");
  }

  struct Child {
    std::string path;
    uint64_t sector;
    uint32_t size;
  };
  std::vector<Child> children;
  std::set<uint32_t> visited;
  std::vector<uint32_t> pending = {0};
  while (!pending.empty()) {
    const uint32_t offset = pending.back();
    pending.pop_back();
    if (!visited.insert(offset).second) {
      continue;
    }
    if (offset + 14 > size) {
      return Fail(Status::kCorrupt, "directory entry out of range");
    }
    const uint8_t* e = table.data() + offset;
    const uint16_t left = Le16(e);
    const uint16_t right = Le16(e + 2);
    const uint32_t entry_sector = Le32(e + 4);
    const uint32_t entry_size = Le32(e + 8);
    const uint8_t attributes = e[12];
    const uint8_t name_length = e[13];
    if (name_length == 0 || offset + 14 + name_length > size) {
      return Fail(Status::kCorrupt, "invalid directory entry");
    }
    const std::string name(reinterpret_cast<const char*>(e + 14), name_length);
    if (!SafeName(name)) {
      return Fail(Status::kUnsafePath, "unsafe name below '" + prefix + "'");
    }
    if (left != 0 && left != 0xFFFF) {
      pending.push_back(uint32_t(left) * 4);
    }
    if (right != 0 && right != 0xFFFF) {
      pending.push_back(uint32_t(right) * 4);
    }
    if (entries_.size() >= kMaxEntries) {
      return Fail(Status::kCorrupt, "too many entries");
    }
    Entry entry;
    entry.path = prefix + name;
    entry.offset = base_ + uint64_t(entry_sector) * kSector;
    entry.size = entry_size;
    entry.directory = (attributes & kDirectoryAttribute) != 0;
    if (entry.directory) {
      children.push_back({entry.path + "/", entry_sector, entry_size});
    } else {
      if (entry.offset + entry.size > image_size_) {
        return Fail(Status::kCorrupt, "'" + entry.path + "' extends past the end of the image");
      }
      total_bytes_ += entry.size;
      ++file_count_;
    }
    entries_.push_back(std::move(entry));
  }
  for (const Child& child : children) {
    if (Result read = ReadDirectory(child.sector, child.size, child.path, depth + 1); !read.ok()) {
      return read;
    }
  }
  return {};
}

const Entry* Image::Find(std::string_view path) const {
  for (const Entry& entry : entries_) {
    if (EqualsIgnoreCase(entry.path, path)) {
      return &entry;
    }
  }
  return nullptr;
}

Result Image::Read(const Entry& entry, std::vector<uint8_t>& out) {
  out.resize(size_t(entry.size));
  file_.clear();
  file_.seekg(std::streamoff(entry.offset));
  if (!file_.read(reinterpret_cast<char*>(out.data()), std::streamsize(entry.size))) {
    return Fail(Status::kCorrupt, "cannot read '" + entry.path + "'");
  }
  return {};
}

Result Image::ExtractTo(const std::filesystem::path& dir, const std::vector<std::string>& skip,
                        const Progress& progress) {
  const auto skipped = [&](const Entry& entry) {
    const std::string top = entry.directory && entry.path.find('/') == std::string::npos ? entry.path
                                                                                         : TopFolder(entry.path);
    return std::any_of(skip.begin(), skip.end(), [&](const std::string& s) { return EqualsIgnoreCase(top, s); });
  };
  uint64_t total = 0;
  for (const Entry& entry : entries_) {
    if (!entry.directory && !skipped(entry)) {
      total += entry.size;
    }
  }
  uint64_t done = 0;
  std::vector<char> buffer(kChunk);
  for (const Entry& entry : entries_) {
    if (skipped(entry)) {
      continue;
    }
    const std::filesystem::path target = dir / std::filesystem::path(entry.path).make_preferred();
    std::error_code ec;
    if (entry.directory) {
      std::filesystem::create_directories(target, ec);
      if (ec) {
        return Fail(Status::kWriteFailed, "cannot create '" + entry.path + "': " + ec.message());
      }
      continue;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Fail(Status::kWriteFailed, "cannot create '" + entry.path + "'");
    }
    file_.clear();
    file_.seekg(std::streamoff(entry.offset));
    uint64_t remaining = entry.size;
    while (remaining > 0) {
      const size_t chunk = size_t(std::min<uint64_t>(remaining, buffer.size()));
      if (!file_.read(buffer.data(), std::streamsize(chunk))) {
        return Fail(Status::kCorrupt, "cannot read '" + entry.path + "'");
      }
      if (!out.write(buffer.data(), std::streamsize(chunk))) {
        return Fail(Status::kWriteFailed, "cannot write '" + entry.path + "'");
      }
      remaining -= chunk;
      done += chunk;
      if (progress && !progress(done, total)) {
        return Fail(Status::kCancelled, {});
      }
    }
    out.close();
    if (!out) {
      return Fail(Status::kWriteFailed, "cannot finish '" + entry.path + "'");
    }
  }
  return {};
}

}  // namespace eck::xiso
