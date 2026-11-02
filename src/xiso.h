#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace eck::xiso {

enum class Status { kOk, kOpenFailed, kNotImage, kCorrupt, kUnsafePath, kCancelled, kWriteFailed };

struct Result {
  Status status = Status::kOk;
  std::string detail;  // for the log, not the UI
  bool ok() const { return status == Status::kOk; }
};

struct Entry {
  std::string path;  // '/'-separated, relative to the image root
  uint64_t offset = 0;
  uint64_t size = 0;
  bool directory = false;
};

using Progress = std::function<bool(uint64_t done, uint64_t total)>;

// Read-only view of the XDVDFS game partition of an Xbox 360 disc image.
class Image {
 public:
  Result Open(const std::filesystem::path& path);

  const std::vector<Entry>& entries() const { return entries_; }
  uint64_t total_bytes() const { return total_bytes_; }
  size_t file_count() const { return file_count_; }

  // Case-insensitive, like the console's file system.
  const Entry* Find(std::string_view path) const;

  Result Read(const Entry& entry, std::vector<uint8_t>& out);

  // Writes every file whose top-level folder `skip` does not name below `dir`. `progress` returning false cancels.
  Result ExtractTo(const std::filesystem::path& dir, const std::vector<std::string>& skip, const Progress& progress);

 private:
  Result ReadDirectory(uint64_t sector, uint32_t size, const std::string& prefix, int depth);

  std::ifstream file_;
  uint64_t image_size_ = 0;
  uint64_t base_ = 0;
  std::vector<Entry> entries_;
  uint64_t total_bytes_ = 0;
  size_t file_count_ = 0;
};

}  // namespace eck::xiso
