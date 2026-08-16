# Strata: a Time-Series Database From Scratch, and Exactly Where My Half Ends

you can't fully trust a bit-level compressor you wrote yourself. your encoder and your decoder were written by the same person, working from the same mental model — so if that mental model has a bug in it, the encoder and the decoder can agree with each other perfectly and both be wrong about reality at the same time. there's no `gorilla-cli` sitting around to diff your output against. the only oracle you have is the code you're trying to verify. that problem, and what it actually takes to get past it, is most of what this post is about.

it's not the only thing this post is about, though, and that's the other half of why i'm writing this. strata is a team project — i built it with [vaishnavi rai](https://github.com/VaishnaviRai287), and that's not a credits-line fact tacked on at the bottom, it's the actual shape of this post. i built strata's core first — the write-ahead log, the memtable, the compression codec, the original compaction path, the crash-safety mechanism, the inverted index, the query router — by myself, and i stand behind every piece of it at the level of rigor i'll walk through below. vaishnavi picked the codebase up after that and did two things to it: she extended single-level compaction into a four-level cascade, and she built a second, completely different index structure specifically to find where mine loses. then she did a third thing, smaller in code but bigger in what it says about her, that i think matters more than either: she went back and checked whether her own "crash safety proven" claim was actually true, and found out it wasn't — quietly, in a way that would have stayed quietly untrue if she hadn't gone looking a second time.

i'm splitting this post cleanly by who actually wrote what, section by section, not because a shared "we" would be false exactly, but because it would be *imprecise* — and imprecision about who's responsible for which claim is exactly the kind of thing this project doesn't let itself get away with anywhere else. so: **mine** is the wal through the query router. **hers** is everything from the compaction cascade onward. i'll say so, every time, as it comes up.

this one goes all the way down to the bit.

---

## what a time-series database is actually for, and why the obvious approach doesn't work

before any of the mechanism, it's worth being precise about the actual problem, because "database for numbers with timestamps" undersells how different this is from a normal database.

a time series is a metric — cpu usage on one server, the temperature of one sensor, the latency of one endpoint — sampled repeatedly over time. `(timestamp, value)` pairs, arriving continuously, forever, from potentially millions of independent sources at once. three things make this genuinely different from, say, a table of user records:

**the write volume never stops and never pauses.** a normal app might write a row when a user clicks a button. a fleet of ten thousand servers each reporting cpu usage every second is ten thousand writes a second, permanently, whether anyone's looking at a dashboard or not. the write path has to be cheap by default, not cheap-when-lucky.

**identifying which series you want is its own hard problem.** a series isn't identified by an id you already know — it's identified by a set of labels: `host=web-3, region=us-east, metric=cpu_usage`. a real fleet has host × region × service × endpoint combinations that can reach millions, and "find every series matching these three label filters" has to stay fast no matter how many series exist. this is not a normal indexing problem — it's closer to search than to a primary-key lookup.

**old data stops being worth its storage cost, but you can't just delete it.** you don't need every individual cpu reading from fourteen months ago at one-second resolution. but you do still want to know "was this server trending up over the last year" — which means you can't just throw the old data away either. you need it to get *cheaper* as it gets *older*, without becoming useless.

the naive approach — store every `(timestamp, value)` pair as sixteen raw bytes (8-byte int64 timestamp, 8-byte double value), index labels by scanning, keep everything forever at full resolution — fails all three at once. sixteen bytes a point sounds small until you multiply it by ten thousand servers times one sample a second times a year. a linear scan over "which series match this filter" is fine at a thousand series and a real liability at a million. and "keep everything forever, uncompressed" is a storage bill that grows without bound while most of what's in it will never be looked at again.

strata's actual design decision — the one every other decision in this post hangs off of — is treating those three problems as one pipeline instead of three separate features bolted on:

```
writers ──> wal (fsync'd) ──> memtable ──> flush ──> l0 (gorilla-compressed, raw resolution)
                                                            │
                                                       compaction  ← mine stops here, at l1
                                                            ▼
                                            l1 (rollups: min/max/avg/count/p99)
                                                            │
                                                  compaction (coarser)  ← this cascade is hers
                                                            ▼
                                                        l2 ──> l3

              inverted index: label filter ──> matching series, flat latency regardless of scale
              b+ tree: a second index, built to answer the one question the inverted index can't  ← hers

  query router: recent range ──> l0 · older range ──> any rollup level · spanning range ──> stitched
```

data lands fast and completely untouched first. the expensive statistical work — collapsing a bucket of raw points into `count, min, max, avg, p99` — happens later, off the write path, as an explicit background pass. that separation is the entire reason the write side can stay cheap and the read side can still be smart, instead of one code path trying to be both and doing neither well.

**what it's not**, up front, honestly: this is single-node, in-memory-indexed, no replication, no sharding, no network protocol. you talk to it through a cli tool linked directly against the engine. the point was never to compete with a real production tsdb — it was to build the actual hard internals of one, end to end, and be honest in public about what each piece costs.

---

# part one: what i built

## stage one: durability before anything else exists to organize

the very first rule strata is built around, the one nothing downstream is allowed to violate: **if the process gets killed the instant after a write returns, that point is not allowed to be gone.** everything else — compression, compaction, indexing — is allowed to be interrupted and resumed. this one thing is not allowed to fail, ever, and that non-negotiability is what a write-ahead log actually buys you.

the mechanism is almost insultingly simple to describe and genuinely fiddly to get exactly right: append every incoming point to a log file, and don't tell the caller the write succeeded until the operating system confirms those bytes are physically on disk, not just sitting in a buffer somewhere waiting to be written later.

a quick grounding in why that even needs saying: a running program never touches a disk directly. hardware access is mediated entirely by the kernel — the one piece of software with that privilege — and a program asks it to do things like "write these bytes to this file" through a system call: a function that looks, in your code, like an ordinary function call (`write()`, `fsync()`), but actually hands control over to the operating system to go do the work. and by default, the kernel doesn't push a `write()`'s bytes to the physical disk right away. writing to ram is thousands of times faster than writing to a spinning disk or even flash storage, so the kernel holds recently-written data in a reserved chunk of memory — the page cache — and flushes it to the actual device later, in a batch, purely for performance. that's the right tradeoff almost everywhere. it's the wrong one for a database that needs to promise a write is safe the instant it's acknowledged.

`fsync()` is the system call that overrides the default: it blocks — your program's execution genuinely pauses right there — until the kernel has actually pushed those specific bytes down to the storage device, not just queued them. a normal `write()` call can return successfully while the data is still only in that page cache, genuinely not on the disk platter or flash cell yet. if the machine loses power at that exact moment, that data is gone, and `write()` told the caller it succeeded anyway. `fsync()` closes that gap. it's slow — call it tens of microseconds to a few milliseconds depending on the device — and strata pays that cost on literally every single write, on purpose, because durability that only applies "most of the time" isn't durability.

each record is a fixed 28 bytes, no length prefix needed because the size never varies:

```cpp
struct WalRecord {
  uint64_t series_id;
  int64_t timestamp;  // unix millis
  double value;
};

inline constexpr size_t kWalRecordSize = 28;  // 8 + 8 + 8 + 4-byte crc32
```

`series_id` is 8 bytes, `timestamp` is 8, `value` is 8 — that's 24 bytes of actual payload — plus a 4-byte crc32 checksum computed over those 24 bytes, appended at the end:

```cpp
void WalWriter::Append(const WalRecord& record) {
  uint8_t buf[kWalRecordSize];
  io::PutU64(buf, record.series_id);
  io::PutI64(buf + 8, record.timestamp);
  io::PutDouble(buf + 16, record.value);
  uint32_t crc = Crc32(buf, 24);
  io::PutU32(buf + 24, crc);

  ssize_t n = ::write(fd_, buf, kWalRecordSize);
  if (n != static_cast<ssize_t>(kWalRecordSize)) {
    throw std::runtime_error("WalWriter: short write");
  }
  if (::fsync(fd_) != 0) {
    throw std::runtime_error(std::string("WalWriter: fsync failed: ") + std::strerror(errno));
  }
}
```

(`fd_` above is a file descriptor — a small integer the kernel hands back when a file gets opened, and the only thing the rest of the program needs afterward to refer to that open file in every later syscall; strata never touches a file's actual bytes except through calls shaped like this one.)

a checksum, in case the concept is new: it's a small number computed from a block of data such that changing even a single bit of that data is overwhelmingly likely to change the number you'd recompute from it. store the checksum alongside the data, recompute it on read, compare the two — that's a cheap way to detect corruption without needing some separate trusted copy sitting elsewhere to compare against. the checksum earns its keep on the replay side here, not the write side. if the process dies mid-`write()` — not mid-`fsync`, mid the actual `write()` syscall itself, which the os is free to do partially — you can end up with a file whose last record is a torn fragment: 11 bytes of a 28-byte record, say, sitting at the end of the file. replay has to detect that and refuse to trust it:

```cpp
void ReplayWal(const std::string& path, const std::function<void(const WalRecord&)>& on_record) {
  // ...
  off_t good_offset = 0;
  while (good_offset + static_cast<off_t>(kWalRecordSize) <= st.st_size) {
    ssize_t n = ::pread(fd, buf, kWalRecordSize, good_offset);
    if (n != static_cast<ssize_t>(kWalRecordSize)) break;

    uint32_t stored_crc = io::GetU32(buf + 24);
    uint32_t actual_crc = Crc32(buf, 24);
    if (stored_crc != actual_crc) break;  // torn write / corruption boundary

    on_record(BuildRecord(buf));
    good_offset += static_cast<off_t>(kWalRecordSize);
  }
  if (good_offset != st.st_size) ::ftruncate(fd, good_offset);  // drop the torn tail
}
```

two separate checks are doing two separate jobs here, and it's worth being precise about which is which. `if (n != kWalRecordSize) break` catches the case where the *file itself* is shorter than a full record — the trailing 11-byte fragment case, a straightforward torn write. the crc check catches something subtler: a record that's the *right length* but has wrong bytes in it, which can happen if a crash lands in a way that leaves stale or partially-flushed data sitting where a real record should be. length alone wouldn't catch that; length-plus-checksum does. once replay hits either failure, it stops trusting anything from that point on and truncates the file there, so the next append starts on a clean boundary instead of appending after garbage.

**the honest cost:** fsync-per-write is the reason strata's write throughput has a hard ceiling — i measured it flat around 30,000-33,000 points/second regardless of how many threads — independent, concurrently-running paths of execution, the mechanism a program uses to genuinely do more than one thing at once on a multi-core machine instead of strictly one-after-another — are writing at once (1, 2, 4, 8, 16, 32 — practically identical), which is exactly what you'd expect when every write serializes through one lock (a mutex: a simple mechanism that lets only one thread run a given piece of code at a time, with every other thread that wants in forced to wait its turn — that queueing is literally what "serializes" means here) and then blocks on physical disk confirmation. the standard fix for this — batching multiple writers' records into one shared fsync, "group commit" — is a well-understood, well-scoped next step that i deliberately didn't build. it's a real architectural change (writers need to coordinate on a batch boundary instead of each fsyncing independently), not a small patch, and it wasn't worth the time against a project that already proves durability-first design end to end. that ceiling is honest, not an oversight.

## stage two: a buffer that's fast to write and cheap to sort

once a point is durably in the wal, it goes into an in-memory sorted buffer, the memtable:

```cpp
class MemTable {
 public:
  using Key = std::pair<uint64_t, int64_t>;  // (series_id, timestamp)
  void Insert(uint64_t series_id, int64_t timestamp, double value) {
    points_[{series_id, timestamp}] = value;
  }
 private:
  std::map<Key, double> points_;
};
```

`std::map` is a red-black tree — a self-balancing binary search tree that keeps itself roughly even-depth as data gets inserted, which is the actual mechanism behind its O(log n) guarantee holding even in the worst case, not just on a lucky average. insertion costs O(log n), which sounds abstract until you translate it plainly: the cost only grows by one extra comparison each time the *total data size doubles*, not each time it grows by one. a tree holding a million entries needs on the order of twenty comparisons to place a new one, not a million. and critically, iterating it front-to-back gives you every point already sorted by `(series_id, timestamp)`, for free, no separate sort pass needed when it's time to flush. that sort order is exactly what the l0 writer needs: it walks the memtable once, linearly, and every series' points are already contiguous and already in timestamp order, ready to hand straight to the compressor. a second write to the same `(series_id, timestamp)` key just overwrites the value at that key — last-write-wins, which is a deliberate, simple choice for what happens if the exact same timestamp gets written twice.

when the memtable crosses a configurable size threshold, it gets flushed: written out as an immutable, compressed l0 block, and the wal segment behind it gets retired. that's the entire ingest path, three steps: append, buffer, flush.

## stage three: what actually lands on disk when a block gets written

one layer up from the codec itself: a block isn't just "a pile of compressed bytes." it's a small, deliberately plain file format — header, then an index, then the compressed data, then a checksum — and the shape of that format is exactly what makes the query router's "skip it without opening it" trick possible much later in this post. worth explaining now, because the codec and the router both either write into this shape or read out of it.

```cpp
struct BlockHeader {
  uint32_t magic;         // 0x4348524E ("CHRN") -- a sanity check this is really a strata block
  uint8_t level;           // 0 = raw l0, 1/2/3 = which rollup level
  uint8_t format_version;
  uint32_t series_count;
  int64_t min_timestamp;   // computed once, at write time, over every point in the block
  int64_t max_timestamp;
  int64_t created_at;      // wall clock -- drives age-based compaction eligibility later
};

struct SeriesIndexEntry {
  uint64_t series_id;
  uint64_t offset;         // this series' compressed bytes start here, inside the data section
  uint32_t byte_length;
  uint32_t point_count;
};
```

a block on disk is four regions, laid out back to back. a fixed **header** — always exactly 34 bytes (4+1+1+4+8+8+8), never more, never less. then one **series index entry** per series in the block, 24 bytes each (8+8+4+4). then the **data section** — every series' gorilla-encoded bitstream, one after another. then a **footer**: one crc32 checksum computed over literally everything that came before it in the file.

why this shape, specifically, instead of one undifferentiated blob of compressed bytes: `min_timestamp` and `max_timestamp` get computed exactly once, when the block is written, over every point going into it — and then they just sit there, at a fixed, always-known byte position in the file, for as long as the block exists. that means answering "could this block possibly matter for this query's time range" costs reading 34 fixed bytes and comparing two integers — never touching the series index, never touching the data section, never running the decompressor. and the series index — offset, byte length, point count, per series — turns "where does series 42's data actually live inside this block" into a lookup in a small fixed-size table, not a scan through compressed bytes hunting for a marker. the header answers "does this block matter at all." the index answers "where, exactly, if it does." decoding only happens after both of those cheap questions come back yes.

the header's `magic` field is a fixed constant every real strata block starts with — the first four bytes read back are checked against it before anything else in the file is trusted, so something that isn't actually a strata block, or one that's corrupted right at the start, fails loudly and immediately instead of getting fed further into code that assumes a shape it doesn't actually have.

## the compression codec: gorilla, worked from the actual bit level

storing every point as a raw 16-byte pair is correct and wasteful — most real metrics barely move from one sample to the next. strata implements **gorilla encoding**, the algorithm behind facebook's in-memory time-series database, entirely by hand: two tricks, one for timestamps and one for values, both exploiting the same underlying idea — encode the *difference* from the last point, and spend almost no bits when that difference is small or exactly repeats.

before either trick makes sense, it's worth being exact about what a `double` actually looks like as bits, because the value-compression trick only makes sense once you can see it. ieee-754 double-precision floating point packs a number into 64 bits: 1 sign bit (positive or negative), 11 exponent bits (roughly, the number's scale — how far to shift the decimal point), and 52 mantissa bits (the actual significant digits). two numbers that are numerically close almost always share the same scale and most of the same leading digits, which is exactly why their bit patterns overlap so heavily — that overlap is the entire reason xor works as a compression trick here. `65.0` is `0x4050400000000000` in that layout. `65.125` — barely different numerically — is `0x4050480000000000`. xor (exclusive or) is a bit-by-bit comparison that outputs a 1 only where two bits *differ*, and a 0 everywhere they match — apply it across two 64-bit numbers, position by position, and every position where the two numbers agree collapses to a zero. xor those two values together and almost everything cancels: the sign bit matches, the exponent matches, most of the mantissa matches, and you're left with a handful of bits in the middle that actually changed. that's the entire insight the value codec is built on.

### timestamps: encode how much the gap changed, not the gap itself

metrics tend to arrive on a regular cadence — every second, every five minutes. so instead of storing each timestamp, or even the gap between consecutive timestamps, strata stores **how much the gap changed** from the previous gap:

```
delta_i = t_i - t_{i-1}
DoD_i   = delta_i - delta_{i-1}
```

for perfectly regular data, that's zero, over and over. `DoD` (delta-of-delta) gets a variable-length code where the smaller it is, the fewer bits it costs — this is the actual encoding function, unabridged:

```cpp
void EncodeTimestampDelta(BitWriter& bw, int64_t dod) {
  if (dod == 0) {
    bw.WriteBit(false);
  } else if (dod >= -63 && dod <= 64) {
    bw.WriteBits(0b10, 2);
    bw.WriteBits(EncodeDodField(dod, 7), 7);
  } else if (dod >= -255 && dod <= 256) {
    bw.WriteBits(0b110, 3);
    bw.WriteBits(EncodeDodField(dod, 9), 9);
  } else if (dod >= -2047 && dod <= 2048) {
    bw.WriteBits(0b1110, 4);
    bw.WriteBits(EncodeDodField(dod, 12), 12);
  } else {
    bw.WriteBits(0b1111, 4);
    bw.WriteBits(EncodeDodField(dod, 32), 32);
  }
}
```

five buckets, cheapest first: exactly zero costs one bit (`0`). a small change costs a 2-bit tag plus 7 bits (9 total). bigger changes cost progressively more tag bits plus a wider field, down to a 36-bit fallback for anything wild. `BitWriter::WriteBits` is the primitive everything sits on top of — it writes the low n bits of a value one at a time, most-significant-bit first, packing them into a byte buffer regardless of byte boundaries:

```cpp
void WriteBits(uint64_t value, int nbits) {
  for (int i = nbits - 1; i >= 0; --i) {
    WriteBit((value >> i) & 1u);
  }
}
```

there's a genuinely subtle piece of code hiding in `EncodeDodField`, worth actually stopping on instead of skipping past:

```cpp
uint64_t EncodeDodField(int64_t dod, int nbits) {
  int64_t max_val = int64_t(1) << (nbits - 1);
  if (dod == max_val) return 0;
  uint64_t mask = (nbits == 64) ? ~0ULL : ((uint64_t(1) << nbits) - 1);
  return uint64_t(dod) & mask;
}
```

look at the range table again: the 7-bit field is documented as covering `-63 to 64`. a signed n-bit two's-complement field can only naturally represent `-64` to `63` — that's what n bits *means* in two's complement. `64` is one value past what the field can hold. the `dod == 0` case is handled entirely separately by the caller with its own single bit, so the all-zero bit pattern inside this 7-bit field is never needed to mean "zero" here — and rather than let that bit pattern go to waste, the code repurposes it: `dod == max_val` (the one value that's genuinely out of range for a plain n-bit signed field) gets encoded as all-zero-bits, and the decoder reverses this exact mapping:

```cpp
int64_t DecodeDodField(uint64_t code, int nbits) {
  if (code == 0) return int64_t(1) << (nbits - 1);
  uint64_t sign_bit = uint64_t(1) << (nbits - 1);
  if (code & sign_bit) {
    uint64_t ext_mask = (nbits == 64) ? 0 : ~((uint64_t(1) << nbits) - 1);
    return int64_t(code | ext_mask);
  }
  return int64_t(code);
}
```

`code == 0` decodes straight back to `max_val`. anything else goes through ordinary sign extension — if the top bit of the n-bit field is set, the value is negative, so the decoder fills every higher bit with 1s (`ext_mask`) to reconstruct the correct negative 64-bit integer; otherwise it's positive and the bits are the value as-is. one reused bit pattern, one extra value squeezed out of every range the table advertises, applied uniformly all the way up to the 32-bit fallback (where it harmlessly just extends that range by one value nobody will ever hit in practice).

worked trace, five points arriving roughly every 1000ms with one 5ms hiccup at the end:

| i | timestamp | delta | dod | encoding | cost |
|---|---|---|---|---|---|
| 0 | 1000 | — | — | raw 64-bit | 64 bits |
| 1 | 2000 | 1000 | 1000 | `1110` + 12-bit(1000) | 16 bits |
| 2 | 3000 | 1000 | 0 | `0` | **1 bit** |
| 3 | 4000 | 1000 | 0 | `0` | **1 bit** |
| 4 | 5005 | 1005 | 5 | `10` + 7-bit(5) | 9 bits |

after the unavoidable first point, four more timestamps cost 16 + 1 + 1 + 9 = **27 bits total**, versus 256 bits if each were stored raw. regular cadence is the overwhelmingly common case in real metrics, and it's exactly the case this encoding was built for.

### values: xor against the previous point, then describe only what changed

the encoder writes one bit if the xor between this value and the last one is exactly zero — value repeated exactly, done, one bit. otherwise it writes a `1`, then has a choice to make about how to describe the nonzero bits:

```cpp
void EncodeValueXor(BitWriter& bw, uint64_t xor_bits, int& prev_leading, int& prev_len) {
  if (xor_bits == 0) {
    bw.WriteBit(false);
    return;
  }
  bw.WriteBit(true);

  int leading = std::min(__builtin_clzll(xor_bits), 31);
  int trailing = __builtin_ctzll(xor_bits);
  int len = 64 - leading - trailing;

  if (prev_len >= 0 && leading == prev_leading && len == prev_len) {
    bw.WriteBit(false);
    bw.WriteBits(xor_bits >> trailing, len);
  } else {
    bw.WriteBit(true);
    bw.WriteBits(uint64_t(leading), 5);
    bw.WriteBits(uint64_t(len - 1), 6);
    bw.WriteBits(xor_bits >> trailing, len);
    prev_leading = leading;
    prev_len = len;
  }
}
```

`__builtin_clzll` counts leading zero bits, `__builtin_ctzll` counts trailing zero bits — both compile to a single hardware instruction on essentially every modern cpu. together they bracket the "meaningful window": the span of bits, from the first 1-bit to the last 1-bit, that actually changed. everything outside that window is zero and doesn't need to be written at all.

now the actual decision: if this xor's window (leading count and length) is identical to the *previous nonzero xor's* window, the encoder writes a single `0` bit and then just the meaningful bits — it doesn't need to re-describe where the window is, because it's the same place it was last time. that's the common case for a value drifting steadily within the same part of its mantissa. if the window moved, it writes a `1` bit, then 5 bits for the new leading-zero count, 6 bits for the window length minus one (so a length anywhere from 1 to 64 fits in a 6-bit field), then the meaningful bits themselves.

worked trace, real ieee-754 bit patterns:

| value | bit pattern (hex) | xor vs. previous |
|---|---|---|
| 65.0 | `0x4050400000000000` | — (first point, stored raw) |
| 65.0 | `0x4050400000000000` | `0x0000000000000000` — identical, **1 bit** |
| 65.125 | `0x4050480000000000` | 1 bit differs, new window |
| 65.125 | `0x4050480000000000` | identical again — **1 bit** |
| 70.5 | `0x4051a00000000000` | 6 bits differ, window moved — new window |

`65.0 → 65.0` costs 1 bit. `65.125 → 65.125` costs 1 bit. only the two points where the value actually jumped pay the full cost of describing which bits changed and where.

### why not just gzip everything

fair question, and the honest answer is that on some data shapes, gzip genuinely compresses tighter than this hand-built codec. they solve different problems. gzip is a **batch** compressor — it needs the whole block in hand to go find repeated patterns anywhere inside it. gorilla is a **streaming** compressor — it encodes each point the instant it arrives, using only the single point directly before it, no buffering, no second pass. that's what lets compression happen *as data is ingested*, on the write path, instead of as a separate pass after the fact. it's a deliberate tradeoff for a specific property, not a missed opportunity to just call `zlib`.

real numbers, measured myself just now against a 16-bytes-per-point naive baseline, not pulled from an old readme: a fresh 100,000-point write across 5 series came out to **3.74 bytes/point, 4.28x smaller than naive**, on regular-cadence data. the existing smooth-drift unit test (`test_l0.cpp`) reports **7.11 bytes/point, 2.25x smaller**, on data that changes a little every point instead of repeating — a genuinely harder case for a codec that's betting on repetition, and it still holds up at over 2x.

## trusting a codec with nothing external to check it against

here's the part i actually think matters most about what i built, more than any of the mechanism above.

when you write a parser, you can find a reference implementation and diff your output against it. when you compress with a well-known format, you can decompress with a standard tool and confirm you get the same bytes back. hand-rolling a bit-level codec doesn't give you that luxury. there's no ground truth sitting outside my own code. the only oracle i had was my own decoder — written by the same person who might have gotten the encoder wrong in a way that happens to be internally consistent. a bug where the encoder and decoder agree with each other but disagree with reality is the worst kind, because round-trip tests *pass*. the corruption is silent.

so the actual engineering discipline here wasn't the bit-packing itself — it was refusing to trust it by default. every value that goes through the codec in the test suite gets compared bit-for-bit against the original on the way back out, using the raw ieee-754 representation, not a floating-point equality check:

```cpp
bool BitExactEqual(double a, double b) {
  uint64_t ba, bb;
  std::memcpy(&ba, &a, 8);
  std::memcpy(&bb, &b, 8);
  return ba == bb;
}
```

that helper is doing more than it looks like. an ordinary `a == b` on two doubles would still pass even if the codec silently normalized a `-0.0` to `0.0`, or flushed a subnormal number to zero, or committed any of a dozen other bit-level sins that are numerically invisible in a `==` check but would be a real corruption of what was actually written. comparing raw 64-bit patterns instead of comparing floating-point values closes that whole class of "looks fine, isn't" bug before it can hide anywhere.

the other half of building trust was working the encoding out by hand before trusting the code to do it — the delta-of-delta table and the xor window table above aren't illustrative filler, they're a real trace, worked digit by digit against what the actual code emits, specifically so a mismatch between "what i think this should encode to" and "what it actually encodes to" would show up as a wrong number in a table i was staring directly at, not as a mysterious failure three files away pointing vaguely at "something in the codec." getting the leading-zero/trailing-zero window-reuse logic right — deciding when to reuse the previous xor's window versus paying to describe a new one — only became something i actually trusted once it had survived both checks: a hand-traced example first, and a bit-exact round trip on real data second. a codec that's merely self-consistent (round-trips against itself) and a codec that merely matches my mental model on one hand-picked example are both individually dangerous. neither one alone would have caught everything. together, they're the reason i can put "4.28x smaller" in this post as a measured fact instead of a number i hoped was right.

## turning points into statistics: compaction, and the swap that has to survive a crash

a background pass scans l0 blocks old enough to be eligible, and for each series, groups its points into fixed-width time buckets — one bucket per hour, say. every bucket gets replaced with one row: `count, min, max, avg, p99`. this is the actual downsampling mechanism:

```cpp
double Percentile(std::vector<double>& values, double p) {
  std::sort(values.begin(), values.end());
  size_t n = values.size();
  size_t rank = static_cast<size_t>(std::ceil(p * double(n)));
  if (rank < 1) rank = 1;
  if (rank > n) rank = n;
  return values[rank - 1];
}

RollupBucket Downsample(int64_t bucket_start, std::vector<double>& values) {
  RollupBucket b;
  b.bucket_start = bucket_start;
  b.count = static_cast<uint32_t>(values.size());
  b.min = *std::min_element(values.begin(), values.end());
  b.max = *std::max_element(values.begin(), values.end());
  double sum = 0.0;
  for (double v : values) sum += v;
  b.avg = sum / double(values.size());
  b.p99 = Percentile(values, 0.99);
  return b;
}
```

`p99` here is the **nearest-rank** percentile: sort the bucket's values, take the value sitting at rank `ceil(0.99 * n)`, counting from 1. for a five-value bucket, that rank is 5 — the max. for a hundred-value bucket, it's rank 99 — genuinely distinct from the max, which is exactly the case the unit test exercises to prove the two don't collapse into the same thing by accident.

**the honest tradeoff, stated plainly:** once a bucket is written, the raw points behind it are gone. count, min, max, and avg all recombine *exactly* if you ever need to merge several buckets — sum the counts, take the extremes, and a count-weighted mean of means equals the true mean, no information lost. p99 does not recombine this way under any linear formula, because a percentile isn't a sum — there's no way to reconstruct a merged 99th-percentile from a handful of smaller buckets' own 99th-percentiles without the original points. i knew this conceptually the whole time i was building single-level compaction, and i said so in the docs. what i didn't do, and vaishnavi did do later — i'll get to it — is build an actual test that puts an exact number on exactly how bad that approximation gets. i stopped at "this is a known, real limitation." she's the one who measured it.

**making the l0-to-l1 swap crash-safe is the actual hard part here**, more than the statistics. compaction writes a brand-new l1 block and then needs to retire the l0 blocks it just summarized — two separate file operations that cannot happen atomically on their own. a crash landing between them could leave the system trusting neither the old data nor the new. the fix rests on one filesystem guarantee: `rename()` is atomic. a `rename()` either fully lands or fully doesn't — there's no partial, torn state a crash can leave it in, unlike two independent writes. why rename gets to make that promise when a plain write doesn't: renaming a file never touches the file's actual content at all. a filesystem keeps something like a small lookup table mapping names to where their underlying data actually lives, and renaming is nothing more than updating one entry in that table — pointing a name at different data, or overwriting what an existing name already points to. that's one small metadata change, and filesystems are built to perform it as a single indivisible step. writing a file's actual content, by contrast, can be many separate operations under the hood, and a crash is free to land in the middle of any of them.

```cpp
void WriteManifestAtomic(const std::string& path, const Manifest& manifest) {
  std::string tmp_path = path + ".tmp";
  // ... serialize manifest content to tmp_path ...
  if (::fsync(fd) != 0) { /* ... */ }
  ::close(fd);
  if (::rename(tmp_path.c_str(), path.c_str()) != 0) { /* ... */ }
}
```

write the new l1 block, `fsync` it. write the new manifest — a small text file listing every block currently considered live — to a temp path, `fsync` that too. then `rename()` the temp file over the real `MANIFEST` path, one atomic operation. only after that rename succeeds do the superseded l0 blocks actually get deleted. a crash before the rename leaves the *old* manifest in place, still correctly pointing at the still-valid old l0 data — the new l1 block sits there orphaned but harmless, and startup cleanup deletes it. a crash after the rename leaves old l0 blocks sitting on disk that the manifest no longer lists — also harmless, also swept up on the next startup.

the actual engineering discipline again wasn't the mechanism — it was proving it, not assuming it. i named five exact points in the code where a crash is genuinely dangerous — the moment right after the l0 block is fsynced but before the manifest knows about it; the moment right after the manifest is updated but before the wal is cleared; the same two moments on the compaction side — and wired a real self-inflicted `SIGKILL` into each one:

```cpp
void MaybeCrash(const char* point) {
  const char* target = std::getenv("STRATA_CRASH_AT");
  if (target == nullptr || std::strcmp(target, point) != 0) return;
  ::kill(::getpid(), SIGKILL);
  for (;;) {}  // unreachable: SIGKILL can't be caught, blocked, or ignored
}
```

a signal, in operating-system terms, is a limited kind of message the kernel can deliver to a running process — often a request ("please pause," "please terminate") that the process is free to intercept and handle with its own cleanup code before anything actually happens. `SIGKILL` is the one signal that isn't a request. the kernel doesn't ask — it terminates the process immediately, no handler gets to run, no chance to close a file or finish a write in progress. that's exactly the property needed here: a real crash, someone pulling the power cord, doesn't ask nicely either, and a test that let the process clean up gracefully first wouldn't actually be testing what a real crash does.

the reason this needs to be a self-inflicted kill rather than an external process racing a timer against the target: the crash windows worth testing are a few statements wide, sub-millisecond. an external `kill -9` (9 being `SIGKILL`'s numeric id — the same signal, just sent from a shell instead of from inside the program) has no way to reliably land inside a window that narrow — it's a coin flip whether the timing lines up at all. instrumenting the exact point and having the process kill itself on cue, keyed by an environment variable, is the standard technique for this (rocksdb calls it syncpoints; tikv and etcd call theirs fail-points), and it's the only way to get a deterministic, repeatable test out of a race condition that would otherwise be nearly impossible to hit on purpose.

strata ships two separate crash-recovery harnesses for two different kinds of confidence. one (`phase1_crash_recovery.sh`) is a realistic external timed kill — start a write burst, `SIGKILL` it from outside at an unpredictable moment, confirm recovery — proving the system survives a crash that looks like what would actually happen on a real machine. the other (`phase6_crash_recovery.sh`) is the deterministic version above, five named points, each checked against an *exact* expected recovery outcome — including one scenario that's supposed to reproduce an already-documented gap (a crash between the manifest update and the wal cleanup genuinely does duplicate a batch of points on replay, and the test confirms that's exactly what happens, bounded and known, rather than something worse).

## finding series without scanning everything: the inverted index

a series is identified by its labels, and real label combinations get into the millions fast. scanning every series to check which ones match a filter doesn't scale, so strata builds a hash map — a structure that turns a key into a number (a hash) and uses that number to jump almost directly to the right storage slot, instead of checking keys one at a time the way a plain list would have to — from each individual `key=value` pair to the sorted list of series carrying it — not from the label *key* alone, which would barely narrow anything down (nearly every series has *a* `host`; only `host=web-3` actually filters):

```cpp
void InvertedIndex::AddSeries(uint64_t series_id, const std::string& canonical_labels) {
  // splits "host=h1,metric=cpu,region=us-east" on commas, appends
  // series_id to each individual pair's postings list
}

std::vector<uint64_t> InvertedIndex::IntersectQuery(const std::vector<std::string>& label_kvs) const {
  // ... look up each matcher's postings list ...
  std::sort(lists.begin(), lists.end(),
            [](const auto* a, const auto* b) { return a->size() < b->size(); });
  std::vector<uint64_t> result = *lists[0];
  for (size_t i = 1; i < lists.size() && !result.empty(); ++i) {
    std::vector<uint64_t> next;
    std::set_intersection(result.begin(), result.end(), lists[i]->begin(), lists[i]->end(),
                           std::back_inserter(next));
    result = std::move(next);
  }
  return result;
}
```

a query with several filters intersects their postings lists, and the sort before the loop is the entire reason this stays fast at scale: it intersects starting from the *smallest* list first. filtering `host=web-3` (one series) against `region=us-east` (possibly hundreds of thousands) by starting with the one-entry list means the huge list barely gets touched — `set_intersection` on a 1-element list against anything terminates almost immediately. without that ordering, a filter that happened to include a common label would degrade linearly with total series count, no matter how selective the *other* filter was.

fresh numbers, measured just now, not carried over from an old draft:

| unique label combos | index size | lookup latency (p50 / p99) |
|---|---|---|
| 1,000 | 147 KB | 209 ns / 625 ns |
| 10,000 | 1.5 MB | 291 ns / 709 ns |
| 100,000 | 15.2 MB | 416 ns / 750 ns |
| 1,000,000 | 147 MB | 541 ns / 1,500 ns |

index size grows linearly with series count, exactly as expected for a hash map. lookup latency barely moves at all — roughly 2.5x from 1,000 series to 1,000,000, a thousand-fold increase in scale. that flatness *is* the result: it's what a hash map with an O(1) average lookup should do, and it's the reason a filter on a fleet of a million hosts costs about the same as a filter on a fleet of a thousand.

## the query router: skip a block without ever opening it

given a label filter and a time range, the router does three things, strictly in this order: resolve the filter to matching series via the inverted index, *before* touching any actual time-series data on disk. then, for every block at every level, check its stored min/max timestamp — kept right in the block's fixed-size header — to decide, without ever opening or decoding the block's actual data, whether it could possibly overlap the requested range:

```cpp
bool Overlaps(int64_t block_min, int64_t block_max, int64_t start_ms, int64_t end_ms) {
  // query range is half-open [start_ms, end_ms); block range is closed [block_min, block_max]
  return block_min < end_ms && block_max >= start_ms;
}
```

only blocks that pass that check get actually read and decoded. a block whose range doesn't overlap is skipped based on two integers read out of its header — never opened for real, never decompressed.

fresh numbers from a real run just now, against 100 days of data with the last two days left "hot" in l0 and everything older compacted into l1 blocks:

| range requested | blocks touched | blocks correctly skipped |
|---|---|---|
| 10 minutes | 1 | 99 |
| 3 days (spans both resolutions) | 3 | 97 |
| 90 days | 90 | 10 |

this reproduces exactly — not approximately, exactly — the same 1/99, 3/97, 90/10 split i first measured when i built the router, which makes sense: it's a deterministic count, not a timing measurement, so it can't drift the way latency numbers do. a 10-minute query against months of history touches one block. one honest nuance worth being precise about, because it's easy to overclaim: the router still opens every *candidate* block's header to check the overlap, even the ones it ends up skipping — the "skip" is cheap (one small file read, not a full decode), but it isn't free. what scales with total history isn't decode cost, it's a small, fixed per-block header check, and that distinction matters if you're reasoning precisely about where the cost actually goes instead of just citing the headline "1 block touched."

---

# part two: what vaishnavi built

that's the entirety of what i built and verified myself: wal, memtable, the gorilla codec (worked and trusted at the bit level), single-level compaction with an honestly-stated but unmeasured p99 caveat, five named crash points proven with self-inflicted `SIGKILL`s, the inverted index, and the query router. everything above this line is mine to stand behind.

vaishnavi picked the codebase up after that. what she added isn't a patch on top of what i built — it's a genuine extension of the same mechanisms, done with the same standard of "prove it, don't assert it" i tried to hold myself to above. i want to be exactly as precise crediting her work as i was being precise about my own, so: everything from here down is hers, and i'm walking through it at the same code level of detail as my own half, not summarizing it more thinly just because i didn't write it.

## extending compaction from one hop to a cascade

single-level compaction (mine) turns raw l0 points into l1 hourly rollups and stops there. that's fine for weeks of retention. it stops being fine once you're keeping years of data — hourly buckets for three years is still a lot of rows. vaishnavi's fix was to make compaction recursive: l1's hourly buckets age into l2 daily buckets, and l2's daily buckets age into l3 monthly buckets, the same merge logic reapplied one level up each time.

the interesting part is that merging *summaries* needs different math than merging *raw points*, and she wrote the function that handles it explicitly rather than reusing the original `Downsample`:

```cpp
RollupBucket MergeRollupBuckets(int64_t bucket_start, const std::vector<RollupBucket>& sources) {
  RollupBucket merged;
  merged.bucket_start = bucket_start;

  uint64_t total_count = 0;
  double min_v = sources[0].min, max_v = sources[0].max;
  double weighted_avg_sum = 0.0, weighted_p99_sum = 0.0;
  for (const auto& s : sources) {
    total_count += s.count;
    if (s.min < min_v) min_v = s.min;
    if (s.max > max_v) max_v = s.max;
    weighted_avg_sum += s.avg * double(s.count);
    weighted_p99_sum += s.p99 * double(s.count);
  }

  merged.count = static_cast<uint32_t>(total_count);
  merged.min = min_v;
  merged.max = max_v;
  merged.avg = weighted_avg_sum / double(total_count);
  merged.p99 = weighted_p99_sum / double(total_count);  // approximation -- see below
  return merged;
}
```

`count` sums cleanly. `min`/`max` take the extremes across every source bucket, which is exact — the true min of a merged range genuinely is the smallest of the sub-ranges' mins. `avg` is a count-weighted mean of means, which is mathematically exact: if you know each sub-bucket's average and count, the true overall average really is `sum(avg_i * count_i) / sum(count_i)`, no information lost.

`p99` uses the identical formula — a count-weighted mean of the source p99s — and this is where she stopped and actually quantified something i'd only ever described in words. a percentile isn't a sum, so this formula is not a reconstruction of the true merged p99, it's a plausible-sounding approximation that happens to use math that's easy to explain and consistent with how `avg` is combined. the failure mode is structural, not a rounding error: a small bucket's own p99 is nearly always sitting right next to its own max, because nearest-rank on a handful of values almost always lands on the highest one. average a bunch of small buckets' near-maxima together, weighted by their small counts, and the result drifts toward the *plain mean of everything*, not toward the true 99th percentile.

she built a test specifically to put an exact, hand-computable number on this instead of leaving it as a plausible-sounding claim — 100 raw points, 98 of them a flat baseline value and 2 real spikes, compacted into 20 narrow buckets (5 points each, so exactly 2 of the 20 buckets each happen to contain one spike), then rolled up one more level. i ran this test myself, live, rather than trust the number from an old draft:

```
test_rollup_compaction: p99 bias -- true=1000.0 merged(L1->L2)=109.0
(89.1% divergence from 20 narrow source buckets)
```

the true p99 across all 100 raw points is 1000 (the spike value — rank 99 of 100 still lands on a spike). the merged estimate, built purely from the 20 buckets' own p99s, comes out to 109 — dramatically low relative to the truth, and it's exact, worked-by-hand math, not noise: 18 of the 20 buckets are all-baseline and each has a p99 of 10 (nearest-rank of five identical values); the 2 buckets containing a spike each have a p99 of 1000 (nearest-rank of `[10,10,10,10,1000]` is the max, since n=5). count-weighted: `(18×10×5 + 2×1000×5) / 100 = 109.0`, exactly what the test asserts. **89.1% divergence from the true value, on data built specifically to demonstrate the failure mode as clearly as possible.**

she left it as-is rather than "fixing" it, and that's the right call for the same reason i left the single-level version honestly-stated instead of quietly patched — the actual fix isn't a bug patch, it's a real scope change: storing a proper streaming quantile sketch (a t-digest, an hdr histogram) per bucket instead of a single number, which is a legitimately different, bigger project. what she did instead was measure exactly how wrong the current shortcut gets and say so precisely, rather than let someone find out the hard way against real data later.

making a four-level cascade required generalizing structure i'd built for exactly one rollup level. the manifest went from a flat `l1_blocks` list to an indexable array:

```cpp
inline constexpr int kNumRollupLevels = 3;  // L1, L2, L3

struct Manifest {
  std::vector<std::string> l0_blocks;
  std::vector<std::vector<std::string>> rollup_levels =
      std::vector<std::vector<std::string>>(kNumRollupLevels);
  std::vector<std::string>& RollupBlocks(int level);  // level is 1, 2, or 3
};
```

and she tightened the manifest's text parser at the same time, in a direction that matters more than it looks: an earlier version of that parser would have silently dropped an unrecognized level token instead of failing loudly.

```cpp
int ParseLevelToken(const std::string& token, const std::string& path) {
  if (token.size() < 2 || token[0] != 'L') {
    throw std::runtime_error("Manifest: unrecognized level token '" + token + "' in " + path);
  }
  // ... digit-check the rest, range-check the level ...
}
```

a manifest that silently drops a line it doesn't recognize would quietly lose track of real, live data the moment a level this specific binary build doesn't know about ever showed up in a manifest it reads — which is exactly the kind of failure mode that stays invisible until it's already cost someone data. throwing instead is strictly more annoying in the moment and strictly safer in the failure case that actually matters.

the query router picked up the same generalization — instead of one hardcoded l0/l1 path, one shared helper scans any rollup level, parameterized by a pointer-to-member so the l1/l2/l3 loops aren't three copies of the same twenty lines:

```cpp
void ScanRollupLevel(const std::string& data_dir, int rollup_level, const Manifest& manifest,
                      std::unordered_map<uint64_t, SeriesQueryResult*>& by_id,
                      int64_t start_ms, int64_t end_ms,
                      std::vector<RollupBucket> SeriesQueryResult::*out_field,
                      uint32_t& scanned, uint32_t& skipped) {
  // same header-overlap-check-before-decode logic as the l0 loop, generalized
  // over which directory to read and which output field to fill in
}
```

a pointer-to-member, in case that syntax is unfamiliar: it's not a pointer to any specific object. it's a stored reference to *which field* to eventually access, on whatever object gets handed to it later. `&SeriesQueryResult::rollup_buckets_l1` doesn't point at any particular series' actual data — it points at the general concept "the l1-buckets field of this class." combine it with a real object later — `it->second->*out_field` — and it resolves to that specific field on that specific instance. that's the whole trick that lets one function body serve l1, l2, and l3 scanning at once: the only thing that changes between the three calls is which field pointer gets passed in, not the logic around it.

three copies of nearly-identical scanning code is three places the same bug can hide independently — collapsing them into one parameterized function is a small decision with an outsized payoff on correctness, not just a tidiness call.

## a second index, built specifically to find where the first one loses

a hash map has no concept of what order its keys are in — that's precisely why looking one up is so fast, and precisely why it structurally cannot answer "give me every series whose label starts with `host=h1`" without checking every single key, one at a time, no shortcut available. vaishnavi built a second, completely different structure to answer exactly that question: a b+ tree, keeping every key in sorted order.

a b+ tree's shape is worth being precise about, since "tree" alone doesn't tell you much. every actual key-value pair lives in a **leaf** node, and every leaf, once full, splits into two and gets **chained together** in sorted order — each leaf holds a pointer (a stored memory address — "where the next node lives," not a copy of it, so following the chain costs one cheap lookup rather than copying anything) to the next one over. above the leaves sit internal nodes that hold nothing but routing keys, telling a search which child to descend into to find a given key. `order` controls how many keys a node holds before it splits — a knob she deliberately swept across a few values (8, 32, 128) rather than picking one number and asserting it was reasonable.

insertion is the part actually worth walking through in code, because the leaf case and the internal case are structurally different in a way that's easy to gloss over:

```cpp
std::unique_ptr<Node> InsertIntoNode(Node* node, const std::string& key, uint64_t series_id,
                                       std::string* sep_key_out) {
  if (node->is_leaf) {
    // find sorted insertion point via lower_bound, insert key+value there
    if (static_cast<int>(node->keys.size()) <= order_) return nullptr;  // no split needed

    // split: right half moves to a brand-new leaf node
    auto new_leaf = std::make_unique<Node>();
    size_t mid = node->keys.size() / 2;
    new_leaf->keys.assign(node->keys.begin() + mid, node->keys.end());
    new_leaf->values.assign(node->values.begin() + mid, node->values.end());
    node->keys.resize(mid);

    new_leaf->next = node->next;   // splice into the leaf chain
    node->next = new_leaf.get();

    *sep_key_out = new_leaf->keys.front();  // the new leaf's smallest key routes to it
    return new_leaf;
  }

  // internal node: descend into the correct child, recurse, then possibly split *this* node too
}
```

(`std::unique_ptr` here is c++'s explicit version of what garbage collection does automatically in a language like python or javascript: it says "this pointer owns the object it points to, and deletes it automatically the instant nothing needs it anymore" — no manual cleanup call required, just opted into by hand instead of handled invisibly by a runtime.)

a leaf split is a straightforward "cut the sorted array in half, splice the new half into the chain." the separator key that gets handed back up to the parent is simply the new leaf's smallest key — anything greater than or equal to that key now lives on the right, anything less stays on the left. an internal-node split is subtly different in a way that's easy to get wrong if you're not paying attention: the middle key gets promoted *up* to the parent and does **not** get copied into either resulting half, unlike a leaf split, where the separator key stays physically present in the leaf that owns it. that asymmetry — leaf splits keep their separator, internal splits promote and discard theirs — is the actual structural rule that makes a b+ tree a b+ tree rather than a plain b-tree, and it's the exact detail the tree's own unit tests (inserting 30 keys with a small order specifically to force several levels of splitting) exist to catch if it's ever implemented backwards.

the one operation that genuinely can't be done on a hash map is the payoff for all of this — a prefix scan, walked directly off the sorted leaf chain:

```cpp
std::vector<uint64_t> PrefixQuery(const std::string& prefix) const {
  std::vector<uint64_t> result;
  Node* leaf = FindLeaf(prefix);
  bool started = false;
  while (leaf) {
    for (size_t i = 0; i < leaf->keys.size(); ++i) {
      bool matches = /* leaf->keys[i] starts with prefix */;
      if (matches) {
        started = true;
        result.insert(result.end(), leaf->values[i].begin(), leaf->values[i].end());
      } else if (started) {
        return result;  // sorted order guarantees every match is one contiguous run
      }
    }
    leaf = leaf->next;
  }
  return result;
}
```

descend once to find roughly where the prefix would sit, then walk forward along the chain only as long as keys keep matching. the moment a non-matching key follows a matching one, every subsequent key is guaranteed not to match either, because the keys are globally sorted and every key sharing a prefix necessarily forms one contiguous run in that order. that early-exit is only correct *because* the keys are sorted — it's the entire structural advantage a hash map cannot have, since a hash map's iteration order carries no relationship to lexicographic order at all. she gave `InvertedIndex` a brute-force `PrefixQuery` too, purely so the comparison has a fair, honest baseline instead of "one structure can't even attempt this" — hers checks every single key, no shortcut, exactly the cost a hash map genuinely pays.

fresh numbers, from a benchmark i ran myself just now, sweeping tree order 8/32/128 against the hash map, all four structures built over the identical synthetic dataset:

| | hash map | b+ tree (order 8) | b+ tree (order 32) | b+ tree (order 128) |
|---|---|---|---|---|
| lookup, 1M entries (p50) | **542 ns** | 4,875 ns | 3,417 ns | 3,375 ns |
| 3-filter intersect, 1M (p50) | **5,084 ns** | 10,958 ns | 9,917 ns | 9,875 ns |
| prefix scan `host=h1*`, 1M (111,111 matches) | 96.9 ms | 26.5 ms | 26.2 ms | **26.7 ms** |
| memory at 1M entries | ~140.2 MB | ~137.1 MB | ~114.7 MB | **~107.3 MB** |

the honest expectation going in was that the hash map wins plain lookups and intersections easily, and it does, at every scale tested — a b+ tree pays real cost walking down through internal nodes that a hash map's single computed bucket index just skips entirely. the prefix scan is the real comparison, the one the hash map has no shortcut for at all, and the tree wins there decisively — the gap widens with scale too: about **2.1x** faster at 1,000 entries, up to roughly **3.6x** faster at 1,000,000. one nuance worth reporting honestly rather than smoothing over: order doesn't win monotonically on *every* axis at *every* scale — at 1,000,000 entries specifically, order 32's prefix-scan time (26.2ms) edges out order 128's (26.7ms) by a hair, close enough to be within normal run-to-run noise rather than a real trend. what order consistently and clearly buys you, at every scale, is memory — order 128 uses roughly 23-24% less than a bare hash map at a million entries, and clearly less than the narrower tree orders too. wider nodes mean fewer total nodes and less per-node pointer/vector overhead, and that part of the story holds cleanly; the prefix-scan-vs-order relationship is close enough among the wider orders that i wouldn't claim a clean monotonic curve there without more samples than one run gives you.

it's worth restating plainly: this tree is **not wired into the live engine**. it's benchmark-only, in-memory, built specifically to answer one comparative question — exactly the same reasoning i used for why the cardinality benchmark drives `InvertedIndex` directly instead of through a live, disk-backed `Engine`: isolating a data structure's own performance from fsync latency doesn't require it to be sitting in the real write/query path.

## catching a gap in her own "proven" claim

step back from the tree for a moment, to the cascade — because this next part happened alongside it, not after it. this is the part i said at the top i think matters more than either of the two features above, so i want to give it the space it deserves rather than mention it in passing.

when she added the two new crash points for the l1-to-l2 manifest swap — the same shape of instrumented `SIGKILL` i'd used for the original five — she went back afterward and audited her own claim the same way i'd tried to hold the original codec and crash-safety work to: not "does this look right," but "have i actually gone and checked." and she found that the two new crash points had been correctly wired into the real compaction code, in exactly the right place, but the test harness meant to trigger them had never actually been taught their names:

```
$ STRATA_CRASH_AT="post_rollup_write_pre_manifest_rename_L2" \
  ./build/strata_tool crashtest "post_rollup_write_pre_manifest_rename_L2" /tmp/test
unknown crash point: post_rollup_write_pre_manifest_rename_L2
exit code: 2
```

this is not a corruption bug. it's arguably worse for a project whose entire pitch rests on "proven, not assumed" — it's a gap between what the crash-safety suite *claimed* to cover and what it actually ran. a reader trusting "seven fault-injection points, all passing" would have been trusting a number that was quietly wrong by two, for a while, in a codebase that exists specifically to demonstrate the opposite habit.

she verified the underlying mechanism was actually sound by driving it manually first, then closed the gap the same way the rest of the crash-safety suite is built: two new named scenarios in the real harness, each checked against an exact expected recovery outcome, not just "it didn't crash." deterministic fault-injection coverage went from my original five points to seven, and — i confirmed this myself, running the harness fresh rather than trusting the count — all seven pass for real now, not by resembling an already-proven case closely enough to pattern-match against.

i think this matters more than the multi-level cascade or the b+ tree because of what it's evidence *of*, not what it built. the codec-trust discipline i described earlier — don't trust it because it round-trips, go check the actual bits — isn't useful if it only ever applies to the one thing i personally happened to be paranoid about. it's only actually a discipline if it transfers to a different part of the codebase, applied by a different person, to their own work, without anyone telling them to. that's exactly what happened here.

---

## the numbers, together, self-measured just now

everything below i ran myself, on this exact codebase, rather than carried forward from an older draft:

**compression**, naive 16 bytes/point baseline: 3.74 B/pt on regular-cadence data (**4.28x** smaller), 7.11 B/pt on smooth-drift data (**2.25x** smaller). mine.

**cardinality scaling** — inverted index lookup latency, p50/p99, 1K → 1M unique series: 209ns/625ns → 541ns/1,500ns. roughly 2.5x latency growth across a thousand-fold increase in scale, index size growing linearly the entire time. mine.

**query planning** — 100 days of data, most of it aged into rollups: a 10-minute query touches 1 block and correctly skips 99; a 3-day query spanning both resolutions touches 3 and skips 97; a 90-day query touches 90 and skips 10. mine, and it reproduces exactly.

**write throughput** — flat at roughly 30,000-33,000 points/second across 1 to 32 concurrent writer threads on this machine, because every write durably fsyncs before it's acknowledged. mine. this is meaningfully lower than a figure i'd measured on a different machine at an earlier point in the project (46,000-49,000/sec) — the shape (flat regardless of concurrency) held perfectly, the absolute number moved by more than the 10-30% run-to-run drift i'd normally expect, which is itself an honest data point about how much fsync latency depends on the specific disk underneath a given machine, not something to paper over.

**crash recovery** — 7 deterministic fault-injection points (5 mine, 2 hers), each checked against an exact expected recovery outcome, all passing on a fresh run. plus one realistic external-kill harness. combined, mine and hers.

**rollup accuracy** — merging two levels of rollups on data built to demonstrate the worst case: 89.1% divergence between the merged p99 estimate and the true value. hers, and it's the single most important honesty-check number in this whole project.

**inverted index vs. b+ tree** — hash map wins point lookup and intersection at every scale tested (542ns vs. 3,375-4,875ns at 1M entries). the b+ tree wins prefix scans, and the margin widens with scale: 2.1x at 1,000 entries, 3.6x at 1,000,000. wider tree nodes save real memory (~23-24% less at order 128 vs. a bare hash map, at 1M entries). hers.

**test suite** — 10 binaries (8 mine, 2 hers), all passing on a fresh build just now. roughly 3,137 lines of engine code and 1,337 lines of tests total, up from an original ~2,400/~900 before her two commits landed — the growth is almost entirely hers.

---

## the good, the bad, and the ugly, stated plainly

**good:** durability is real, not aspirational — a point that's been acknowledged as written cannot be lost, proven with self-inflicted kills at exact code points rather than asserted in a comment. the compression codec is verified at the bit level, not just round-tripped. the query router and the inverted index both skip work at the cheapest possible check before doing the expensive thing, and both are measured, not assumed, at real scale. the crash-safety story extends cleanly across a four-level cascade because the same atomic-manifest-swap mechanism only had to be invented once and generalizes.

**bad, and known:** fsync-per-write caps throughput hard, by design, with group commit as the obvious and deliberately unbuilt next step. one coarse lock guards the whole engine — correct, simple, and not what you'd ship if lock contention ever became the actual bottleneck (it isn't yet; the wal's fsync dominates by a wide margin). the b+ tree is a comparison tool, not a production index — it's not wired into the live write or query path, on purpose.

**ugly, and the part i'd genuinely tell someone to focus on if they only had time for one thing:** the p99 rollup approximation is the sharpest edge in this whole system, and it compounds — a single level of compaction loses precision in a way i described but never quantified; two levels of compaction, measured directly, diverges by 89.1% on data built to expose exactly that failure mode. and separately, the crash-safety suite itself once claimed coverage it didn't actually have — two fault-injection points that were real in the code but unreachable by the harness meant to exercise them, caught only because someone went back and checked their own "proven" claim a second time instead of trusting it because it had passed before.

that last one is the actual thesis of this whole post, more than the codec or the cascade or the tree. a system that's supposed to be correct at the bit level, or the crash-recovery level, or the "this test suite actually covers what it claims to" level, doesn't get to just look correct in a demo or a green checkmark. it has to survive someone going and checking, specifically, on purpose, the way they'd check if they didn't already trust it — and it has to survive that check twice: once from the person who wrote it, and once from whoever picks it up next. i did the first check on my half. vaishnavi did both checks on hers, including on herself. the number you didn't double-check yet is the one worth double-checking first, and that held true for both of us, independently, months apart.

---

*written by akshat kankani. the wal, memtable, gorilla codec, single-level compaction, crash-safety fault injection, inverted index, and query router are mine. the multi-level compaction cascade, the b+ tree comparison, and the crash-safety audit that found the untested fault-injection points are vaishnavi rai's. source on [github](https://github.com/kankaniakshat185/strata).*
