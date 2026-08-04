#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace strata {

struct WalRecord {
  uint64_t series_id;
  int64_t timestamp;  // unix millis
  double value;
};

// series_id(8) + timestamp(8) + value(8) + crc32(4), no length prefix.
inline constexpr size_t kWalRecordSize = 28;

// Appends fixed-size WAL records, fsync-ing after every append so a
// completed Append() is durable against a subsequent process kill.
class WalWriter {
 public:
  explicit WalWriter(const std::string& path);
  ~WalWriter();
  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  void Append(const WalRecord& record);

 private:
  int fd_ = -1;
};

// Replays a WAL file front to back, calling `on_record` for each complete,
// checksum-valid record. A trailing partial record (file size not a
// multiple of 28) or one whose CRC doesn't match is a torn write from a
// mid-append crash: replay stops there and the file is truncated to the
// last good record, per STRATA_DESIGN.md. Missing file is a no-op.
void ReplayWal(const std::string& path,
                const std::function<void(const WalRecord&)>& on_record);

}  // namespace strata
