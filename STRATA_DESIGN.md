# Strata — A Tiered-Resolution Time-Series Storage Engine

## One-line pitch

A time-series database that treats **compaction as downsampling**: raw high-resolution
metrics are Gorilla-compressed on write, then a background compaction pass rolls
older data into a coarser-resolution summary (LSM-style leveling), while an inverted
label index keeps high-cardinality queries fast. Compaction levels are literally
strata — layers of decreasing resolution as data ages, the way sediment settles.

**Language:** C++
**Timeline:** 2 weeks, build + understand deeply enough to defend every design choice.

---

## The problem

Systems that ingest continuous time-stamped data (server metrics, IoT sensors,
financial ticks) face three problems usually treated separately but really one
system wearing different hats:

1. **Ingest volume** — thousands of sources emitting readings every 1–5 seconds
   produces enormous write volume; naive storage is slow to write and huge on disk.
2. **Cardinality explosion** — metrics tagged with labels (host, region, service)
   can produce millions of unique label combinations; indexing them naively kills
   memory and query latency.
3. **Retention vs. resolution** — nobody needs second-by-second data from six
   months ago, but discarding it loses trend analysis. Production systems keep
   recent data at full resolution and age data into coarser resolution over time.

| Technique | Problem solved |
|---|---|
| Gorilla compression (delta-of-delta timestamps + XOR float encoding) | Ingest volume |
| Compaction-as-downsampling (LSM-inspired, single level: L0→L1) | Retention vs. resolution |
| Inverted label index | Cardinality explosion |

This is the ELT pattern applied to a storage engine: points **Load** into L0
untransformed and fast (WAL → MemTable → flush), and the expensive work —
statistical **Transform** — happens later, off the write path, during compaction.
Most from-scratch time-series projects treat compression, compaction, and indexing
as three separate bolted-on features; Strata's distinguishing claim is that
compaction isn't just merging sorted runs, it's the transform step of an ELT
pipeline, applied as a storage-engine design principle.

---

## Architecture / pipeline

```
Agents ──push──> Ingestion API ──> WAL ──> MemTable (sorted by time)
                                                │
                                         flush on threshold
                                                ▼
                                   L0: raw-resolution, Gorilla-compressed blocks
                                                │
                                   background compaction (age-triggered)
                                                ▼
                                   L1: rollups (min / max / avg / count / p99)

                     Inverted Index: label key → series IDs, intersected across filters

Query Router: recent range → L0; older range → L1; range spanning both → stitch
```

- **Write path**: each point appended to a WAL for crash safety, inserted into an
  in-memory MemTable keyed by `(series_id, timestamp)`, flushed to an immutable
  Gorilla-compressed L0 block once the MemTable crosses a size/time threshold.
- **Compaction path**: a background worker scans L0 blocks older than a threshold,
  groups points into fixed windows, and emits one rollup record per window
  (min/max/avg/count/p99) into an L1 block — a real statistical transform, not
  dedup.
- **Read path**: a query specifies metric, label filter, time range. The inverted
  index resolves the filter to candidate series before any time-series data is
  touched; the query router picks L0, L1, or both based on the requested range.

---

## Implemented vs. stretch goals

Be explicit about this in the README — it's more credible than overclaiming.

**Core (build this):**
- WAL, MemTable, flush to L0
- Gorilla compression (delta-of-delta + XOR)
- One compaction level (L0→L1) with statistical downsampling
- Inverted label index
- Query router
- Crash recovery via WAL replay, verified with fault injection

**Stretch (only if time remains):**
- Additional compaction levels (L1→L2→L3) — same mechanism, longer thresholds
- B+ tree index as a comparison point against the inverted index

---

## Locked-in on-disk format

### Series identity

- **Canonical label string**: sort labels alphabetically by key, join as
  `metric=cpu_usage,host=a1,region=us-east`.
- **Series catalog** (`series_catalog.log`): append-only file of
  `(series_id: uint64, label_len: uint32, label_string: bytes)`, written once per
  new series. Replayed on startup to rebuild `label_string ↔ series_id` maps and
  the inverted index. `series_id` is a monotonically increasing counter, not a
  hash — avoids collisions, keeps it a clean 8-byte key everywhere downstream.

### WAL record (fixed-size, 28 bytes, no length prefix)

| Field | Size |
|---|---|
| `series_id` | 8 bytes |
| `timestamp` (unix millis) | 8 bytes |
| `value` (raw IEEE754 double) | 8 bytes |
| `crc32` (over the 3 fields above) | 4 bytes |

Fixed size means recovery is simple: if file size isn't a multiple of 28, the
trailing partial record is a torn write from a mid-append crash — truncate to the
last complete record. No compression in the WAL; it's transient, deleted once its
data is durably flushed.

### Block format (shared by L0 and L1)

```
[Header]        fixed size
[Series Index]  series_count entries, sorted by series_id
[Data Streams]  one Gorilla-encoded bitstream per series
[Footer]        crc32 over everything above
```

**Header**

| Field | Size |
|---|---|
| `magic` (0x4348524E, "CHRN") | 4 bytes |
| `level` (0=L0, 1=L1) | 1 byte |
| `format_version` | 1 byte |
| `series_count` | 4 bytes |
| `min_timestamp` / `max_timestamp` | 8 + 8 bytes |
| `created_at` (wall clock, drives age-based compaction) | 8 bytes |

**Series Index entry** (repeated `series_count` times)

| Field | Size |
|---|---|
| `series_id` | 8 bytes |
| `offset` (into data section) | 8 bytes |
| `byte_length` | 4 bytes |
| `point_count` | 4 bytes |

**L0 data stream** (per series): first point raw (`int64 timestamp` + `double
value`), then each subsequent point as delta-of-delta timestamp + XOR value,
bit-packed per the encoding below.

**L1 data stream** (per series, one entry per rollup bucket): `bucket_start`
(delta-of-delta encoded — buckets are evenly spaced, compresses to near-zero same
as L0), `count` (raw `uint32`), then `min/max/avg/p99` as four raw `double`s.
**Deliberate simplification**: don't XOR-compress the L1 statistics — L1 volume
is already a fraction of L0's, so the compression payoff is small relative to the
implementation cost of four parallel XOR streams. State this as a scoping choice
in the writeup, not an oversight.

**Footer**: `crc32` over header + index + data, for corruption detection on load.

### Gorilla bit-encoding spec

**Timestamp delta-of-delta**, given `DoD = (t_i - t_{i-1}) - (t_{i-1} - t_{i-2})`:
- `DoD == 0` → write `0`
- `DoD ∈ [-63, 64]` → write `10` + 7-bit signed value
- `DoD ∈ [-255, 256]` → write `110` + 9-bit signed value
- `DoD ∈ [-2047, 2048]` → write `1110` + 12-bit signed value
- else → write `1111` + 32-bit signed value

**Value XOR**, given `XOR = value_i ^ value_{i-1}` (bitwise, on the `double`'s raw bits):
- `XOR == 0` → write `0`
- else, write `1`, then:
  - if the meaningful-bits window (leading/trailing zero count) matches the
    previous nonzero XOR → write `0`, then just the meaningful bits
  - else → write `1`, then 5 bits leading-zero-count + 6 bits meaningful-bit-length,
    then the meaningful bits

Implement the bit writer/reader against this spec first, unit-test it against
known byte sequences, before wiring it into the block writer.

### Crash-safety at the block level: MANIFEST

Compaction deletes old L0 blocks after writing a new L1 block — a crash mid-swap
shouldn't leave neither valid. Keep a `MANIFEST` file listing which block files
are currently live per level. Compaction sequence: write new L1 block → `fsync`
→ write new MANIFEST to a temp file → `fsync` → atomic `rename()` over the old
MANIFEST → then delete the superseded L0 blocks. Crash before the rename means
the old MANIFEST still points at the old L0 blocks — nothing lost, compaction
just redoes on next run.

### Directory layout

```
strata_data/
  wal/000001.wal
  L0/00000001.blk
  L1/00000001.blk
  series_catalog.log
  MANIFEST
```

All integers little-endian, documented explicitly.

---

## Build phases (2-week plan)

**Phase 0 — Setup (few hours)**
CMake/Makefile, `src/`/`tests/`/`bench/` layout, block format decisions locked
(above). Checkpoint: empty project builds, one trivial test runs.

**Phase 1 — WAL + MemTable + flush (Days 1–3)**
WAL append with fsync, MemTable as `std::map<(series_id, timestamp), value>`,
flush to uncompressed L0 blocks on threshold, WAL replay on startup.
Checkpoint: write a burst, kill the process, restart, confirm recovery.

**Phase 2 — Gorilla encoding (Days 4–6)**
Bit writer/reader (test in isolation first), delta-of-delta timestamps, XOR
values, swap flush path to emit Gorilla-encoded L0 blocks.
Checkpoint: encode/decode round-trip is bit-exact; first compression benchmark
(bytes/point vs. naive 16-byte storage).

**Phase 3 — Compaction with downsampling (Days 7–9)**
Background scan of aged L0 blocks, bucket into fixed windows, emit rollup
records to L1, delete source L0 blocks via the MANIFEST swap.
Checkpoint: hand-verify rollup values against a reference; plot storage size
before/after compaction.

**Phase 4 — Inverted index (Days 10–11)**
`unordered_map<label_kv, vector<series_id>>`, updated on flush/compaction,
intersect postings lists for multi-label queries.
Checkpoint: cardinality-scaling benchmark — index size and p50/p99 lookup
latency at 1K/10K/100K/1M unique label combos. This is the headline graph.

**Phase 5 — Query router (Day 12, part 1)**
Route by requested time range to L0/L1/both, stitch when a range spans both.
Checkpoint: query latency at 10 min / 1 day / 90 day ranges, confirm no
unnecessary scans.

**Phase 6 — Formal crash recovery testing (Day 12, part 2)**
Harness that sends `SIGKILL` mid-MemTable-buildup, mid-flush, mid-compaction;
restart and verify no corruption, no loss beyond the last unacknowledged write.

**Phase 7 — Load testing + writeup (Days 13–14)**
Load generator ramping concurrent writers to find the throughput knee. Compile
all benchmark results into the README, explicit implemented-vs-stretch section.

---

## Benchmarking plan

- **Compression**: bytes/point at L0 vs. L1, vs. naive uncompressed (16
  bytes/point) and gzip-on-CSV.
- **Cardinality scaling**: index size + p50/p99 lookup latency from 1K to 1M
  unique label-sets — the single most important graph.
- **Compaction cost & accuracy**: storage footprint over time with/without
  compaction; how much a p99 rollup diverges from the true raw p99 (state this
  tradeoff honestly).
- **Query latency by range**: confirms the router reads only the level(s) it
  needs.
- **Write throughput**: sustained points/sec under increasing concurrent load,
  report the degradation knee.
- **Crash recovery**: kill-point → data loss table (should read "none beyond
  in-flight" at every point).

---

## Interview framing

*"Most from-scratch time-series projects treat compression, compaction, and
indexing as separate features. I built compaction as the transform step of an
ELT pipeline — it doesn't just merge data, it downsamples it — which is closer
to how production systems like M3DB actually handle retention."* Then let the
cardinality-scaling benchmark do the rest of the talking — it's the same curve
production monitoring vendors fight against, and showing exactly where the
system degrades is more convincing than showing that it merely "works."

## Why C++, why this scope

Chosen because it's the strongest language for whoever's building this — removes
the risk of losing time to unfamiliar-language friction while still requiring
real systems work (manual bit-packing, thread-safe compaction, no GC safety net).
Scope was deliberately narrowed from an original wider design (multi-level
L0→L1→L2→L3, B+ tree index) to what's finishable *and defensible* in two weeks:
one compaction level proves the mechanism, an inverted index proves the
cardinality solution and matches what Prometheus actually does, and the B+ tree
becomes an honest stretch goal rather than a rushed, half-understood centerpiece.
