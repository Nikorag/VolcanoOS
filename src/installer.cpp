#include "installer.hpp"

#include <sys/stat.h>

#include <chrono>
#include <cstdio>

#include "extract.hpp"
#include "http.hpp"
#include "json.hpp"
#include "sha256.hpp"

namespace vos {

using nlohmann::json;

static std::string safeVersion(const std::string& v) {
  std::string o;
  for (unsigned char c : v) o += (std::isalnum(c) || c == '.' || c == '-' || c == '_') ? static_cast<char>(c) : '_';
  return o.empty() ? "0" : o;
}

fs::path Installed::root() const { return gamesDir() / slugify(slug); }
fs::path Installed::versionDir() const { return root() / safeVersion(version); }
fs::path Installed::executable() const { return versionDir() / exePath; }

std::string InstallJob::message() const {
  std::lock_guard<std::mutex> l(m_);
  return message_;
}
void InstallJob::setMessage(std::string m) {
  std::lock_guard<std::mutex> l(m_);
  message_ = std::move(m);
}
const char* InstallJob::phaseLabel() const {
  switch (phase.load()) {
    case Queued: return "QUEUED";
    case Preparing: return "PREPARING";
    case Downloading: return "DOWNLOADING";
    case Verifying: return "VERIFYING";
    case Extracting: return "UNPACKING";
    case Finalizing: return "FINISHING";
    case Done: return "DONE";
    case Failed: return "FAILED";
    default: return "CANCELLED";
  }
}

Installer::~Installer() {
  std::vector<std::shared_ptr<InstallJob>> js;
  {
    std::lock_guard<std::mutex> l(m_);
    for (auto& [_, j] : jobs_) js.push_back(j);
  }
  for (auto& j : js) j->cancel.store(true);
  for (auto& j : js)
    if (j->worker.joinable()) j->worker.join();
}

void Installer::scan() {
  std::lock_guard<std::mutex> l(m_);
  installed_.clear();
  std::error_code ec;
  for (auto& d : fs::directory_iterator(gamesDir(), ec)) {
    if (!d.is_directory()) continue;
    std::string text;
    if (!readFile(d.path() / "manifest.json", text)) continue;
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) continue;
    Installed i;
    i.slug = j.value("slug", "");
    i.version = j.value("version", "");
    i.exePath = j.value("exePath", "");
    i.launchArgs = j.value("launchArgs", "");
    i.sha256 = j.value("sha256", "");
    if (i.slug.empty() || i.version.empty()) continue;
    if (!fs::exists(i.executable(), ec)) {
      logf("install of %s is missing %s; ignoring", i.slug.c_str(), i.exePath.c_str());
      continue;
    }
    installed_[i.slug] = i;
  }
}

const Installed* Installer::installed(const std::string& slug) const {
  std::lock_guard<std::mutex> l(m_);
  auto it = installed_.find(slug);
  return it == installed_.end() ? nullptr : &it->second;
}

std::vector<std::pair<std::string, std::string>> Installer::installs() const {
  std::lock_guard<std::mutex> l(m_);
  std::vector<std::pair<std::string, std::string>> out;
  for (auto& [slug, i] : installed_) out.emplace_back(slug, i.version);
  return out;
}

std::shared_ptr<InstallJob> Installer::job(const std::string& slug) const {
  std::lock_guard<std::mutex> l(m_);
  auto it = jobs_.find(slug);
  return it == jobs_.end() ? nullptr : it->second;
}

bool Installer::start(const std::string& slug, const std::string&, const BuildInfo& expected) {
  std::shared_ptr<InstallJob> j;
  {
    std::lock_guard<std::mutex> l(m_);
    if (jobs_.count(slug)) return false;
    j = std::make_shared<InstallJob>();
    j->slug = slug;
    jobs_[slug] = j;
  }
  j->total.store(expected.sizeBytes);
  j->worker = std::thread([this, j, expected] {
    if (demo_) runDemo(j, expected);
    else runJob(j, expected);
  });
  return true;
}

void Installer::cancel(const std::string& slug) {
  if (auto j = job(slug)) j->cancel.store(true);
}

bool Installer::uninstall(const std::string& slug) {
  if (job(slug)) return false;  // cancel first
  fs::path root;
  {
    std::lock_guard<std::mutex> l(m_);
    auto it = installed_.find(slug);
    root = (it != installed_.end() ? it->second.root() : gamesDir() / slugify(slug));
    installed_.erase(slug);
  }
  std::error_code ec;
  fs::remove_all(root, ec);  // also clears any .part
  return !ec;
}

void Installer::finish(const std::shared_ptr<InstallJob>& job, InstallJob::Phase p, const std::string& msg) {
  job->setMessage(msg);
  job->phase.store(p);
  std::string slug = job->slug;
  main_.post([this, slug, p, msg] {
    std::shared_ptr<InstallJob> j;
    {
      std::lock_guard<std::mutex> l(m_);
      auto it = jobs_.find(slug);
      if (it != jobs_.end()) {
        j = it->second;
        jobs_.erase(it);
      }
    }
    if (j && j->worker.joinable()) j->worker.join();  // thread is at its last line
    if (onFinished) onFinished(slug, p == InstallJob::Done, p == InstallJob::Cancelled, msg);
  });
}

void Installer::runJob(std::shared_ptr<InstallJob> job, BuildInfo expected) {
  const std::string slug = job->slug;
  Installed target;
  target.slug = slug;
  target.version = expected.version;
  target.exePath = expected.exePath;
  target.launchArgs = expected.launchArgs;
  target.sha256 = expected.sha256;

  fs::path root = target.root();
  std::error_code ec;
  fs::create_directories(root, ec);
  fs::path part = root / (safeVersion(expected.version) + ".part");
  fs::path incoming = root / (safeVersion(expected.version) + ".incoming");
  fs::path finalDir = target.versionDir();

  job->phase.store(InstallJob::Preparing);

  // ---- download (resumable; the signed URL is renewed each attempt) --------
  const int kAttempts = 4;
  std::string lastError = "download failed";
  bool downloaded = false;
  for (int attempt = 1; attempt <= kAttempts && !downloaded; ++attempt) {
    if (job->cancel.load()) return finish(job, InstallJob::Cancelled, "Cancelled");

    uint64_t have = fs::exists(part, ec) ? static_cast<uint64_t>(fs::file_size(part, ec)) : 0;
    auto dl = api_.buildDownload(slug);
    if (!dl.ok) {
      if (dl.status == 401 || dl.status == 403 || dl.status == 404) return finish(job, InstallJob::Failed, dl.error);
      lastError = dl.error;
      std::this_thread::sleep_for(std::chrono::seconds(2));
      continue;
    }
    // The build may have moved on since the library was fetched; trust the server.
    if (dl.value.info.version != expected.version) {
      target.version = dl.value.info.version;
      target.exePath = dl.value.info.exePath;
      target.launchArgs = dl.value.info.launchArgs;
      target.sha256 = dl.value.info.sha256;
      expected = dl.value.info;
      part = root / (safeVersion(expected.version) + ".part");
      incoming = root / (safeVersion(expected.version) + ".incoming");
      finalDir = target.versionDir();
      have = fs::exists(part, ec) ? static_cast<uint64_t>(fs::file_size(part, ec)) : 0;
    }
    job->total.store(expected.sizeBytes);
    if (have > expected.sizeBytes) {
      fs::remove(part, ec);
      have = 0;
    }
    if (have == expected.sizeBytes && have > 0) {
      downloaded = true;
      break;
    }

    job->phase.store(InstallJob::Downloading);
    job->done.store(have);
    DownloadRequest rq;
    rq.url = dl.value.url;
    rq.file = part;
    rq.resumeFrom = have;
    rq.cancel = &job->cancel;
    rq.progress = [&](uint64_t now, uint64_t total) {
      job->done.store(now);
      if (total) job->total.store(total);
    };
    DownloadResult r = httpDownload(rq);
    if (r.cancelled) return finish(job, InstallJob::Cancelled, "Cancelled");
    if (r.completed) {
      downloaded = true;
      break;
    }
    lastError = r.error.empty() ? "download failed" : r.error;
    logf("download attempt %d for %s failed: %s", attempt, slug.c_str(), lastError.c_str());
    // 416 means our .part is already past what the server has: start clean.
    if (r.status == 416) fs::remove(part, ec);
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  if (!downloaded) return finish(job, InstallJob::Failed, "Download failed: " + lastError);

  // ---- verify ---------------------------------------------------------------
  job->phase.store(InstallJob::Verifying);
  {
    uint64_t size = static_cast<uint64_t>(fs::file_size(part, ec));
    job->total.store(size);
    job->done.store(0);
    std::FILE* f = std::fopen(part.string().c_str(), "rb");
    if (!f) return finish(job, InstallJob::Failed, "Could not read the downloaded file.");
    Sha256 h;
    std::vector<char> buf(1 << 20);
    uint64_t read = 0;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
      h.update(buf.data(), n);
      read += n;
      job->done.store(read);
      if (job->cancel.load()) {
        std::fclose(f);
        return finish(job, InstallJob::Cancelled, "Cancelled");
      }
    }
    std::fclose(f);
    std::string got = Sha256::hex(h.finish());
    if (!expected.sha256.empty() && upper(got) != upper(expected.sha256)) {
      fs::remove(part, ec);  // corrupt: don't resume from it
      return finish(job, InstallJob::Failed, "Checksum mismatch. The download was corrupt; try again.");
    }
  }

  // ---- extract into <version>.incoming ---------------------------------------
  job->phase.store(InstallJob::Extracting);
  fs::remove_all(incoming, ec);
  job->done.store(0);
  job->total.store(static_cast<uint64_t>(fs::file_size(part, ec)));
  ExtractResult x = extractArchive(part, incoming, [&](uint64_t consumed) { job->done.store(consumed); }, &job->cancel);
  if (x.cancelled) {
    fs::remove_all(incoming, ec);
    return finish(job, InstallJob::Cancelled, "Cancelled");
  }
  if (!x.ok) {
    fs::remove_all(incoming, ec);
    return finish(job, InstallJob::Failed, "Unpack failed: " + x.error);
  }

  // ---- confirm, promote, record ---------------------------------------------
  job->phase.store(InstallJob::Finalizing);
  fs::path exe = incoming / target.exePath;
  if (!fs::exists(exe, ec)) {
    fs::remove_all(incoming, ec);
    return finish(job, InstallJob::Failed, target.exePath + " was not found in the archive.");
  }
#ifndef _WIN32
  if (fs::is_regular_file(exe, ec)) chmod(exe.string().c_str(), 0755);
#endif
  fs::remove_all(finalDir, ec);
  fs::rename(incoming, finalDir, ec);
  if (ec) {
    fs::remove_all(incoming, ec);
    return finish(job, InstallJob::Failed, "Could not move the install into place: " + ec.message());
  }
  json m = {{"slug", slug},           {"version", target.version},       {"exePath", target.exePath},
            {"launchArgs", target.launchArgs}, {"sha256", target.sha256}};
  if (!writeFileAtomic(root / "manifest.json", m.dump(2)))
    return finish(job, InstallJob::Failed, "Could not record the install.");

  // The new version is live: drop the old ones and the archive.
  for (auto& d : fs::directory_iterator(root, ec)) {
    if (d.is_directory() && d.path() != finalDir) fs::remove_all(d.path(), ec);
  }
  fs::remove(part, ec);
  {
    std::lock_guard<std::mutex> l(m_);
    installed_[slug] = target;
  }
  finish(job, InstallJob::Done, "Installed " + target.version);
}

// Offline demo: walks the same phases with fake progress so the UI can be
// exercised (and screenshotted) without a server.
void Installer::runDemo(std::shared_ptr<InstallJob> job, BuildInfo expected) {
  using namespace std::chrono_literals;
  uint64_t total = expected.sizeBytes ? expected.sizeBytes : 800ull << 20;
  job->total.store(total);
  job->phase.store(InstallJob::Downloading);
  for (int i = 0; i <= 100; ++i) {
    if (job->cancel.load()) return finish(job, InstallJob::Cancelled, "Cancelled");
    job->done.store(total / 100 * i);
    std::this_thread::sleep_for(40ms);
  }
  job->phase.store(InstallJob::Verifying);
  std::this_thread::sleep_for(500ms);
  job->phase.store(InstallJob::Extracting);
  job->done.store(0);
  for (int i = 0; i <= 50; ++i) {
    if (job->cancel.load()) return finish(job, InstallJob::Cancelled, "Cancelled");
    job->done.store(total / 50 * i);
    std::this_thread::sleep_for(30ms);
  }
  Installed t;
  t.slug = job->slug;
  t.version = expected.version;
  t.exePath = expected.exePath;
  {
    std::lock_guard<std::mutex> l(m_);
    installed_[job->slug] = t;
  }
  finish(job, InstallJob::Done, "Installed " + expected.version + " (demo)");
}

}  // namespace vos
