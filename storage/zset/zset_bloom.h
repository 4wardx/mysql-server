#ifndef ZSET_BLOOM_H
#define ZSET_BLOOM_H

#include <vector>

#include "my_inttypes.h"

// A minimal bloom filter over member keys, one per sstable. A query for a
// member that was never added always returns false; one that was added
// returns true. False positives are possible, false negatives are not,
// so the filter safely skips files during point lookups and duplicate
// checks.
//
// On-disk form: m_bits(4B LE) | k(1B) | bit array.

class ZsetBloomFilter {
 public:
  ZsetBloomFilter() = default;

  // Size the filter for num_entries members (about 8 bits each).
  void init(uint32_t num_entries);

  // Add a member by its double hash.
  void add_hash(uint64 h1, uint64 h2);

  // Query a member by its double hash.
  bool may_contain_hash(uint64 h1, uint64 h2) const;

  // The two independent hashes of a member.
  static void hash(const uchar *member, uint len, uint64 *h1, uint64 *h2);

  // Add a member.
  void add(const uchar *member, uint len);

  // Query a member.
  bool may_contain(const uchar *member, uint len) const;

  // Bytes the filter occupies on disk.
  size_t byte_size() const { return 5 + bits_.size(); }

  // Serialize into out (must hold byte_size() bytes).
  void serialize(uchar *out) const;

  // Deserialize from data; returns bytes consumed, or 0 on error.
  size_t deserialize(const uchar *data, size_t len);

 private:
  // The k-bit double-hashing positions for a key.
  void set_bits(uint64 h1, uint64 h2);
  bool test_bits(uint64 h1, uint64 h2) const;

  std::vector<uchar> bits_;  // Bit array
  uint32_t m_bits_ = 0;      // Number of bits
  uint8_t k_ = 0;            // Number of hash functions
};

#endif  // ZSET_BLOOM_H
