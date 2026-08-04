#pragma once

namespace strata {

// If the environment variable STRATA_CRASH_AT is set and equals `point`,
// sends this process a real SIGKILL right here, right now. A no-op
// otherwise (a single getenv + strcmp; negligible even called often).
//
// Why self-inflicted SIGKILL instead of an external harness racing a
// sleep against the target process (what tests/phase1_crash_recovery.sh
// does): the crash windows this is for -- e.g. "after the new L0 block
// is fsynced but before MANIFEST is updated" -- are a few statements
// wide, sub-millisecond. An external process has no way to land a kill
// inside a window that narrow. Instrumenting the exact point and having
// the process kill itself on cue is the standard technique for this (cf.
// RocksDB's SyncPoint, TiKV/etcd fail-points) and is what actually makes
// "formal" crash-recovery testing possible for Phase 6 rather than
// hoping a timed kill happens to land somewhere interesting.
//
// The Phase 6 test harness (tests/phase6_crash_recovery.sh, driven via
// `strata_tool crashtest`) sets STRATA_CRASH_AT before launching the
// child process; see ARCHITECTURE.md for the specific points instrumented
// and what each one proves.
void MaybeCrash(const char* point);

}  // namespace strata
