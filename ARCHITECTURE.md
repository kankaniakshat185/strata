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

## Git

The repo is committed per phase as each phase's checkpoint passes, so
`git log` doubles as a build history matching [STRATA_DESIGN.md](STRATA_DESIGN.md)'s
phase plan.
