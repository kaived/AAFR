#pragma once

// SHA-256 (FIPS 180-4) for verifying imported research artifacts against
// their provenance manifest. Plain C++17, no I/O except ReadFileBytes.

#include <optional>
#include <string>

namespace aafr::research {

// Lower-case hex digest of `bytes`.
std::string Sha256Hex(const std::string& bytes);

// Whole file in binary mode; nullopt when it cannot be read.
std::optional<std::string> ReadFileBytes(const std::string& path);

}  // namespace aafr::research
