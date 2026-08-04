# Architecture decisions & trade-offs

Companion to [STRATA_DESIGN.md](STRATA_DESIGN.md), which locks in the on-disk
format and phase plan. This file tracks *implementation* decisions made while
building each phase — the places where the design doc left room for a
choice, or where reality (missing tools, subtle correctness issues) forced
one. Updated as each phase lands.

---

## Phase 0 — Setup

**Makefile instead of CMake.** `cmake` isn't installed on the build machine
and installing it wasn't asked for. The design doc lists "CMake/Makefile" as
interchangeable for Phase 0, and this project's actual needs — no external
dependencies, one platform, well under 20 source files even once every phase
lands — don't need CMake's `find_package`/`FetchContent` machinery. Revisit
if a test framework dependency (e.g. Catch2) or cross-platform build becomes
a real requirement.

---

## Phase 1 — WAL + MemTable + flush

**Explicit little-endian encode/decode helpers ([byteio.hpp](src/strata/byteio.hpp)) instead of `sizeof(struct)`.**
A `WalRecord` struct of `{uint64_t, int64_t, double, uint32_t}` doesn't
naturally pack to 28 bytes — the trailing 4-byte field gets padded to keep
8-byte alignment, so `sizeof(WalRecord)` is 32, not 28. Serializing field-by-field
through explicit helpers sidesteps struct layout entirely and makes the
"little-endian, documented explicitly" requirement literal in code rather
than incidental to the host platform.

**CRC32 per WAL record, checked on replay, not just file-size % 28.**
The design doc's torn-write detection is "file size isn't a multiple of 28."
That catches a crash mid-write of the *last* record, but not single-bit
corruption inside an otherwise complete-looking record. Checking the CRC on
every record during replay catches both with one mechanism, and both are
treated identically: stop replay there, truncate the file to the last known-good
record.

**Flush isn't atomic with WAL reset — a known, accepted gap.** `Engine::Flush()`
writes and fsyncs a new L0 block, *then* deletes and recreates the WAL. A
crash in between means those points exist in both the just-written L0 block
and (after replay) get re-inserted and eventually re-flushed into a second
L0 block. Nothing reads L0 data for real yet, so duplicate points across two
blocks are currently harmless. This needs revisiting once Phase 3
(compaction) or Phase 5 (queries) actually reads L0 — either by making the
L0-write-then-WAL-delete sequence crash-safe (e.g. a marker file, similar in
spirit to the MANIFEST swap already planned for compaction), or by having
readers de-duplicate by `(series_id, timestamp)`.

**Single WAL file (`wal/000001.wal`), no segment rotation.** The design
doc's directory layout only shows one WAL file at this stage. A single
growing-then-reset file is simplest and matches current scale; multiple
numbered segments would only earn their complexity if flush thresholds and
WAL rotation ever need to be decoupled.

**L0 block ID is derived by scanning the `L0/` directory on startup**, not
persisted as separate counter state. One less piece of state to keep
consistent with reality; costs an `O(#blocks)` directory listing at startup,
which is fine at the block counts Phase 1–2 produce and gets reconsidered
once compaction (Phase 3) starts keeping block counts bounded anyway.

---

## Phase 2 — Gorilla encoding

**Block container (header/index/footer) built and tested before the data
stream encoding was real.** Phase 1 shipped L0 blocks with raw 16-byte
`(timestamp, value)` pairs as a placeholder data stream, specifically so the
part of the format that crash recovery and (later) the query router
actually depend on — the header, series index, and footer — could be
locked down and tested independently of the compression scheme. Phase 2
only had to swap the data-stream encoder; nothing else in the block format
changed.

**Bit ordering (MSB-first) is an arbitrary but load-bearing choice.**
[bitio.hpp](src/strata/bitio.hpp)'s `BitWriter`/`BitReader` pack bits most-significant-first
within each byte. This has no external interop requirement to satisfy — it
only has to be self-consistent between writer and reader, which it is by
construction (same class of code on both ends).

**Leading-zero-count field clamped to 5 bits (max 31), per the design
doc's field width — even though real XOR values commonly exceed that.**
Two consecutive readings of a slowly-varying metric (like 45.10 → 45.12)
often differ only in low mantissa bits, which can mean *well* over 31
leading zero bits in the 64-bit XOR. Following the standard approach used
by real Gorilla implementations (Facebook's paper, Prometheus, `go-tsz`):
clamp the stored leading-zero count to 31 and let the "meaningful bits"
window absorb the extra always-zero bits above that. Still correct — it
just occasionally spends a few more bits than the theoretical minimum. The
alternative (widening the field past the design doc's 5 bits) would be more
bit-efficient but breaks from the locked-in spec for a marginal gain.

**The delta-of-delta "reserved zero code means the range's max value" trick
(from the original Gorilla paper) is applied uniformly to all four range
branches, including the 32-bit catch-all.** The design doc's range table
(e.g. `[-63, 64]` for 7 bits) is asymmetric versus a plain signed field
(`[-64, 63]`) specifically because of this trick, so it was needed for the
three bounded ranges regardless. Applying it to the 32-bit branch too costs
nothing and keeps the encode/decode helper uniform across all four cases —
it just harmlessly extends that branch's range by one value.

**`prev_delta` starts at 0 before the second point, rather than
special-casing the first delta as a raw value.** This keeps the encoder as
one uniform loop — "every point after the first is delta-of-delta" — matching
the design doc's literal description, instead of a separate first-delta code
path. The cost: the very first gap between point 0 and point 1 is encoded
as if the "previous gap" were zero, so it usually lands in the largest
bucket (32-bit) if that gap is nonzero. For any series with more than a
handful of points this is negligible against total series size.

---

## Phase 3 — Compaction with downsampling

**MANIFEST is plain text, not a binary format.** Unlike the block formats,
STRATA_DESIGN.md doesn't pin down MANIFEST's exact bytes — only that it
must list live blocks per level and support an atomic temp-file-then-rename
swap. It's tiny, off the hot path, and this phase's checkpoint is literally
"hand-verify" — a human-readable `L0 00000004.blk` / `L1 00000001.blk` file
is easier to inspect mid-debugging than a binary one, at zero cost since
there's no compression or performance reason to prefer binary here.

**"Background worker" is, for now, an explicit synchronous call
(`RunCompaction`, invoked via `strata_tool compact`), not an actual
background thread.** The design doc's Phase 3 checkpoint is about the
compaction *mechanism* — bucketing, downsampling, the MANIFEST swap — not
about scheduling. A real background thread would need to coordinate with
`Engine`'s WAL/MemTable/flush path (which blocks aren't safe to compact
while mid-flush, locking) — real concurrency-control work that's out of
proportion to what this phase's checkpoint asks for. Scoped down the same
way the design doc itself scopes down L1's XOR compression: noted here as
a deliberate simplification, not an oversight. Revisit if/when a real
scheduler is worth building.

**Compaction eligibility uses the block header's `created_at` field**,
which STRATA_DESIGN.md already defines for exactly this purpose ("wall
clock, drives age-based compaction") — not file mtime, which would work
too but ignores a field the format already carries for this job.

**L1's series index entry reuses `SeriesIndexEntry.point_count` to mean
"bucket count."** Same 24-byte layout as L0's index entry, no format
change needed — the block header's `level` byte already tells a reader
which unit to expect, so a separate field would just duplicate that
information.

**Multiple eligible L0 blocks are merged by concatenating each series'
points and sorting once, not streamed/merged incrementally.** Simpler, and
correct as long as a compaction batch's total point count stays in the
"fits comfortably in memory" range it's expected to at this project's
scale. A true production system compacting many-GB batches would want a
streaming k-way merge instead.

**p99 uses the nearest-rank method** (sort ascending, take element at
`ceil(0.99 * n)`, 1-indexed) — deterministic and simple to hand-verify
against a reference, which is exactly what this phase's checkpoint calls
for. It's also honest about small-`n` behavior: for a bucket with only a
handful of points, nearest-rank p99 often equals the max (see
`tests/test_compaction.cpp`'s 5- and 7-point buckets) — not a bug, just
what "99th percentile" means with too few samples to have a meaningful
tail. STRATA_DESIGN.md's benchmarking plan already calls out comparing
rollup p99 against the true raw p99 as a tradeoff to report honestly in
Phase 7's writeup.

**MANIFEST-driven orphan cleanup runs on every `Engine`/compaction
startup, not as a separate maintenance pass.** `LoadOrBootstrapManifest`
both loads (or bootstraps, for pre-Phase-3 data directories) the manifest
*and* deletes any on-disk block file it doesn't list — the cleanup for a
compaction that crashed after writing a new L1 block but before the
MANIFEST rename made it official. Folding cleanup into every load keeps a
simple invariant: after `Engine::Engine()` or `RunCompaction()` starts,
disk state and the in-memory manifest are guaranteed to agree, with no
separate "run the cleanup job" step to remember.

**`Engine::Stats()` and `Flush()` now go through the manifest instead of
raw directory listings**, closing part of the gap Phase 1 flagged: before
MANIFEST existed, stats came from listing `L0/*.blk` directly, which would
have wrongly counted an orphaned block left by an interrupted compaction
swap. The underlying flush/WAL-reset non-atomicity noted in Phase 1 is
unchanged — MANIFEST fixes the compaction-swap crash window, not that one.

---

## Phase 4 — Inverted label index

**Keyed by the full `"key=value"` matcher, not by key alone.** The design
doc's diagram says "label key → series IDs," but a key-only index (e.g.
`host` → every series with any host) wouldn't actually solve the
cardinality problem it's meant to solve — nearly every series has a
`host`, so that postings list would be close to the whole index. Keying by
the complete pair (`host=h3`) is what makes the index selective, and is
how Prometheus's label index actually works, which STRATA_DESIGN.md's own
interview-framing section names as the point of comparison.

**No separate persisted index format — it's entirely rebuilt from
`series_catalog.log` on startup.** Series identity is immutable once a
series_id is assigned (STRATA_DESIGN.md is explicit that ids are a
monotonic counter, never reused), so the index is a pure function of the
catalog log's contents. `SeriesCatalog` owns the `InvertedIndex` and
updates it at the only two places series get created: `GetOrCreate` (new
series during normal writes) and `Replay` (startup). This was already
implied by the design doc's own text under "Series catalog" — replaying
the log is described as rebuilding "the inverted index," not just the
id map — Phase 4 just makes that literal.

**Postings-list sort order is a load-bearing invariant, not an
accident.** `IntersectQuery` uses a merge-based `std::set_intersection`
(O(n+m), not a hash-based scan) instead of sorting on every query call.
That's only correct because every postings list is guaranteed sorted
ascending by construction: `series_id` is assigned by a monotonic
counter, and `AddSeries` is only ever called in that same creation order
(via `GetOrCreate` for new writes, via `Replay` in file order for
recovery — both already increasing). Flagged explicitly in
[inverted_index.hpp](src/strata/inverted_index.hpp) because it's the kind
of invariant that's easy to silently violate later (e.g. a future
deletion/merge feature that reorders or removes ids) without anything
obviously breaking until a query returns a wrong, silently-incomplete
result.

**Intersection starts from the smallest postings list.** A query like
`{region=us-east, host=h3}` mixes a highly selective filter (one host)
with a filter that might match a large fraction of the index (a whole
region). Sorting the requested lists by size before intersecting, and
starting from the smallest, keeps the running intersection small the
whole way through instead of repeatedly filtering a huge list down. This
is exactly what the cardinality-scaling benchmark below demonstrates:
3-filter intersection latency stays roughly flat even once the
shared-label postings lists (metric, region) have grown to hundreds of
thousands of entries.

**The cardinality-scaling benchmark drives `InvertedIndex` directly,
bypassing `SeriesCatalog`'s disk-backed path entirely.** `GetOrCreate`
fsyncs on every new series — appropriate for real writes, but it would
make a 1M-series benchmark measure disk latency, not the index's own
scaling behavior, which is what this checkpoint is actually about. The
benchmark tool (`strata_tool cardbench`) builds an `InvertedIndex` in
memory and assigns series_ids itself.

**Benchmark data shape: one high-cardinality dimension (`host`, unique per
series) plus two shared low-cardinality dimensions (`metric`, `region`,
each a handful of values).** Uniform-cardinality synthetic data (every
label unique) wouldn't exercise the actual failure mode the design doc's
"cardinality explosion" section describes — the interesting cost is
postings lists for common label values (a whole region, a whole metric
type) growing alongside total series count. This shape produces both: N
one-entry postings lists (`host=hN`) and a handful of postings lists that
grow to a large fraction of N (`region=us-east`), which is what makes the
small-list-first intersection optimization above actually matter in the
benchmark rather than being untested code.

**`EstimatedBytes()` is an approximation, not a real memory measurement.**
`unordered_map`'s actual per-bucket/node overhead is implementation- and
platform-specific; the estimate sums string/vector capacities plus a
flat per-entry guess. Good enough to plot a size-vs-cardinality trend
(the benchmark's purpose), not precise enough to be a memory budget.

Results at 1K / 10K / 100K / 1M unique combos (measured on the dev
machine; full data in `tests/` output and the cardinality-bench
artifact): index size scales roughly linearly with cardinality as
expected (~147KB at 1K to ~147MB at 1M), while lookup and intersection
latency both stay in the sub-microsecond-to-low-microsecond range even
at 1M — the one exception being 3-filter intersection p99 at 1M, which
jumps to ~25.5µs, plausibly tail latency from occasionally hitting the
large shared-label postings lists before the small-list-first ordering
kicks in. Worth digging into further if Phase 7's writeup wants to make
a claim about worst-case query latency, not just typical-case.

---

## Git

The repo is committed per phase as each phase's checkpoint passes, so
`git log` doubles as a build history matching [STRATA_DESIGN.md](STRATA_DESIGN.md)'s
phase plan.
