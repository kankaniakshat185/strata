#include "strata/fsutil.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace strata {

void MakeDirs(const std::string& path) {
  std::string prefix;
  size_t pos = 0;
  while (pos < path.size()) {
    size_t next = path.find('/', pos);
    if (next == std::string::npos) next = path.size();
    prefix = path.substr(0, next);
    if (!prefix.empty() && ::mkdir(prefix.c_str(), 0755) != 0 &&
        errno != EEXIST) {
      throw std::runtime_error("MakeDirs: mkdir failed for " + prefix + ": " +
                                std::strerror(errno));
    }
    pos = next + 1;
  }
}

std::vector<std::string> ListFilesWithSuffix(const std::string& dir,
                                              const std::string& suffix) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return out;

  struct dirent* entry;
  while ((entry = ::readdir(d)) != nullptr) {
    std::string name(entry->d_name);
    if (name.size() >= suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) ==
            0) {
      out.push_back(name);
    }
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

uint64_t NextBlockId(const std::string& dir, const std::string& suffix) {
  uint64_t next_id = 1;
  for (const auto& name : ListFilesWithSuffix(dir, suffix)) {
    uint64_t id = std::strtoull(name.c_str(), nullptr, 10);
    if (id >= next_id) next_id = id + 1;
  }
  return next_id;
}

uint64_t FileSize(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    throw std::runtime_error("FileSize: stat failed for " + path);
  }
  return static_cast<uint64_t>(st.st_size);
}

}  // namespace strata
