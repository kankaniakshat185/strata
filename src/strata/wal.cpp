#include "strata/wal.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "strata/byteio.hpp"
#include "strata/crc32.hpp"

namespace strata {

WalWriter::WalWriter(const std::string& path) {
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd_ < 0) {
    throw std::runtime_error("WalWriter: open failed for " + path + ": " +
                              std::strerror(errno));
  }
}

WalWriter::~WalWriter() {
  if (fd_ >= 0) ::close(fd_);
}

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
    throw std::runtime_error(std::string("WalWriter: fsync failed: ") +
                              std::strerror(errno));
  }
}

void ReplayWal(const std::string& path,
                const std::function<void(const WalRecord&)>& on_record) {
  int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    if (errno == ENOENT) return;
    throw std::runtime_error("ReplayWal: open failed for " + path + ": " +
                              std::strerror(errno));
  }

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error("ReplayWal: fstat failed for " + path);
  }

  uint8_t buf[kWalRecordSize];
  off_t good_offset = 0;
  while (good_offset + static_cast<off_t>(kWalRecordSize) <= st.st_size) {
    ssize_t n = ::pread(fd, buf, kWalRecordSize, good_offset);
    if (n != static_cast<ssize_t>(kWalRecordSize)) break;

    uint32_t stored_crc = io::GetU32(buf + 24);
    uint32_t actual_crc = Crc32(buf, 24);
    if (stored_crc != actual_crc) break;  // torn write / corruption boundary

    WalRecord r;
    r.series_id = io::GetU64(buf);
    r.timestamp = io::GetI64(buf + 8);
    r.value = io::GetDouble(buf + 16);
    on_record(r);

    good_offset += static_cast<off_t>(kWalRecordSize);
  }

  if (good_offset != st.st_size) {
    // Drop the torn/corrupt tail so future appends start clean.
    ::ftruncate(fd, good_offset);
  }
  ::close(fd);
}

}  // namespace strata
