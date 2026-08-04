#include "strata/l0_writer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

#include "strata/block_format.hpp"
#include "strata/byteio.hpp"
#include "strata/crc32.hpp"
#include "strata/gorilla.hpp"

namespace strata {

namespace {

int64_t NowUnixSeconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch())
      .count();
}

void WriteAll(int fd, const uint8_t* data, size_t len,
              const std::string& what) {
  ssize_t n = ::write(fd, data, len);
  if (n != static_cast<ssize_t>(len)) {
    throw std::runtime_error("L0: short write on " + what);
  }
}

struct SeriesPoints {
  uint64_t series_id;
  std::vector<std::pair<int64_t, double>> points;
};

}  // namespace

void WriteL0Block(const std::string& path, const MemTable& memtable) {
  // MemTable's std::map is already sorted by (series_id, timestamp), so a
  // single linear pass groups points into per-series runs.
  std::vector<SeriesPoints> series;
  for (const auto& [key, value] : memtable.points()) {
    uint64_t series_id = key.first;
    int64_t ts = key.second;
    if (series.empty() || series.back().series_id != series_id) {
      series.push_back(SeriesPoints{series_id, {}});
    }
    series.back().points.push_back({ts, value});
  }

  BlockHeader header;
  header.level = kLevelL0;
  header.series_count = static_cast<uint32_t>(series.size());
  header.created_at = NowUnixSeconds();
  bool first = true;
  for (const auto& s : series) {
    for (const auto& [ts, value] : s.points) {
      (void)value;
      if (first) {
        header.min_timestamp = header.max_timestamp = ts;
        first = false;
      }
      if (ts < header.min_timestamp) header.min_timestamp = ts;
      if (ts > header.max_timestamp) header.max_timestamp = ts;
    }
  }

  size_t index_bytes = series.size() * kSeriesIndexEntrySize;
  std::vector<uint8_t> buf(kBlockHeaderSize + index_bytes);
  EncodeBlockHeader(header, buf.data());

  std::vector<uint8_t> data_section;
  size_t running_offset = 0;
  for (size_t si = 0; si < series.size(); ++si) {
    const auto& s = series[si];
    std::vector<uint8_t> stream = gorilla::EncodeSeries(s.points);

    SeriesIndexEntry e;
    e.series_id = s.series_id;
    e.offset = running_offset;
    e.byte_length = static_cast<uint32_t>(stream.size());
    e.point_count = static_cast<uint32_t>(s.points.size());
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
  if (fd < 0) throw std::runtime_error("L0: open failed for " + path);
  WriteAll(fd, buf.data(), buf.size(), path);
  WriteAll(fd, footer, kFooterSize, path);
  if (::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("L0: fsync failed for " + path);
  }
  ::close(fd);
}

L0Summary SummarizeL0Block(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("L0: open failed for " + path);

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error("L0: fstat failed for " + path);
  }

  std::vector<uint8_t> buf(st.st_size);
  ssize_t n = ::pread(fd, buf.data(), buf.size(), 0);
  ::close(fd);
  if (n != static_cast<ssize_t>(buf.size())) {
    throw std::runtime_error("L0: short read for " + path);
  }
  if (buf.size() < kBlockHeaderSize + kFooterSize) {
    throw std::runtime_error("L0: truncated block " + path);
  }

  uint32_t stored_crc = io::GetU32(buf.data() + buf.size() - kFooterSize);
  uint32_t actual_crc = Crc32(buf.data(), buf.size() - kFooterSize);
  if (stored_crc != actual_crc) {
    throw std::runtime_error("L0: footer CRC mismatch (corrupt block) " +
                              path);
  }

  BlockHeader header = DecodeBlockHeader(buf.data());
  if (header.magic != kBlockMagic) {
    throw std::runtime_error("L0: bad magic in " + path);
  }

  L0Summary summary;
  summary.series_count = header.series_count;
  summary.min_timestamp = header.min_timestamp;
  summary.max_timestamp = header.max_timestamp;
  summary.created_at = header.created_at;
  for (uint32_t i = 0; i < header.series_count; ++i) {
    const uint8_t* p =
        buf.data() + kBlockHeaderSize + i * kSeriesIndexEntrySize;
    SeriesIndexEntry e = DecodeSeriesIndexEntry(p);
    summary.point_count += e.point_count;
    summary.total_data_bytes += e.byte_length;
  }
  return summary;
}

L0BlockData ReadL0Block(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("L0: open failed for " + path);

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error("L0: fstat failed for " + path);
  }

  std::vector<uint8_t> buf(st.st_size);
  ssize_t n = ::pread(fd, buf.data(), buf.size(), 0);
  ::close(fd);
  if (n != static_cast<ssize_t>(buf.size())) {
    throw std::runtime_error("L0: short read for " + path);
  }
  if (buf.size() < kBlockHeaderSize + kFooterSize) {
    throw std::runtime_error("L0: truncated block " + path);
  }

  uint32_t stored_crc = io::GetU32(buf.data() + buf.size() - kFooterSize);
  uint32_t actual_crc = Crc32(buf.data(), buf.size() - kFooterSize);
  if (stored_crc != actual_crc) {
    throw std::runtime_error("L0: footer CRC mismatch (corrupt block) " +
                              path);
  }

  BlockHeader header = DecodeBlockHeader(buf.data());
  if (header.magic != kBlockMagic) {
    throw std::runtime_error("L0: bad magic in " + path);
  }

  size_t data_start = kBlockHeaderSize + header.series_count * kSeriesIndexEntrySize;

  L0BlockData block;
  block.series_count = header.series_count;
  block.min_timestamp = header.min_timestamp;
  block.max_timestamp = header.max_timestamp;
  block.series.reserve(header.series_count);

  for (uint32_t i = 0; i < header.series_count; ++i) {
    const uint8_t* p =
        buf.data() + kBlockHeaderSize + i * kSeriesIndexEntrySize;
    SeriesIndexEntry e = DecodeSeriesIndexEntry(p);

    L0SeriesData sd;
    sd.series_id = e.series_id;
    sd.points = gorilla::DecodeSeries(buf.data() + data_start + e.offset,
                                       e.byte_length, e.point_count);
    block.series.push_back(std::move(sd));
  }
  return block;
}

}  // namespace strata
