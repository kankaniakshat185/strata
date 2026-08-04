#pragma once

#include <cstddef>
#include <cstdint>

namespace strata {

// Standard reflected CRC-32 (polynomial 0xEDB88320, same table as zlib).
// Used for WAL record integrity and block footer corruption detection.
uint32_t Crc32(const void* data, size_t len);

}  // namespace strata
