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

}  // namespace strata
