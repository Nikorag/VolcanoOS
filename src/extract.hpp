// Archive extraction (zip and tar.gz) through libarchive, refusing any entry
// that would land outside the install root.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "util.hpp"

namespace vos {

struct ExtractResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;
};

// `progress(bytesReadFromArchive)` is called periodically; `archiveSize` lets
// callers turn it into a fraction.
ExtractResult extractArchive(const fs::path& archive, const fs::path& destDir,
                             const std::function<void(uint64_t)>& progress,
                             const std::atomic<bool>* cancel);

}  // namespace vos
