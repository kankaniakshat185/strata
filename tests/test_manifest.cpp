// MANIFEST correctness: normal round trip, missing-file default, the
// crash-safety property the design doc calls for ("crash before rename
// means the old MANIFEST still points at the old blocks -- nothing
// lost"), bootstrapping from an existing data dir, and orphan cleanup.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "strata/fsutil.hpp"
#include "strata/manifest.hpp"

namespace {

bool Contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

void TouchFile(const std::string& path) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  assert(fd >= 0);
  ::close(fd);
}

void TestRoundTrip() {
  const std::string path = "/tmp/strata_test_manifest_roundtrip";
  ::unlink(path.c_str());
  ::unlink((path + ".tmp").c_str());

  strata::Manifest m;
  m.l0_blocks = {"00000001.blk", "00000002.blk"};
  m.RollupBlocks(1) = {"00000001.blk"};
  m.RollupBlocks(2) = {"00000001.blk", "00000002.blk"};
  // L3 deliberately left empty -- round trip should preserve "no blocks
  // at this level" as cleanly as "some blocks."
  strata::WriteManifestAtomic(path, m);

  strata::Manifest loaded = strata::LoadManifest(path);
  assert(loaded.l0_blocks.size() == 2);
  assert(Contains(loaded.l0_blocks, "00000001.blk"));
  assert(Contains(loaded.l0_blocks, "00000002.blk"));
  assert(loaded.RollupBlocks(1).size() == 1);
  assert(Contains(loaded.RollupBlocks(1), "00000001.blk"));
  assert(loaded.RollupBlocks(2).size() == 2);
  assert(Contains(loaded.RollupBlocks(2), "00000001.blk"));
  assert(Contains(loaded.RollupBlocks(2), "00000002.blk"));
  assert(loaded.RollupBlocks(3).empty());

  ::unlink(path.c_str());
}

void TestMissingFileIsEmpty() {
  strata::Manifest m = strata::LoadManifest("/tmp/strata_test_manifest_nope_xyz");
  assert(m.l0_blocks.empty());
  assert(m.RollupBlocks(1).empty());
  assert(m.RollupBlocks(2).empty());
  assert(m.RollupBlocks(3).empty());
}

void TestCrashBeforeRenameKeepsOldManifest() {
  const std::string path = "/tmp/strata_test_manifest_crash";
  ::unlink(path.c_str());
  ::unlink((path + ".tmp").c_str());

  strata::Manifest v1;
  v1.l0_blocks = {"00000001.blk"};
  strata::WriteManifestAtomic(path, v1);

  // Simulate a crash partway through writing v2: the temp file gets
  // written directly (not through WriteManifestAtomic, which would also
  // rename it) so the swap never completes.
  int fd = ::open((path + ".tmp").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  assert(fd >= 0);
  std::string content = "L0 00000002.blk\nL0 00000003.blk\n";
  ssize_t n = ::write(fd, content.data(), content.size());
  assert(n == static_cast<ssize_t>(content.size()));
  ::close(fd);

  // Old manifest at `path` must be untouched.
  strata::Manifest loaded = strata::LoadManifest(path);
  assert(loaded.l0_blocks.size() == 1);
  assert(loaded.l0_blocks[0] == "00000001.blk");

  ::unlink(path.c_str());
  ::unlink((path + ".tmp").c_str());
}

void TestBootstrapAndOrphanCleanup() {
  const std::string data_dir = "/tmp/strata_test_manifest_bootstrap";
  std::system(("rm -rf " + data_dir).c_str());
  strata::MakeDirs(data_dir + "/L0/");
  strata::MakeDirs(data_dir + "/L1/");

  TouchFile(data_dir + "/L0/00000001.blk");
  TouchFile(data_dir + "/L0/00000002.blk");

  // No MANIFEST exists yet: bootstrap should adopt both existing L0
  // blocks as live and persist that.
  strata::Manifest m = strata::LoadOrBootstrapManifest(data_dir);
  assert(m.l0_blocks.size() == 2);
  assert(Contains(m.l0_blocks, "00000001.blk"));
  assert(Contains(m.l0_blocks, "00000002.blk"));

  strata::Manifest persisted = strata::LoadManifest(data_dir + "/MANIFEST");
  assert(persisted.l0_blocks.size() == 2);

  // Now simulate an orphan: a block file that exists on disk but isn't
  // (and never was) in the manifest -- e.g. left behind by a compaction
  // that crashed after writing a new L1 block but before the rename.
  TouchFile(data_dir + "/L1/00000099.blk");  // orphan, not in manifest

  strata::Manifest m2 = strata::LoadOrBootstrapManifest(data_dir);
  assert(m2.RollupBlocks(1).empty());  // manifest still says no L1 blocks are live

  struct stat st;
  bool orphan_gone = ::stat((data_dir + "/L1/00000099.blk").c_str(), &st) != 0;
  assert(orphan_gone);

  std::system(("rm -rf " + data_dir).c_str());
}

void TestUnrecognizedLevelThrows() {
  // A previous version of the parser silently dropped any level token it
  // didn't recognize ("L9", garbage, etc.) -- quietly losing whatever
  // that line described. It should error loudly instead.
  const std::string path = "/tmp/strata_test_manifest_bad_level";
  ::unlink(path.c_str());

  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  assert(fd >= 0);
  std::string content = "L0 00000001.blk\nL9 00000002.blk\n";
  ssize_t n = ::write(fd, content.data(), content.size());
  assert(n == static_cast<ssize_t>(content.size()));
  ::close(fd);

  bool threw = false;
  try {
    strata::LoadManifest(path);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(threw);

  ::unlink(path.c_str());
}

}  // namespace

int main() {
  TestRoundTrip();
  TestMissingFileIsEmpty();
  TestCrashBeforeRenameKeepsOldManifest();
  TestBootstrapAndOrphanCleanup();
  TestUnrecognizedLevelThrows();
  std::printf("test_manifest: all assertions passed\n");
  return 0;
}
