// Where the 30-day bearer lives: the OS keychain on macOS, otherwise a 0600
// file under the data dir. A launcher that can't persist a token would send
// the player to their browser on every start.
#pragma once

#include <optional>
#include <string>

namespace vos {

namespace tokenstore {
std::optional<std::string> load();
bool save(const std::string& token);
void clear();
}  // namespace tokenstore

}  // namespace vos
