// libcurl wrapper: JSON-ish requests and resumable, cancellable downloads.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "util.hpp"

namespace vos {

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::string body;
  std::string contentType;  // empty = none
  std::string bearer;       // empty = no Authorization header
  long timeoutSec = 30;
};

struct HttpResponse {
  long status = 0;  // 0 = transport failure
  std::string body;
  std::vector<std::string> setCookies;
  std::string error;  // transport-level error text
  bool ok() const { return status >= 200 && status < 300; }
};

void httpGlobalInit();
void httpGlobalCleanup();
HttpResponse httpRequest(const HttpRequest& req);

struct DownloadRequest {
  std::string url;
  fs::path file;                // appended to when resumeFrom > 0
  uint64_t resumeFrom = 0;
  std::function<void(uint64_t now, uint64_t total)> progress;  // absolute byte counts
  const std::atomic<bool>* cancel = nullptr;
};

struct DownloadResult {
  long status = 0;
  bool cancelled = false;
  bool completed = false;
  std::string error;
};

DownloadResult httpDownload(const DownloadRequest& req);

}  // namespace vos
