#include <cstring>

#include "storage/zset/zset_bloom.h"

namespace {

// Two independent 64-bit hashes of the member bytes (FNV-1a, two seeds).
void member_hashes(const uchar *member, uint len, uint64 *h1, uint64 *h2) {
  uint64 a = 0xcbf29ce484222325ULL;
  uint64 b = 0x84222325cbf29ce4ULL;
  const uint64 kPrime = 0x100000001b3ULL;
  for (uint i = 0; i < len; i++) {
    a ^= member[i];
    a *= kPrime;
    b ^= member[i];
    b *= 0x9e3779b97f4a7c15ULL;  // a different multiplier
  }
  *h1 = a;
  *h2 = b;
}

}  // namespace

void ZsetBloomFilter::init(uint32_t num_entries) {
  // 16 bits per entry keeps the false-positive rate under ~0.1%, so point
  // lookups and duplicate checks rarely pay for a confirmatory scan.
  m_bits_ = num_entries * 16;
  if (m_bits_ < 128) {
    m_bits_ = 128;
  }
  k_ = 11;  // near optimal for 16 bits per entry
  bits_.assign((m_bits_ + 7) / 8, 0);
}

void ZsetBloomFilter::add_hash(uint64 h1, uint64 h2) { set_bits(h1, h2); }

bool ZsetBloomFilter::may_contain_hash(uint64 h1, uint64 h2) const {
  return test_bits(h1, h2);
}

void ZsetBloomFilter::hash(const uchar *member, uint len, uint64 *h1,
                           uint64 *h2) {
  member_hashes(member, len, h1, h2);
}

void ZsetBloomFilter::add(const uchar *member, uint len) {
  uint64 h1, h2;
  member_hashes(member, len, &h1, &h2);
  set_bits(h1, h2);
}

bool ZsetBloomFilter::may_contain(const uchar *member, uint len) const {
  uint64 h1, h2;
  member_hashes(member, len, &h1, &h2);
  return test_bits(h1, h2);
}

void ZsetBloomFilter::set_bits(uint64 h1, uint64 h2) {
  for (uint8_t i = 0; i < k_; i++) {
    const uint64 pos = (h1 + i * h2) % m_bits_;
    bits_[pos / 8] |= static_cast<uchar>(1 << (pos % 8));
  }
}

bool ZsetBloomFilter::test_bits(uint64 h1, uint64 h2) const {
  for (uint8_t i = 0; i < k_; i++) {
    const uint64 pos = (h1 + i * h2) % m_bits_;
    if ((bits_[pos / 8] & static_cast<uchar>(1 << (pos % 8))) == 0) {
      return false;
    }
  }
  return true;
}

void ZsetBloomFilter::serialize(uchar *out) const {
  out[0] = static_cast<uchar>(m_bits_ & 0xFF);
  out[1] = static_cast<uchar>((m_bits_ >> 8) & 0xFF);
  out[2] = static_cast<uchar>((m_bits_ >> 16) & 0xFF);
  out[3] = static_cast<uchar>((m_bits_ >> 24) & 0xFF);
  out[4] = k_;
  memcpy(out + 5, bits_.data(), bits_.size());
}

size_t ZsetBloomFilter::deserialize(const uchar *data, size_t len) {
  if (len < 5) {
    return 0;
  }
  m_bits_ = static_cast<uint32_t>(data[0]) |
            (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16) |
            (static_cast<uint32_t>(data[3]) << 24);
  k_ = data[4];
  const size_t nbytes = (m_bits_ + 7) / 8;
  if (len < 5 + nbytes) {
    return 0;
  }
  bits_.assign(data + 5, data + 5 + nbytes);
  return 5 + nbytes;
}
