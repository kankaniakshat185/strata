# Strata

A tiered-resolution time-series storage engine in C++. Raw metrics are
Gorilla-compressed on write; a background compaction pass downsamples
aged data into coarser-resolution rollups (LSM-style leveling); an
inverted label index keeps high-cardinality label queries fast; a query
router picks the right resolution level (or both, stitched) per request.

Full design rationale and the locked on-disk format: [STRATA_DESIGN.md](STRATA_DESIGN.md).
Implementation trade-offs made along the way, phase by phase: [ARCHITECTURE.md](ARCHITECTURE.md).
This file is the front door: what got built, how to run it, and every
benchmark result in one place.

## Quick start

```bash
make test                                  # build everything, run all unit tests
./build/strata_tool write ./data 100000    # write a burst of points
./build/strata_tool recover ./data         # replay + report what's there
./build/strata_tool compact ./data         # L0 -> L1 downsampling
./build/strata_tool bench ./data           # compression ratio
./build/strata_tool cardbench              # cardinality-scaling benchmark
./build/strata_tool query-bench ./data     # query-latency-by-range benchmark
./build/strata_tool loadtest ./data        # concurrent-writer throughput
./tests/phase1_crash_recovery.sh           # timed external-kill recovery test
./tests/phase6_crash_recovery.sh           # deterministic fault-injection recovery tests
```

No external dependencies; `make` uses `clang++ -std=c++20` directly (see
[ARCHITECTURE.md](ARCHITECTURE.md#phase-0--setup) for why Make instead of
CMake). 12 unit test binaries, all passing; 2 shell-driven crash-recovery
harnesses.

## Implemented vs. stretch — final status

**Everything in STRATA_DESIGN.md's "Core" list shipped:**

| Component | Status |
|---|---|
| WAL, MemTable, flush to L0 | Done — [Phase 1](ARCHITECTURE.md#phase-1--wal--memtable--flush) |
| Gorilla compression (delta-of-delta + XOR) | Done — [Phase 2](ARCHITECTURE.md#phase-2--gorilla-encoding) |
| One compaction level (L0→L1), statistical downsampling | Done — [Phase 3](ARCHITECTURE.md#phase-3--compaction-with-downsampling) |
| Inverted label index | Done — [Phase 4](ARCHITECTURE.md#phase-4--inverted-label-index) |
| Query router | Done — [Phase 5](ARCHITECTURE.md#phase-5--query-router) |
| Crash recovery via WAL replay, verified with fault injection | Done — [Phase 6](ARCHITECTURE.md#phase-6--formal-crash-recovery-testing) |

**Stretch goals — not attempted, scope stayed on core:**

- Additional compaction levels (L1→L2→L3). The mechanism (age-triggered
  scan, MANIFEST swap, downsampling) is already generic in
  [compactor.cpp](src/strata/compactor.cpp) — extending it to a second
  level is mostly a matter of parameterizing the source/target level
  rather than new design work, but wasn't done.
- B+ tree index as a comparison point against the inverted index. Skipped
  in favor of spending the time on formal crash-recovery testing
  ([Phase 6](#crash-recovery)) and the load generator ([Phase 7](#write-throughput--the-concurrency-knee)),
  judged more valuable for a storage engine's actual defensibility than a
  second index structure that wouldn't change the cardinality-scaling
  story.

**Beyond the original plan** (added because Phase 5/7 exposed real gaps,
not scope creep for its own sake):

- `Engine` is now genuinely thread-safe (one coarse mutex — see
  [Write throughput](#write-throughput--the-concurrency-knee) below for
  why coarse, not sharded).
- Deterministic fault injection ([fault_injection.hpp](src/strata/fault_injection.hpp))
  for crash testing narrower than an external kill could ever reliably
  hit — and it caught one real bug (a live `Engine`'s MANIFEST going
  stale under out-of-band compaction) before it could ship.

## Benchmarks

All numbers measured on the dev machine; absolute numbers will vary by
hardware, but the *shapes* of these results (compression ratio range,
flat query latency vs. skipped blocks, throughput plateau) are the actual
findings, not the specific microsecond counts.

### Compression

`bytes/point` at L0, vs. naive uncompressed binary (16 B/point) and
gzip on an equivalent CSV — STRATA_DESIGN.md's benchmarking plan calls
for both comparisons:

| Dataset shape | Naive binary | CSV text | gzip -9 on CSV | Gorilla (L0) |
|---|---|---|---|---|
| Regular cadence, cyclical repeat (tool's default `write` pattern, 500K pts) | 16.0 B/pt | 18.0 B/pt | **2.50 B/pt** (7.2x vs CSV) | 3.74 B/pt (4.3x vs naive) |
| Smooth drift, sine-like (test_l0's pattern, 8K pts) | 16.0 B/pt | 24.0 B/pt | **6.80 B/pt** (3.5x vs CSV) | 7.11 B/pt (2.25x vs naive) |

**Honest tradeoff, stated plainly:** gzip beats Gorilla on both shapes
here, more dramatically on the first. That dataset repeats its value
sequence exactly every 100 points (`(i % 100) * 0.1`, chosen for
reproducibility across every benchmark in this project) — gzip's
whole-file lookback window finds that repetition trivially; Gorilla's
single-pass XOR-of-adjacent-values encoding structurally can't, since it
only ever looks at the immediately preceding point. On the smoother,
non-repeating pattern the gap narrows to 7.11 vs 6.80 B/pt — close, with
gzip still slightly ahead. This isn't a case where Gorilla "wins the
benchmark"; the honest comparison is that gzip is a strong general-purpose
batch compressor with a large lookback window, and Gorilla is a
single-pass streaming encoder designed to compress *as data arrives*, one
point at a time, without ever buffering a whole block or making a second
pass — a fundamentally different point in the design space, which is why
production time-series engines (the actual Facebook Gorilla paper this
project is based on) use it despite gzip often winning on raw ratio: it
composes with continuous ingestion in a way a block-oriented batch
compressor doesn't.

### Cardinality scaling — 1K to 1M unique label combinations

The design doc calls this "the single most important graph." Full
methodology and chart:
[ARCHITECTURE.md's Phase 4 notes](ARCHITECTURE.md#phase-4--inverted-label-index).

| N | Distinct label pairs | Index size (est.) | Lookup p50 / p99 | 3-filter intersect p50 / p99 |
|---|---|---|---|---|
| 1,000 | 1,007 | 147 KB | 291 ns / 791 ns | 4.0 µs / 6.9 µs |
| 10,000 | 10,007 | 1.5 MB | 250 ns / 792 ns | 3.2 µs / 4.8 µs |
| 100,000 | 100,007 | 15.2 MB | 417 ns / 916 ns | 3.0 µs / 4.4 µs |
| 1,000,000 | 1,000,007 | 147.0 MB | 500 ns / 1,291 ns | 3.6 µs / **25.5 µs** |

Index size scales linearly with cardinality, as expected for a hash-map
of postings lists. Lookup and intersection latency both stay flat in the
sub-to-low-microsecond range all the way to 1M — the interesting
exception is 3-filter intersection p99 jumping to 25.5µs at 1M, plausibly
tail latency from occasionally landing on the large shared-label postings
lists (`region=us-east` etc. grow to hundreds of thousands of entries)
before the small-list-first intersection ordering narrows things down.
Worth a closer look if this system needed a hard p99 query-latency SLA;
not investigated further here.

### Compaction — storage footprint and rollup accuracy

500,000 points, 5 series → 25 L0 blocks (1,871,975 bytes, Gorilla-encoded)
compacted into a single L1 block: **1,848 bytes, 45 rollup buckets** —
roughly 1,013x smaller. That ratio is dramatic specifically because the
demo data is dense and low-cardinality; real, less-repetitive traffic
won't compress anywhere near that hard, but the mechanism (many raw
points → few statistical summaries) is the same either way.

**p99 divergence — the design doc's honesty check.** L1 stores one p99
value per bucket (nearest-rank over that bucket's points). A query
spanning multiple buckets has no raw points to recompute a true combined
p99 from — the natural approximation is averaging the per-bucket p99s.
Measured against a synthetic latency-like dataset (10,000 points, ~2%
spike rate, the shape that makes p99 actually diverge from the median):

| Bucket granularity | Points/bucket | True range p99 | Avg-of-bucket-p99s | Divergence |
|---|---|---|---|---|
| 2 buckets | 5,000 | 225.10 | 225.23 | 0.1% |
| 5 buckets | 2,000 | 225.10 | 223.57 | 0.7% |
| 10 buckets | 1,000 | 225.10 | 219.73 | 2.4% |
| 50 buckets | 200 | 225.10 | 188.92 | 16.1% |
| 100 buckets | 100 | 225.10 | 162.50 | 27.8% |

The error grows sharply as buckets get smaller, and it's systematically
an *underestimate*, not noise: with only ~2 spikes expected per 100
points, many small buckets end up with zero spikes at all, and a
spike-free bucket's "99th percentile" is just an ordinary baseline value.
Averaging those in with the buckets that do have spikes drags the
combined estimate well below the truth — a real instance of the general
fact that percentiles don't average across sub-populations. Practical
takeaway: pick L1 bucket widths wide enough that each bucket holds enough
points for its own p99 to be meaningful (thousands, not hundreds, for
data with a ~2% tail-event rate), or accept that rollup p99 is a rough
signal for old data, not a number to alert on. A production system
wanting accurate historical percentiles would store a proper streaming
quantile sketch (t-digest, HDR histogram) per bucket instead of one
point estimate — a real design change, not a tuning knob, and out of
scope here.

### Query latency by range — confirming no unnecessary scans

100 synthetic days of data: the most recent 2 days left "hot" in L0,
everything older compacted into one L1 block per day (98 L1 blocks
total). Full methodology: [ARCHITECTURE.md's Phase 5 notes](ARCHITECTURE.md#phase-5--query-router).

| Range | Latency | L0 blocks scanned/skipped | L1 blocks scanned/skipped | Result |
|---|---|---|---|---|
| 10 min | 4.5 ms | 1 / 1 | 0 / 98 | pure L0, all 98 L1 blocks correctly untouched |
| 1 day | 4.3 ms | 1 / 1 | 0 / 98 | still entirely inside the hot L0 window |
| 3 day | 4.4 ms | 2 / 0 | 1 / 97 | genuine stitch — raw points + rollup buckets both returned |
| 90 day | 14.8 ms | 2 / 0 | 88 / 10 | mostly L1, correctly skips the 10 blocks outside the window |

Every range scans exactly the blocks that overlap it and skips the rest,
at every scale from 10 minutes to 90 days — the actual claim this phase
needed to prove.

### Write throughput — the concurrency knee

Ramped 1 → 32 concurrent writer threads against one shared `Engine`,
15,000 points/thread/step, flush suppressed during the run so the
measurement isolates the write path itself rather than flush pauses:

| Threads | Points/sec |
|---|---|
| 1 | 46,500 |
| 2 | 46,042 |
| 4 | 47,917 |
| 8 | 48,296 |
| 16 | 47,956 |
| 32 | 48,920 |

**The knee is at N=1.** Throughput is flat across the entire range (run
to run it varies roughly 37K–57K depending on system load, but never
shows a scaling trend either up or down with thread count). This is the
expected, honest result given the design: `WalWriter::Append` `fsync`s on
every single write, and `Engine` holds one coarse mutex around the whole
write path (added specifically to make this benchmark meaningful and
correct — see [ARCHITECTURE.md's Phase 7 notes](ARCHITECTURE.md#phase-7--load-testing--writeup)
for why coarse rather than sharded). Every writer thread serializes
behind that fsync regardless of how many are contending for it, so
additional concurrency buys nothing — the system is disk-fsync-bound from
a single writer already. The natural next step to push this ceiling
would be WAL group-commit batching (accumulate concurrent writers'
records and fsync once per batch instead of once per record, standard
practice in real databases) — legitimate future work, not attempted here.

### Crash recovery — kill-point → data loss table

Two harnesses: [tests/phase1_crash_recovery.sh](tests/phase1_crash_recovery.sh)
(external timed `SIGKILL` against a live burst write) and
[tests/phase6_crash_recovery.sh](tests/phase6_crash_recovery.sh)
(deterministic fault injection at 5 named code points — see
[ARCHITECTURE.md's Phase 6 notes](ARCHITECTURE.md#phase-6--formal-crash-recovery-testing)
for why timing alone can't reach these windows).

| Kill point | Outcome |
|---|---|
| Random point during a live burst write (Phase 1, external kill) | Partial write recovered exactly (e.g. 20,000 flushed + 4,337 replayed = 24,337/3,000,000), no crash, no corruption |
| Mid-MemTable-buildup, before any flush | All in-flight points recovered from WAL alone |
| Mid-flush, before MANIFEST is updated | Orphaned-but-valid L0 block cleaned up automatically; WAL (untouched) recovers every point — no loss |
| Mid-flush, after MANIFEST is updated, before WAL is cleared | **Known, bounded gap**: points duplicated across two L0 blocks (formally confirmed: 100 points → 200 after recovery) — not lost, not corrupted, exactly the documented behavior |
| Mid-compaction, after L1 is written, before MANIFEST swap | Orphaned L1 block cleaned up; original L0 data untouched; compaction safe to retry |
| Mid-compaction, after MANIFEST swap, before old L0 is deleted | Superseded L0 cleaned up automatically; data already correctly live via the new L1 block |

Every scenario reads "none beyond in-flight" except the one documented
exception, which duplicates rather than loses data and is exactly bounded
as predicted before the test was written — the point of Phase 6 was
proving that prediction, not discovering a surprise.

## Known limitations

- **The flush/WAL-reset duplication gap** (table above) is real and
  unfixed. Nothing currently reads L0 data expecting exactly-once points,
  so it's harmless today; it would need addressing (e.g. a marker file,
  in the spirit of the MANIFEST swap already used for compaction) before
  any consumer relied on exact point counts from unaged data.
- **Compaction runs as a separate, explicit invocation**, not a real
  background thread — and critically, **must not run in the same process
  as a live `Engine` on the same data directory**: a live `Engine` caches
  MANIFEST in memory and doesn't reload it, so an out-of-band compaction's
  swap gets silently reverted on the engine's next flush. This bit the
  Phase 5 query-bench tool during development (see
  [ARCHITECTURE.md](ARCHITECTURE.md#phase-5--query-router)); the fix there
  was procedural (don't do that), not architectural. A real deployment
  would need `Engine` to either own compaction directly or reload MANIFEST
  before trusting it.
- **Write throughput is capped by fsync-per-record + one coarse lock**,
  by design choice, not oversight — see the write-throughput section
  above. Group-commit batching is the natural next step, not implemented.
- **Rollup p99 is a point estimate, not a sketch** — see the compaction
  accuracy table above. Fine as a trend signal for aged data, not
  something to build alerting on without understanding the divergence.

## Interview framing

Most from-scratch time-series projects treat compression, compaction, and
indexing as three separate bolted-on features. This one treats compaction
as the transform step of an ELT pipeline — points land in L0 untransformed
and fast (WAL → MemTable → flush), and the actual statistical work happens
later, off the write path, during compaction — closer to how production
systems like M3DB actually handle retention than a naive "merge sorted
runs" compactor. The cardinality-scaling benchmark is the strongest single
piece of evidence for the design: the same curve production monitoring
vendors fight against, with a concrete number for exactly how flat lookup
latency stays as label cardinality grows from 1K to 1M.

Equally worth defending: what *didn't* get engineered around. Compaction
staying a synchronous, separately-invoked operation instead of a real
background thread; write throughput staying fsync-bound instead of adding
group-commit batching; rollup p99 staying a point estimate instead of a
proper sketch. Each of those was a deliberate scope decision made and
recorded *before* being asked about it, with the actual mechanism (the
part that's hard and worth defending) built correctly rather than the
polish around it — matching STRATA_DESIGN.md's own framing: be explicit
about what's implemented vs. stretch, because that's more credible than
overclaiming.
