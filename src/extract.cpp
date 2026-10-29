#include "extract.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <sys/stat.h>

namespace vos {

namespace {

// True when `rel` stays inside the root once normalised: no absolute paths, no
// drive letters, no ".." segments. Archives are untrusted input.
bool safeRelative(const std::string& rel) {
  if (rel.empty()) return false;
  if (rel[0] == '/' || rel[0] == '\\') return false;
  if (rel.size() > 1 && rel[1] == ':') return false;
  size_t i = 0;
  while (i <= rel.size()) {
    size_t j = rel.find_first_of("/\\", i);
    std::string seg = rel.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (seg == "..") return false;
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return true;
}

}  // namespace

ExtractResult extractArchive(const fs::path& archivePath, const fs::path& dest,
                             const std::function<void(uint64_t)>& progress, const std::atomic<bool>* cancel) {
  ExtractResult res;
  std::error_code ec;
  fs::create_directories(dest, ec);

  struct archive* in = archive_read_new();
  archive_read_support_format_zip(in);
  archive_read_support_format_tar(in);
  archive_read_support_filter_gzip(in);
  archive_read_support_filter_none(in);

  struct archive* out = archive_write_disk_new();
  // Keep permissions (the executable bit matters) and mtimes; never follow
  // symlinks out of the tree, never set ownership.
  archive_write_disk_set_options(out, ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                                         ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS);
  archive_write_disk_set_standard_lookup(out);

  if (archive_read_open_filename(in, archivePath.string().c_str(), 1 << 20) != ARCHIVE_OK) {
    res.error = std::string("could not open the archive: ") + archive_error_string(in);
    archive_read_free(in);
    archive_write_free(out);
    return res;
  }

  struct archive_entry* e = nullptr;
  std::string fail;
  for (;;) {
    if (cancel && cancel->load()) {
      res.cancelled = true;
      break;
    }
    int r = archive_read_next_header(in, &e);
    if (r == ARCHIVE_EOF) {
      res.ok = true;
      break;
    }
    if (r < ARCHIVE_WARN) {
      fail = std::string("archive is damaged: ") + archive_error_string(in);
      break;
    }
    const char* name = archive_entry_pathname(e);
    std::string rel = name ? name : "";
    if (!safeRelative(rel)) {
      fail = "archive contains an unsafe path: " + rel;
      break;
    }
    // Hard/symlink targets must stay inside the root as well.
    if (const char* hl = archive_entry_hardlink(e); hl && !safeRelative(hl)) {
      fail = std::string("archive contains an unsafe link: ") + hl;
      break;
    }
    if (const char* sl = archive_entry_symlink(e); sl && !safeRelative(sl) && sl[0] == '/') {
      fail = std::string("archive contains an unsafe link: ") + sl;
      break;
    }
    std::string full = (dest / rel).string();
    archive_entry_set_pathname(e, full.c_str());
    if (const char* hl = archive_entry_hardlink(e)) {
      std::string hfull = (dest / hl).string();
      archive_entry_set_hardlink(e, hfull.c_str());
    }

    if (archive_write_header(out, e) < ARCHIVE_WARN) {
      fail = std::string("could not write ") + rel + ": " + archive_error_string(out);
      break;
    }
    if (archive_entry_size(e) > 0) {
      const void* buf;
      size_t len;
      la_int64_t off;
      bool bad = false;
      for (;;) {
        int d = archive_read_data_block(in, &buf, &len, &off);
        if (d == ARCHIVE_EOF) break;
        if (d < ARCHIVE_WARN) {
          fail = std::string("archive is damaged: ") + archive_error_string(in);
          bad = true;
          break;
        }
        if (archive_write_data_block(out, buf, len, off) < ARCHIVE_WARN) {
          fail = std::string("could not write ") + rel + ": " + archive_error_string(out);
          bad = true;
          break;
        }
        if (cancel && cancel->load()) {
          res.cancelled = true;
          bad = true;
          break;
        }
      }
      if (bad) break;
    }
    archive_write_finish_entry(out);
    if (progress) progress(static_cast<uint64_t>(archive_filter_bytes(in, -1)));
  }

  archive_read_close(in);
  archive_read_free(in);
  archive_write_close(out);
  archive_write_free(out);

  if (!fail.empty()) {
    res.ok = false;
    res.error = fail;
  }
  if (res.cancelled) res.ok = false;
  return res;
}

}  // namespace vos
