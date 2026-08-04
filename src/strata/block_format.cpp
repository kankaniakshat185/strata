#include "strata/block_format.hpp"

#include "strata/byteio.hpp"

namespace strata {

void EncodeBlockHeader(const BlockHeader& h, uint8_t* out) {
  io::PutU32(out, h.magic);
  io::PutU8(out + 4, h.level);
  io::PutU8(out + 5, h.format_version);
  io::PutU32(out + 6, h.series_count);
  io::PutI64(out + 10, h.min_timestamp);
  io::PutI64(out + 18, h.max_timestamp);
  io::PutI64(out + 26, h.created_at);
}

BlockHeader DecodeBlockHeader(const uint8_t* in) {
  BlockHeader h;
  h.magic = io::GetU32(in);
  h.level = io::GetU8(in + 4);
  h.format_version = io::GetU8(in + 5);
  h.series_count = io::GetU32(in + 6);
  h.min_timestamp = io::GetI64(in + 10);
  h.max_timestamp = io::GetI64(in + 18);
  h.created_at = io::GetI64(in + 26);
  return h;
}

void EncodeSeriesIndexEntry(const SeriesIndexEntry& e, uint8_t* out) {
  io::PutU64(out, e.series_id);
  io::PutU64(out + 8, e.offset);
  io::PutU32(out + 16, e.byte_length);
  io::PutU32(out + 20, e.point_count);
}

SeriesIndexEntry DecodeSeriesIndexEntry(const uint8_t* in) {
  SeriesIndexEntry e;
  e.series_id = io::GetU64(in);
  e.offset = io::GetU64(in + 8);
  e.byte_length = io::GetU32(in + 16);
  e.point_count = io::GetU32(in + 20);
  return e;
}

}  // namespace strata
