#include "http.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <cstring>

namespace vos {

void httpGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void httpGlobalCleanup() { curl_global_cleanup(); }

namespace {

const char* kUserAgent = "ZontoVolcano/" VOS_VERSION " VolcanoOS (Zonto Games launcher)";

size_t writeString(char* p, size_t sz, size_t n, void* ud) {
  static_cast<std::string*>(ud)->append(p, sz * n);
  return sz * n;
}

struct HeaderSink {
  std::vector<std::string>* cookies;
};

size_t headerCb(char* p, size_t sz, size_t n, void* ud) {
  size_t len = sz * n;
  static const char kSet[] = "set-cookie:";
  if (len > sizeof(kSet) - 1 && strncasecmp(p, kSet, sizeof(kSet) - 1) == 0) {
    std::string v(p + sizeof(kSet) - 1, len - (sizeof(kSet) - 1));
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
    size_t a = v.find_first_not_of(' ');
    static_cast<HeaderSink*>(ud)->cookies->push_back(a == std::string::npos ? "" : v.substr(a));
  }
  return len;
}

}  // namespace

HttpResponse httpRequest(const HttpRequest& req) {
  HttpResponse out;
  CURL* c = curl_easy_init();
  if (!c) {
    out.error = "curl init failed";
    return out;
  }
  curl_slist* headers = nullptr;
  if (!req.bearer.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + req.bearer).c_str());
  if (!req.contentType.empty()) headers = curl_slist_append(headers, ("Content-Type: " + req.contentType).c_str());
  headers = curl_slist_append(headers, "Accept: application/json");

  HeaderSink sink{&out.setCookies};
  curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, req.timeoutSec);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeString);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out.body);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headerCb);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &sink);
  if (req.method == "POST" || req.method == "PUT" || req.method == "PATCH") {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
  } else if (req.method != "GET") {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req.method.c_str());
  }

  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK) {
    out.error = curl_easy_strerror(rc);
  } else {
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out.status);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  return out;
}

namespace {

struct DlCtx {
  std::FILE* f = nullptr;
  fs::path path;
  uint64_t offset = 0;  // absolute position in the file
  bool resuming = false;
  long status = 0;
  bool statusChecked = false;
  bool failedStatus = false;
  CURL* curl = nullptr;
  const DownloadRequest* req = nullptr;
  uint64_t total = 0;
};

size_t dlWrite(char* p, size_t sz, size_t n, void* ud) {
  auto* c = static_cast<DlCtx*>(ud);
  if (!c->statusChecked) {
    c->statusChecked = true;
    curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &c->status);
    if (c->status >= 400) {
      c->failedStatus = true;
      return 0;  // abort; the caller reads `status`
    }
    // Asked for a range but the server sent the whole object: start over
    // rather than corrupting the archive by appending a full copy.
    if (c->resuming && c->status == 200) {
      std::fclose(c->f);
      c->f = std::fopen(c->path.string().c_str(), "wb");
      c->offset = 0;
      if (!c->f) return 0;
    }
  }
  size_t w = std::fwrite(p, 1, sz * n, c->f);
  c->offset += w;
  return w;
}

int dlProgress(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
  auto* c = static_cast<DlCtx*>(ud);
  if (c->req->cancel && c->req->cancel->load()) return 1;
  if (c->req->progress && c->statusChecked) {
    uint64_t base = (c->resuming && c->status == 206) ? c->req->resumeFrom : 0;
    uint64_t total = dltotal > 0 ? base + static_cast<uint64_t>(dltotal) : 0;
    c->req->progress(base + static_cast<uint64_t>(dlnow), total);
  }
  return 0;
}

}  // namespace

DownloadResult httpDownload(const DownloadRequest& req) {
  DownloadResult res;
  DlCtx ctx;
  ctx.req = &req;
  ctx.path = req.file;
  ctx.resuming = req.resumeFrom > 0;
  ctx.offset = req.resumeFrom;

  std::error_code ec;
  fs::create_directories(req.file.parent_path(), ec);
  ctx.f = std::fopen(req.file.string().c_str(), ctx.resuming ? "ab" : "wb");
  if (!ctx.f) {
    res.error = "could not open " + req.file.string() + " for writing";
    return res;
  }

  CURL* c = curl_easy_init();
  ctx.curl = c;
  curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  // Stall detection instead of a total timeout: archives are gigabytes.
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 45L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dlWrite);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dlProgress);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
  if (ctx.resuming) {
    std::string range = std::to_string(req.resumeFrom) + "-";
    curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
  }

  CURLcode rc = curl_easy_perform(c);
  if (ctx.status == 0) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &ctx.status);
  res.status = ctx.status;
  if (ctx.f) std::fclose(ctx.f);
  curl_easy_cleanup(c);

  if (rc == CURLE_ABORTED_BY_CALLBACK) {
    res.cancelled = true;
  } else if (ctx.failedStatus) {
    res.error = "download refused (HTTP " + std::to_string(ctx.status) + ")";
  } else if (rc != CURLE_OK) {
    res.error = curl_easy_strerror(rc);
  } else {
    res.completed = true;
  }
  return res;
}

}  // namespace vos
