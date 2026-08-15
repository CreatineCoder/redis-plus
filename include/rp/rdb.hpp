#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rp/store.hpp"

namespace rp {

// RDB snapshot format.
//
// This is the real redis on-disk layout -- magic "REDIS0011", length-prefixed
// strings, expiry opcodes, CRC64 trailer -- not an invented one. Matching it
// costs little and buys two things: `redis-check-rdb` can validate our output,
// and Phase 5's full resync can ship this exact payload to a replica, which is
// what a real PSYNC does.
//
// Writing covers what the store holds today: string values, optional
// millisecond expiries. Reading additionally accepts the integer-encoded
// strings real redis emits, so files it wrote can be loaded. LZF-compressed
// strings are detected and reported rather than silently mangled.

inline constexpr std::string_view kRdbMagic = "REDIS";
inline constexpr std::string_view kRdbVersion = "0011";

// Redis' CRC64 (Jones polynomial, reflected). Seed with 0.
std::uint64_t crc64(std::uint64_t crc, const void* data, std::size_t len);

// Serialize to a complete, self-contained RDB payload including the CRC
// trailer. Phase 5 sends the return value verbatim over the wire.
std::string rdb_serialize(const std::vector<Record>& records);

// Parse a complete payload. Returns false and sets `error` on a bad magic,
// truncation, an unknown opcode, or a CRC mismatch -- a corrupt snapshot must
// be loud, never partially applied.
bool rdb_parse(std::string_view payload, std::vector<Record>* out,
               std::string* error);

// Write atomically: serialize to a temp file in the same directory, fsync,
// then rename over the target. A crash mid-save must never leave a truncated
// snapshot where the previous good one was.
bool rdb_save_file(const std::string& path, const std::vector<Record>& records,
                   std::string* error);

// Load a snapshot. A missing file is success with zero records -- a fresh
// server has no snapshot yet, and that is not an error.
bool rdb_load_file(const std::string& path, std::vector<Record>* out,
                   std::string* error);

}  // namespace rp
