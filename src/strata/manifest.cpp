#include "strata/manifest.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "strata/fsutil.hpp"

namespace strata {

std::vector<std::string>& Manifest::RollupBlocks(int level) {
  assert(level >= 1 && level <= kNumRollupLevels);
  return rollup_levels[level - 1];
}

const std::vector<std::string>& Manifest::RollupBlocks(int level) const {
  assert(level >= 1 && level <= kNumRollupLevels);
  return rollup_levels[level - 1];
}

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

// Parses "L<n>" into n (0 for L0, 1..kNumRollupLevels for rollup
// levels). Throws on anything else -- an earlier version of this parser
// silently dropped unrecognized level tokens, which would have quietly
// lost data the moment a level this binary didn't know about showed up
// in a MANIFEST it read. Loud and wrong beats quiet and wrong here.
int ParseLevelToken(const std::string& token, const std::string& path) {
  if (token.size() < 2 || token[0] != 'L') {
    throw std::runtime_error("Manifest: unrecognized level token '" + token +
                              "' in " + path);
  }
  for (size_t i = 1; i < token.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(token[i]))) {
      throw std::runtime_error("Manifest: unrecognized level token '" +
                                token + "' in " + path);
    }
  }
  int level = std::stoi(token.substr(1));
  if (level < 0 || level > kNumRollupLevels) {
    throw std::runtime_error("Manifest: level '" + token +
                              "' out of range in " + path);
  }
  return level;
}

std::string LevelDir(const std::string& data_dir, int level) {
  return data_dir + "/L" + std::to_string(level);
}

}  // namespace

Manifest LoadManifest(const std::string& path) {
  Manifest m;
  if (!FileExists(path)) return m;

  std::istringstream iss(ReadWholeFile(path));
  std::string level_token, name;
  while (iss >> level_token >> name) {
    int level = ParseLevelToken(level_token, path);
    if (level == 0) {
      m.l0_blocks.push_back(name);
    } else {
      m.RollupBlocks(level).push_back(name);
    }
  }
  return m;
}

void WriteManifestAtomic(const std::string& path, const Manifest& manifest) {
  std::string tmp_path = path + ".tmp";

  std::ostringstream oss;
  for (const auto& name : manifest.l0_blocks) oss << "L0 " << name << "\n";
  for (int level = 1; level <= kNumRollupLevels; ++level) {
    for (const auto& name : manifest.RollupBlocks(level)) {
      oss << "L" << level << " " << name << "\n";
    }
  }
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
    // First run, or data written before MANIFEST existed: treat
    // everything currently on disk as live and persist that as the
    // initial manifest.
    m.l0_blocks = ListFilesWithSuffix(LevelDir(data_dir, 0), ".blk");
    for (int level = 1; level <= kNumRollupLevels; ++level) {
      m.RollupBlocks(level) =
          ListFilesWithSuffix(LevelDir(data_dir, level), ".blk");
    }
    WriteManifestAtomic(path, m);
  } else {
    m = LoadManifest(path);
  }

  // Cleanup: delete any block file on disk that the manifest doesn't know
  // about -- the leftover from a new rollup block whose compaction
  // crashed before the MANIFEST rename made it official.
  auto Cleanup = [](const std::string& dir,
                     const std::vector<std::string>& live) {
    for (const auto& name : ListFilesWithSuffix(dir, ".blk")) {
      if (std::find(live.begin(), live.end(), name) == live.end()) {
        ::unlink((dir + "/" + name).c_str());
      }
    }
  };
  Cleanup(LevelDir(data_dir, 0), m.l0_blocks);
  for (int level = 1; level <= kNumRollupLevels; ++level) {
    Cleanup(LevelDir(data_dir, level), m.RollupBlocks(level));
  }

  return m;
}

}  // namespace strata
