#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace strata {

// mkdir -p semantics: creates `path` and any missing parent directories.
void MakeDirs(const std::string& path);

// Lists filenames (not full paths) directly inside `dir` matching
// `suffix`, e.g. ".blk". Returns empty if `dir` doesn't exist.
std::vector<std::string> ListFilesWithSuffix(const std::string& dir,
                                              const std::string& suffix);

// Scans `dir` for files named "<digits><suffix>" and returns one past the
// largest id found (1 if none exist), for block-file numbering.
uint64_t NextBlockId(const std::string& dir, const std::string& suffix);

// Size in bytes of the file at `path`.
uint64_t FileSize(const std::string& path);

}  // namespace strata
