#include "strata/l1_writer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <stdexcept>

#include "strata/bitio.hpp"
#include "strata/block_format.hpp"
#include "strata/byteio.hpp"
#include "strata/crc32.hpp"
#include "strata/gorilla.hpp"

// L1's series index entry reuses SeriesIndexEntry.point_count to mean
// "bucket count" instead of "point count" -- same field, different unit,
// since a block only ever holds one level's worth of series at a time and
// the header's `level` byte already disambiguates which.
namespace strata {

namespace {

int64_t NowUnixSeconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch())
      .count();
}

uint64_t DoubleToBits(double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  return bits;
}

double BitsToDouble(uint64_t bits) {
  double v;
  std::memcpy(&v, &bits, 8);
  return v;
}

void WriteAll(int fd, const uint8_t* data, size_t len,
              const std::string& what) {
  ssize_t n = ::write(fd, data, len);
  if (n != static_cast<ssize_t>(len)) {
    throw std::runtime_error("L1: short write on " + what);
  }
}

void WriteBucketStats(BitWriter& bw, const RollupBucket& b) {
  bw.WriteBits(uint64_t(b.count), 32);
  bw.WriteBits(DoubleToBits(b.min), 64);
  bw.WriteBits(DoubleToBits(b.max), 64);
  bw.WriteBits(DoubleToBits(b.avg), 64);
  bw.WriteBits(DoubleToBits(b.p99), 64);
}

void ReadBucketStats(BitReader& br, RollupBucket& b) {
  b.count = uint32_t(br.ReadBits(32));
  b.min = BitsToDouble(br.ReadBits(64));
  b.max = BitsToDouble(br.ReadBits(64));
  b.avg = BitsToDouble(br.ReadBits(64));
  b.p99 = BitsToDouble(br.ReadBits(64));
}

// First bucket_start raw, each subsequent one delta-of-delta -- exactly
// the timestamp half of gorilla::EncodeSeries, reused here. No XOR value
// half: stats are stored raw per STRATA_DESIGN.md's deliberate
// simplification (L1 volume is a fraction of L0's; not worth four more
// parallel XOR streams for the payoff).
std::vector<uint8_t> EncodeSeriesRollup(const std::vector<RollupBucket>& buckets) {
  BitWriter bw;
  if (buckets.empty()) return bw.Finish();

  bw.WriteBits(uint64_t(buckets[0].bucket_start), 64);
  WriteBucketStats(bw, buckets[0]);

  int64_t prev_ts = buckets[0].bucket_start;
  int64_t prev_delta = 0;
  for (size_t i = 1; i < buckets.size(); ++i) {
    int64_t ts = buckets[i].bucket_start;
    int64_t delta = ts - prev_ts;
    gorilla::EncodeTimestampDelta(bw, delta - prev_delta);
    prev_delta = delta;
    prev_ts = ts;
    WriteBucketStats(bw, buckets[i]);
  }
  return bw.Finish();
}

std::vector<RollupBucket> DecodeSeriesRollup(const uint8_t* data,
                                              size_t byte_len,
                                              uint32_t bucket_count) {
  std::vector<RollupBucket> buckets;
  if (bucket_count == 0) return buckets;
  buckets.reserve(bucket_count);

  BitReader br(data, byte_len);
  RollupBucket first;
  first.bucket_start = int64_t(br.ReadBits(64));
  ReadBucketStats(br, first);
  buckets.push_back(first);

  int64_t prev_ts = first.bucket_start;
  int64_t prev_delta = 0;
  for (uint32_t i = 1; i < bucket_count; ++i) {
    int64_t dod = gorilla::DecodeTimestampDelta(br);
    int64_t delta = prev_delta + dod;
    int64_t ts = prev_ts + delta;
    prev_delta = delta;
    prev_ts = ts;

    RollupBucket b;
    b.bucket_start = ts;
    ReadBucketStats(br, b);
    buckets.push_back(b);
  }
  return buckets;
}

}  // namespace

void WriteL1Block(const std::string& path,
                   const std::vector<L1SeriesRollup>& series) {
  BlockHeader header;
  header.level = kLevelL1;
  header.series_count = static_cast<uint32_t>(series.size());
  header.created_at = NowUnixSeconds();
  bool first = true;
  for (const auto& s : series) {
    for (const auto& b : s.buckets) {
      if (first) {
        header.min_timestamp = header.max_timestamp = b.bucket_start;
        first = false;
      }
      if (b.bucket_start < header.min_timestamp)
        header.min_timestamp = b.bucket_start;
      if (b.bucket_start > header.max_timestamp)
        header.max_timestamp = b.bucket_start;
    }
  }

  size_t index_bytes = series.size() * kSeriesIndexEntrySize;
  std::vector<uint8_t> buf(kBlockHeaderSize + index_bytes);
  EncodeBlockHeader(header, buf.data());

  std::vector<uint8_t> data_section;
  size_t running_offset = 0;
  for (size_t si = 0; si < series.size(); ++si) {
    const auto& s = series[si];
    std::vector<uint8_t> stream = EncodeSeriesRollup(s.buckets);

    SeriesIndexEntry e;
    e.series_id = s.series_id;
    e.offset = running_offset;
    e.byte_length = static_cast<uint32_t>(stream.size());
    e.point_count = static_cast<uint32_t>(s.buckets.size());  // bucket count
    EncodeSeriesIndexEntry(
        e, buf.data() + kBlockHeaderSize + si * kSeriesIndexEntrySize);

    data_section.insert(data_section.end(), stream.begin(), stream.end());
    running_offset += stream.size();
  }

  buf.insert(buf.end(), data_section.begin(), data_section.end());

  uint32_t crc = Crc32(buf.data(), buf.size());
  uint8_t footer[kFooterSize];
  io::PutU32(footer, crc);

  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) throw std::runtime_error("L1: open failed for " + path);
  WriteAll(fd, buf.data(), buf.size(), path);
  WriteAll(fd, footer, kFooterSize, path);
  if (::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("L1: fsync failed for " + path);
  }
  ::close(fd);
}

namespace {

std::vector<uint8_t> ReadWholeFileChecked(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("L1: open failed for " + path);

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error("L1: fstat failed for " + path);
  }

  std::vector<uint8_t> buf(st.st_size);
  ssize_t n = ::pread(fd, buf.data(), buf.size(), 0);
  ::close(fd);
  if (n != static_cast<ssize_t>(buf.size())) {
    throw std::runtime_error("L1: short read for " + path);
  }
  if (buf.size() < kBlockHeaderSize + kFooterSize) {
    throw std::runtime_error("L1: truncated block " + path);
  }

  uint32_t stored_crc = io::GetU32(buf.data() + buf.size() - kFooterSize);
  uint32_t actual_crc = Crc32(buf.data(), buf.size() - kFooterSize);
  if (stored_crc != actual_crc) {
    throw std::runtime_error("L1: footer CRC mismatch (corrupt block) " +
                              path);
  }

  BlockHeader header = DecodeBlockHeader(buf.data());
  if (header.magic != kBlockMagic) {
    throw std::runtime_error("L1: bad magic in " + path);
  }
  if (header.level != kLevelL1) {
    throw std::runtime_error("L1: not an L1 block: " + path);
  }
  return buf;
}

}  // namespace

L1Summary SummarizeL1Block(const std::string& path) {
  std::vector<uint8_t> buf = ReadWholeFileChecked(path);
  BlockHeader header = DecodeBlockHeader(buf.data());

  L1Summary summary;
  summary.series_count = header.series_count;
  summary.min_timestamp = header.min_timestamp;
  summary.max_timestamp = header.max_timestamp;
  for (uint32_t i = 0; i < header.series_count; ++i) {
    const uint8_t* p =
        buf.data() + kBlockHeaderSize + i * kSeriesIndexEntrySize;
    SeriesIndexEntry e = DecodeSeriesIndexEntry(p);
    summary.bucket_count += e.point_count;
    summary.total_data_bytes += e.byte_length;
  }
  return summary;
}

std::vector<L1SeriesRollup> ReadL1Block(const std::string& path) {
  std::vector<uint8_t> buf = ReadWholeFileChecked(path);
  BlockHeader header = DecodeBlockHeader(buf.data());

  size_t data_start =
      kBlockHeaderSize + header.series_count * kSeriesIndexEntrySize;

  std::vector<L1SeriesRollup> result;
  result.reserve(header.series_count);
  for (uint32_t i = 0; i < header.series_count; ++i) {
    const uint8_t* p =
        buf.data() + kBlockHeaderSize + i * kSeriesIndexEntrySize;
    SeriesIndexEntry e = DecodeSeriesIndexEntry(p);

    L1SeriesRollup r;
    r.series_id = e.series_id;
    r.buckets = DecodeSeriesRollup(buf.data() + data_start + e.offset,
                                    e.byte_length, e.point_count);
    result.push_back(std::move(r));
  }
  return result;
}

}  // namespace strata
