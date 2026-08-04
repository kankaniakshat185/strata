#include "strata/manifest.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "strata/fsutil.hpp"

namespace strata {

namespace {

bool FileExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

std::string ReadWholeFile(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("Manifest: open failed for " + path);
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error("Manifest: fstat failed for " + path);
  }
  std::string data(st.st_size, '\0');
  ssize_t n = ::pread(fd, data.data(), data.size(), 0);
  ::close(fd);
  if (n != static_cast<ssize_t>(data.size())) {
    throw std::runtime_error("Manifest: short read for " + path);
  }
  return data;
}

}  // namespace

Manifest LoadManifest(const std::string& path) {
  Manifest m;
  if (!FileExists(path)) return m;

  std::istringstream iss(ReadWholeFile(path));
  std::string level, name;
  while (iss >> level >> name) {
    if (level == "L0") {
      m.l0_blocks.push_back(name);
    } else if (level == "L1") {
      m.l1_blocks.push_back(name);
    }
  }
  return m;
}

void WriteManifestAtomic(const std::string& path, const Manifest& manifest) {
  std::string tmp_path = path + ".tmp";

  std::ostringstream oss;
  for (const auto& name : manifest.l0_blocks) oss << "L0 " << name << "\n";
  for (const auto& name : manifest.l1_blocks) oss << "L1 " << name << "\n";
  std::string content = oss.str();

  int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    throw std::runtime_error("Manifest: open failed for " + tmp_path);
  }
  ssize_t n = ::write(fd, content.data(), content.size());
  if (n != static_cast<ssize_t>(content.size())) {
    ::close(fd);
    throw std::runtime_error("Manifest: short write to " + tmp_path);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("Manifest: fsync failed for " + tmp_path);
  }
  ::close(fd);

  if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
    throw std::runtime_error("Manifest: rename failed: " +
                              std::string(std::strerror(errno)));
  }
}

Manifest LoadOrBootstrapManifest(const std::string& data_dir) {
  std::string path = data_dir + "/MANIFEST";

  Manifest m;
  if (!FileExists(path)) {
    // First run, or data written before Phase 3: treat everything
    // currently on disk as live and persist that as the initial manifest.
    m.l0_blocks = ListFilesWithSuffix(data_dir + "/L0", ".blk");
    m.l1_blocks = ListFilesWithSuffix(data_dir + "/L1", ".blk");
    WriteManifestAtomic(path, m);
  } else {
    m = LoadManifest(path);
  }

  // Cleanup: delete any block file on disk that the manifest doesn't know
  // about -- the leftover from a new L1 block whose compaction crashed
  // before the MANIFEST rename made it official.
  auto Cleanup = [](const std::string& dir,
                     const std::vector<std::string>& live) {
    for (const auto& name : ListFilesWithSuffix(dir, ".blk")) {
      if (std::find(live.begin(), live.end(), name) == live.end()) {
        ::unlink((dir + "/" + name).c_str());
      }
    }
  };
  Cleanup(data_dir + "/L0", m.l0_blocks);
  Cleanup(data_dir + "/L1", m.l1_blocks);

  return m;
}

}  // namespace strata
