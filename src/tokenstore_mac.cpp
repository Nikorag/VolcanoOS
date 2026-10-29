// macOS Keychain backend (generic password item) via the C Security API.
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <optional>
#include <string>

namespace vos::tokenstore {

namespace {
const char* kService = "co.uk.nikorag.zonto.volcanoos";
const char* kAccount = "bearer";

CFMutableDictionaryRef baseQuery() {
  CFMutableDictionaryRef q =
      CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFStringRef svc = CFStringCreateWithCString(nullptr, kService, kCFStringEncodingUTF8);
  CFStringRef acc = CFStringCreateWithCString(nullptr, kAccount, kCFStringEncodingUTF8);
  CFDictionarySetValue(q, kSecAttrService, svc);
  CFDictionarySetValue(q, kSecAttrAccount, acc);
  CFRelease(svc);
  CFRelease(acc);
  return q;
}
}  // namespace

std::optional<std::string> keychainLoad() {
  CFMutableDictionaryRef q = baseQuery();
  CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef out = nullptr;
  OSStatus st = SecItemCopyMatching(q, &out);
  CFRelease(q);
  if (st != errSecSuccess || !out) return std::nullopt;
  CFDataRef data = static_cast<CFDataRef>(out);
  std::string s(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<size_t>(CFDataGetLength(data)));
  CFRelease(out);
  if (s.empty()) return std::nullopt;
  return s;
}

void keychainClear();

bool keychainSave(const std::string& token) {
  keychainClear();
  CFMutableDictionaryRef q = baseQuery();
  CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(token.data()), static_cast<CFIndex>(token.size()));
  CFDictionarySetValue(q, kSecValueData, data);
  OSStatus st = SecItemAdd(q, nullptr);
  CFRelease(data);
  CFRelease(q);
  return st == errSecSuccess;
}

void keychainClear() {
  CFMutableDictionaryRef q = baseQuery();
  SecItemDelete(q);
  CFRelease(q);
}

}  // namespace vos::tokenstore
