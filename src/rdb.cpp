#include "rp/rdb.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rp {
namespace {

// --- opcodes ---------------------------------------------------------------
constexpr std::uint8_t kOpAux = 0xFA;
constexpr std::uint8_t kOpResizeDb = 0xFB;
constexpr std::uint8_t kOpExpireMs = 0xFC;
constexpr std::uint8_t kOpExpireSec = 0xFD;
constexpr std::uint8_t kOpSelectDb = 0xFE;
constexpr std::uint8_t kOpEof = 0xFF;
constexpr std::uint8_t kTypeString = 0x00;

// --- length encoding -------------------------------------------------------
constexpr std::uint8_t k6Bit = 0x00;
constexpr std::uint8_t k14Bit = 0x40;
constexpr std::uint8_t kEncoded = 0xC0;  // special string encodings
constexpr std::uint8_t k32Bit = 0x80;
constexpr std::uint8_t k64Bit = 0x81;

constexpr std::uint8_t kEncInt8 = 0;
constexpr std::uint8_t kEncInt16 = 1;
constexpr std::uint8_t kEncInt32 = 2;
constexpr std::uint8_t kEncLzf = 3;

// --- CRC64 (Jones, reflected) ----------------------------------------------
constexpr std::uint64_t kPoly = 0xad93d23594c935a9ULL;

std::uint64_t reflect(std::uint64_t value, int bits) {
  std::uint64_t out = 0;
  for (int i = 0; i < bits; ++i) {
    if (value & (1ULL << i)) out |= 1ULL << (bits - 1 - i);
  }
  return out;
}

const std::array<std::uint64_t, 256>& crc_table() {
  static const std::array<std::uint64_t, 256> table = [] {
    std::array<std::uint64_t, 256> t{};
    for (int n = 0; n < 256; ++n) {
      std::uint64_t crc = reflect(static_cast<std::uint64_t>(n), 8) << 56;
      for (int k = 0; k < 8; ++k) {
        crc = (crc & 0x8000000000000000ULL) ? (crc << 1) ^ kPoly : (crc << 1);
      }
      t[n] = reflect(crc, 64);
    }
    return t;
  }();
  return table;
}

void put_u8(std::string& out, std::uint8_t v) {
  out.push_back(static_cast<char>(v));
}

void put_u64_le(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) put_u8(out, static_cast<std::uint8_t>(v >> (i * 8)));
}

void put_length(std::string& out, std::uint64_t len) {
  if (len < (1 << 6)) {
    put_u8(out, static_cast<std::uint8_t>(k6Bit | len));
  } else if (len < (1 << 14)) {
    put_u8(out, static_cast<std::uint8_t>(k14Bit | (len >> 8)));
    put_u8(out, static_cast<std::uint8_t>(len & 0xFF));
  } else if (len <= 0xFFFFFFFFULL) {
    put_u8(out, k32Bit);
    for (int i = 3; i >= 0; --i) {  // big endian, as redis writes it
      put_u8(out, static_cast<std::uint8_t>(len >> (i * 8)));
    }
  } else {
    put_u8(out, k64Bit);
    for (int i = 7; i >= 0; --i) {
      put_u8(out, static_cast<std::uint8_t>(len >> (i * 8)));
    }
  }
}

void put_string(std::string& out, std::string_view s) {
  put_length(out, s.size());
  out.append(s);
}

// --- reader ----------------------------------------------------------------

class Reader {
 public:
  explicit Reader(std::string_view data) : data_(data) {}

  bool eof() const { return pos_ >= data_.size(); }
  std::size_t pos() const { return pos_; }
  bool ok() const { return ok_; }

  std::uint8_t u8() {
    if (pos_ + 1 > data_.size()) return fail();
    return static_cast<std::uint8_t>(data_[pos_++]);
  }

  std::uint8_t peek() {
    if (pos_ >= data_.size()) return fail();
    return static_cast<std::uint8_t>(data_[pos_]);
  }

  std::uint64_t u64_le() {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<std::uint64_t>(u8()) << (i * 8);
    }
    return v;
  }

  std::uint32_t u32_le() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(u8()) << (i * 8);
    }
    return v;
  }

  // Returns the length; sets `encoding` when the byte was a special string
  // encoding rather than a plain length.
  std::uint64_t length(bool* is_encoded, std::uint8_t* encoding) {
    *is_encoded = false;
    *encoding = 0;
    const std::uint8_t first = u8();
    if (!ok_) return 0;

    switch (first & 0xC0) {
      case k6Bit:
        return first & 0x3F;
      case k14Bit: {
        const std::uint8_t second = u8();
        return (static_cast<std::uint64_t>(first & 0x3F) << 8) | second;
      }
      case kEncoded:
        *is_encoded = true;
        *encoding = first & 0x3F;
        return 0;
      default:
        break;
    }
    if (first == k32Bit) {
      std::uint64_t v = 0;
      for (int i = 0; i < 4; ++i) v = (v << 8) | u8();
      return v;
    }
    if (first == k64Bit) {
      std::uint64_t v = 0;
      for (int i = 0; i < 8; ++i) v = (v << 8) | u8();
      return v;
    }
    return fail();
  }

  std::string str(std::string* error) {
    bool encoded = false;
    std::uint8_t encoding = 0;
    const std::uint64_t len = length(&encoded, &encoding);
    if (!ok_) return {};

    if (encoded) {
      switch (encoding) {
        case kEncInt8:
          return std::to_string(static_cast<std::int8_t>(u8()));
        case kEncInt16: {
          const std::uint16_t lo = u8(), hi = u8();
          return std::to_string(
              static_cast<std::int16_t>(lo | (hi << 8)));
        }
        case kEncInt32:
          return std::to_string(static_cast<std::int32_t>(u32_le()));
        case kEncLzf:
          *error =
              "LZF-compressed strings are not supported; re-save with "
              "rdbcompression no";
          ok_ = false;
          return {};
        default:
          *error = "unknown string encoding " + std::to_string(encoding);
          ok_ = false;
          return {};
      }
    }

    if (pos_ + len > data_.size()) {
      fail();
      return {};
    }
    std::string out(data_.substr(pos_, static_cast<std::size_t>(len)));
    pos_ += static_cast<std::size_t>(len);
    return out;
  }

 private:
  std::uint8_t fail() {
    ok_ = false;
    pos_ = data_.size();
    return 0;
  }

  std::string_view data_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

bool fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}

}  // namespace

std::uint64_t crc64(std::uint64_t crc, const void* data, std::size_t len) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  const auto& table = crc_table();
  for (std::size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

std::string rdb_serialize(const std::vector<Record>& records) {
  std::string out;
  out.reserve(records.size() * 48 + 64);

  out.append(kRdbMagic);
  out.append(kRdbVersion);

  put_u8(out, kOpAux);
  put_string(out, "redis-ver");
  put_string(out, "7.0.0-rp");

  put_u8(out, kOpSelectDb);
  put_length(out, 0);

  std::size_t with_expiry = 0;
  for (const auto& r : records) {
    if (r.expire_at != kNoExpiry) ++with_expiry;
  }
  put_u8(out, kOpResizeDb);
  put_length(out, records.size());
  put_length(out, with_expiry);

  for (const auto& r : records) {
    if (r.expire_at != kNoExpiry) {
      put_u8(out, kOpExpireMs);
      put_u64_le(out, static_cast<std::uint64_t>(r.expire_at));
    }
    put_u8(out, kTypeString);
    put_string(out, r.key);
    put_string(out, r.value);
  }

  put_u8(out, kOpEof);
  put_u64_le(out, crc64(0, out.data(), out.size()));
  return out;
}

bool rdb_parse(std::string_view payload, std::vector<Record>* out,
               std::string* error) {
  out->clear();

  const std::size_t header = kRdbMagic.size() + kRdbVersion.size();
  if (payload.size() < header) return fail(error, "file too short");
  if (payload.substr(0, kRdbMagic.size()) != kRdbMagic) {
    return fail(error, "bad magic: not an RDB file");
  }

  const std::string version(payload.substr(kRdbMagic.size(), 4));
  if (version > std::string(kRdbVersion)) {
    return fail(error, "RDB version " + version + " is newer than supported " +
                           std::string(kRdbVersion));
  }

  // The CRC trailer covers everything before it. Verify before interpreting a
  // single record, so a corrupt file is never partially applied.
  if (payload.size() < header + 9) return fail(error, "truncated: no EOF marker");
  const std::size_t crc_offset = payload.size() - 8;
  std::uint64_t stored = 0;
  for (int i = 0; i < 8; ++i) {
    stored |= static_cast<std::uint64_t>(
                  static_cast<std::uint8_t>(payload[crc_offset + i]))
              << (i * 8);
  }
  if (static_cast<std::uint8_t>(payload[crc_offset - 1]) != kOpEof) {
    return fail(error, "truncated: EOF marker missing");
  }
  // A zero CRC means checksumming was disabled when the file was written.
  if (stored != 0) {
    const std::uint64_t actual = crc64(0, payload.data(), crc_offset);
    if (actual != stored) return fail(error, "checksum mismatch: file corrupt");
  }

  Reader r(payload.substr(header, crc_offset - 1 - header));
  std::int64_t pending_expiry = kNoExpiry;

  while (!r.eof()) {
    const std::uint8_t opcode = r.u8();
    if (!r.ok()) return fail(error, "truncated while reading opcode");

    switch (opcode) {
      case kOpAux: {
        r.str(error);
        r.str(error);
        break;
      }
      case kOpSelectDb: {
        bool enc = false;
        std::uint8_t e = 0;
        r.length(&enc, &e);
        break;
      }
      case kOpResizeDb: {
        bool enc = false;
        std::uint8_t e = 0;
        r.length(&enc, &e);
        r.length(&enc, &e);
        break;
      }
      case kOpExpireMs:
        pending_expiry = static_cast<std::int64_t>(r.u64_le());
        break;
      case kOpExpireSec:
        pending_expiry = static_cast<std::int64_t>(r.u32_le()) * 1000;
        break;
      case kTypeString: {
        Record record;
        record.key = r.str(error);
        record.value = r.str(error);
        record.expire_at = pending_expiry;
        pending_expiry = kNoExpiry;
        if (!r.ok()) {
          return fail(error, error && !error->empty()
                                 ? *error
                                 : "truncated while reading a key");
        }
        out->push_back(std::move(record));
        break;
      }
      default:
        return fail(error, "unsupported opcode 0x" +
                               std::to_string(static_cast<int>(opcode)) +
                               " (only string values are supported)");
    }

    if (!r.ok()) {
      return fail(error, (error && !error->empty()) ? *error
                                                   : "truncated payload");
    }
  }

  return true;
}

bool rdb_save_file(const std::string& path, const std::vector<Record>& records,
                   std::string* error) {
  const std::string payload = rdb_serialize(records);
  const std::string temp = path + ".tmp-" + std::to_string(
#if defined(_WIN32)
                                                _getpid()
#else
                                                ::getpid()
#endif
                                            );

  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) return fail(error, "cannot open " + temp + " for writing");
    file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    file.flush();
    if (!file) {
      std::remove(temp.c_str());
      return fail(error, "write failed for " + temp);
    }
  }

  // fsync the data before the rename, or a crash can leave the rename durable
  // while the contents are not.
#if !defined(_WIN32)
  const int fd = ::open(temp.c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
#endif

  std::remove(path.c_str());  // Windows rename() refuses an existing target
  if (std::rename(temp.c_str(), path.c_str()) != 0) {
    std::remove(temp.c_str());
    return fail(error, "rename into place failed for " + path);
  }
  return true;
}

bool rdb_load_file(const std::string& path, std::vector<Record>* out,
                   std::string* error) {
  out->clear();
  std::ifstream file(path, std::ios::binary);
  if (!file) return true;  // no snapshot yet: a fresh server, not an error

  const std::string payload((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
  if (payload.empty()) return true;
  return rdb_parse(payload, out, error);
}

}  // namespace rp
