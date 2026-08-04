#pragma once

#include <string>
#include <vector>

// MANIFEST tracks which L0/L1 block filenames currently represent live
// data, per STRATA_DESIGN.md's crash-safety section. It exists specifically
// so compaction's "write new L1, delete superseded L0s" swap can't leave
// the engine in a state where a partially-completed swap looks like valid
// data: a crash between writing the new L1 block and renaming the new
// MANIFEST into place means the old MANIFEST (and therefore the old L0
// blocks) is still the source of truth, and the orphaned-but-otherwise-
// valid new L1 block is simply ignored.
//
// Format is plain text, one "<level> <filename>" pair per line (e.g.
// "L0 00000004.blk"). MANIFEST is small, off the hot path, and benefits
// from being human-readable while hand-verifying compaction -- there's no
// reason to spend a binary format on it.
namespace strata {

struct Manifest {
  std::vector<std::string> l0_blocks;  // filenames only, e.g. "00000004.blk"
  std::vector<std::string> l1_blocks;
};

// Reads MANIFEST at `path`. Returns an empty Manifest if the file doesn't
// exist (first run, before any manifest has ever been written).
Manifest LoadManifest(const std::string& path);

// Writes `manifest` to `path` atomically: serialize to `path + ".tmp"`,
// fsync, then rename() over `path`. A crash at any point before the
// rename leaves the old MANIFEST at `path` untouched.
void WriteManifestAtomic(const std::string& path, const Manifest& manifest);

// Loads the MANIFEST at `<data_dir>/MANIFEST`, bootstrapping it from
// whatever's currently in L0/ and L1/ if it doesn't exist yet (e.g. data
// written before Phase 3 introduced MANIFEST). Then deletes any block file
// present on disk but not listed in the (possibly just-bootstrapped)
// manifest -- the cleanup for a compaction that crashed after writing a
// new L1 block but before the MANIFEST rename made it official.
// Afterward, disk state and the returned Manifest are guaranteed to agree.
Manifest LoadOrBootstrapManifest(const std::string& data_dir);

}  // namespace strata
