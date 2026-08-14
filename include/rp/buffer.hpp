#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace rp {

// A growable byte buffer with an independent read cursor.
//
// Phase 1 exists largely because the legacy server had no such thing: it read
// into a fixed 1024-byte stack array and assumed one recv() == one complete
// command. Every partial-read, pipelining and large-value bug traces back to
// that. Bytes accumulate here until a parser (Phase 2) consumes them.
class Buffer {
 public:
  Buffer() = default;
  explicit Buffer(std::size_t reserve) { data_.reserve(reserve); }

  void append(const char* p, std::size_t n) {
    if (n == 0) return;
    reclaim();
    data_.insert(data_.end(), p, p + n);
  }
  void append(std::string_view s) { append(s.data(), s.size()); }

  // Bytes written but not yet consumed.
  std::string_view readable() const {
    return std::string_view(data_.data() + read_pos_, size());
  }

  // Mark n bytes as processed. n is clamped to the readable size.
  void consume(std::size_t n) {
    read_pos_ += (n > size() ? size() : n);
    if (read_pos_ == data_.size()) clear();
  }

  std::size_t size() const { return data_.size() - read_pos_; }
  bool empty() const { return size() == 0; }
  std::size_t capacity() const { return data_.capacity(); }

  void clear() {
    data_.clear();
    read_pos_ = 0;
  }

 private:
  // Drop already-consumed bytes once they dominate the allocation, so a long
  // lived connection doing many small commands does not grow without bound.
  void reclaim() {
    if (read_pos_ == 0) return;
    if (read_pos_ < data_.size() / 2 && read_pos_ < kReclaimThreshold) return;
    const std::size_t remaining = size();
    if (remaining > 0) {
      std::memmove(data_.data(), data_.data() + read_pos_, remaining);
    }
    data_.resize(remaining);
    read_pos_ = 0;
  }

  static constexpr std::size_t kReclaimThreshold = 8 * 1024;

  std::vector<char> data_;
  std::size_t read_pos_ = 0;
};

}  // namespace rp
