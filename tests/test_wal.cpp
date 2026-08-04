// Unit tests for WAL append/replay: normal round-trip, torn-write
// truncation (partial trailing record), and corruption detection (a
// record whose CRC doesn't match its bytes) -- both are treated as the
// same "stop and truncate here" case per STRATA_DESIGN.md.
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

#include "strata/wal.hpp"

namespace {

void RemoveIfExists(const std::string& path) { ::unlink(path.c_str()); }

off_t FileSize(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return -1;
  return st.st_size;
}

void TestRoundTrip() {
  const std::string path = "/tmp/strata_test_wal_roundtrip.wal";
  RemoveIfExists(path);

  {
    strata::WalWriter w(path);
    w.Append({1, 100, 1.5});
    w.Append({1, 200, 2.5});
    w.Append({2, 300, 3.5});
  }

  std::vector<strata::WalRecord> got;
  strata::ReplayWal(path, [&](const strata::WalRecord& r) { got.push_back(r); });

  assert(got.size() == 3);
  assert(got[0].series_id == 1 && got[0].timestamp == 100 && got[0].value == 1.5);
  assert(got[1].series_id == 1 && got[1].timestamp == 200 && got[1].value == 2.5);
  assert(got[2].series_id == 2 && got[2].timestamp == 300 && got[2].value == 3.5);
  assert(FileSize(path) == static_cast<off_t>(3 * strata::kWalRecordSize));

  RemoveIfExists(path);
}

void TestTornTailTruncation() {
  const std::string path = "/tmp/strata_test_wal_torn.wal";
  RemoveIfExists(path);

  {
    strata::WalWriter w(path);
    w.Append({7, 111, 9.0});
    w.Append({7, 222, 10.0});
  }

  // Simulate a crash mid-append: a partial (torn) third record.
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
  assert(fd >= 0);
  uint8_t garbage[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  ssize_t n = ::write(fd, garbage, sizeof(garbage));
  assert(n == static_cast<ssize_t>(sizeof(garbage)));
  ::close(fd);

  assert(FileSize(path) == static_cast<off_t>(2 * strata::kWalRecordSize + 10));

  std::vector<strata::WalRecord> got;
  strata::ReplayWal(path, [&](const strata::WalRecord& r) { got.push_back(r); });

  assert(got.size() == 2);
  assert(got[0].timestamp == 111 && got[1].timestamp == 222);
  // Replay must truncate the torn tail so future appends start clean.
  assert(FileSize(path) == static_cast<off_t>(2 * strata::kWalRecordSize));

  RemoveIfExists(path);
}

void TestCorruptRecordDetected() {
  const std::string path = "/tmp/strata_test_wal_corrupt.wal";
  RemoveIfExists(path);

  {
    strata::WalWriter w(path);
    w.Append({1, 1, 1.0});
    w.Append({1, 2, 2.0});
    w.Append({1, 3, 3.0});
  }

  // Flip a byte inside the second record's value field -- its CRC no
  // longer matches, so replay should treat it (and everything after) as
  // unrecoverable, exactly like a torn write.
  int fd = ::open(path.c_str(), O_WRONLY);
  assert(fd >= 0);
  off_t corrupt_offset = strata::kWalRecordSize + 16;  // inside record 2's value
  uint8_t byte = 0xFF;
  ssize_t n = ::pwrite(fd, &byte, 1, corrupt_offset);
  assert(n == 1);
  ::close(fd);

  std::vector<strata::WalRecord> got;
  strata::ReplayWal(path, [&](const strata::WalRecord& r) { got.push_back(r); });

  assert(got.size() == 1);
  assert(got[0].timestamp == 1);
  assert(FileSize(path) == static_cast<off_t>(1 * strata::kWalRecordSize));

  RemoveIfExists(path);
}

}  // namespace

int main() {
  TestRoundTrip();
  TestTornTailTruncation();
  TestCorruptRecordDetected();
  std::printf("test_wal: all assertions passed\n");
  return 0;
}
