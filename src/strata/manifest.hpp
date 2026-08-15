#pragma once

#include <string>
#include <vector>

// MANIFEST tracks which block filenames (across every level) currently
// represent live data, per STRATA_DESIGN.md's crash-safety section. It
// exists specifically so compaction's "write the new coarser block,
// delete the superseded finer ones" swap can't leave the engine in a
// state where a partially-completed swap looks like valid data: a crash
// between writing the new block and renaming the new MANIFEST into place
// means the old MANIFEST (and therefore the old blocks) is still the
// source of truth, and the orphaned-but-otherwise-valid new block is
// simply ignored.
//
// Format is plain text, one "<level> <filename>" pair per line (e.g.
// "L0 00000004.blk", "L2 00000001.blk"). MANIFEST is small, off the hot
// path, and benefits from being human-readable while hand-verifying
// compaction -- there's no reason to spend a binary format on it.
namespace strata {

// L1, L2, L3 -- the rollup levels this engine compacts to. L0 (raw
// points) is deliberately not part of this count: it decodes via a
// different code path (ReadL0Block vs ReadL1Block) and every rollup
// level shares the same block shape (RollupBucket), so it's the rollup
// levels specifically that are worth keeping in one indexable list.
inline constexpr int kNumRollupLevels = 3;

struct Manifest {
  std::vector<std::string> l0_blocks;  // filenames only, e.g. "00000004.blk"

  // rollup_levels[0] = L1's blocks, [1] = L2's, [2] = L3's. Always
  // exactly kNumRollupLevels long -- never grown/shrunk at runtime -- so
  // "this level has zero blocks" and "this level slot doesn't exist" are
  // never ambiguous. Use RollupBlocks(level) rather than indexing this
  // directly, to keep the level-to-index (level - 1) arithmetic in one
  // place.
  std::vector<std::vector<std::string>> rollup_levels =
      std::vector<std::vector<std::string>>(kNumRollupLevels);

  // `level` is 1, 2, or 3 (L1/L2/L3). Asserts on an out-of-range level --
  // every call site should already know which level it's dealing with,
  // so an out-of-range value is a caller bug, not a runtime condition to
  // handle gracefully.
  std::vector<std::string>& RollupBlocks(int level);
  const std::vector<std::string>& RollupBlocks(int level) const;
};

// Reads MANIFEST at `path`. Returns an empty Manifest if the file doesn't
// exist (first run, before any manifest has ever been written).
Manifest LoadManifest(const std::string& path);

// Writes `manifest` to `path` atomically: serialize to `path + ".tmp"`,
// fsync, then rename() over `path`. A crash at any point before the
// rename leaves the old MANIFEST at `path` untouched.
void WriteManifestAtomic(const std::string& path, const Manifest& manifest);

// Loads the MANIFEST at `<data_dir>/MANIFEST`, bootstrapping it from
// whatever's currently in L0/, L1/, L2/, and L3/ if it doesn't exist yet
// (e.g. data written before MANIFEST existed at all). Then deletes any
// block file present on disk but not listed in the (possibly just-
// bootstrapped) manifest -- the cleanup for a compaction that crashed
// after writing a new rollup block but before the MANIFEST rename made
// it official. Afterward, disk state and the returned Manifest are
// guaranteed to agree.
Manifest LoadOrBootstrapManifest(const std::string& data_dir);

}  // namespace strata
