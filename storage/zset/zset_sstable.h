#ifndef ZSET_SSTABLE_H
#define ZSET_SSTABLE_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "my_inttypes.h"
#include "my_sys.h"

#include "storage/zset/zset_bloom.h"
#include "storage/zset/zset_skiplist.h"

// Immutable sorted tables (SST) for the ZSET LSM.
//
// An internal key is byte-sortable and matches the in-memory order.
// Every multi-byte field is big-endian, so a byte-wise memcmp yields the
// engine's key order:
//
//   sort-encoded score(8) | member_len(2) | member | ~seq(8) | type(1)
//
//   - score: IEEE754 order-preserving transform, so byte order equals
//     numeric order
//   - member: 2-byte length prefix + raw bytes
//   - ~seq: bitwise complement, so the newest version sorts first
//   - type: 1=PUT 2=DELETE, ascending
//
// File layout:
//
//   [data block]... [index block] [footer]
//
//   data block: length-prefixed internal keys, plus a restart array every
//               kRestartEvery entries for block-local binary search
//   index block: one entry per data block: first key + offset + length
//   footer: fixed size, points at the index block
//
// Writer keys must arrive in ascending order (a flush iterates the
// memtable, which is already sorted).

// Order-preserving transforms for the score. For a negative value, the
// complement makes it sort before all positives; for a positive value the
// sign bit is set so it sorts after all negatives. Byte order therefore
// equals numeric order: -inf < -1 < 0 < 1 < +inf.
inline uint64 zset_encode_score(double score) {
  uint64 u;
  memcpy(&u, &score, sizeof(u));
  return u ^ (static_cast<int64_t>(u) >> 63 | 0x8000000000000000ULL);
}

inline double zset_decode_score(uint64 encoded) {
  const bool negative = (encoded >> 63) == 0;
  const uint64 u = negative ? ~encoded : encoded & 0x7FFFFFFFFFFFFFFFULL;
  double score;
  memcpy(&score, &u, sizeof(score));
  return score;
}

// One member index entry: truncated member hash -> data block number.
// The reader scans just the blocks listed for a member's hash, so point
// lookups never walk the whole file.
struct ZsetMemberIndexEntry {
  uint32_t hash;
  uint32_t block;
};

class ZsetSSTableWriter {
 public:
  ZsetSSTableWriter();
  ~ZsetSSTableWriter();

  ZsetSSTableWriter(const ZsetSSTableWriter &) = delete;
  ZsetSSTableWriter &operator=(const ZsetSSTableWriter &) = delete;

  // Create or truncate the output file.
  int open(const char *path);

  // Append one internal key; must be >= the previous key.
  int append(double score, const uchar *member, uint len, uint64 sequence,
             ZsetType type);

  // Flush the last block, write the index block and footer, then close.
  int finish();

  // Bytes written so far.
  uint64 size() const { return size_; }

 private:
  static constexpr size_t kBlockSize = 4096;
  static constexpr size_t kRestartEvery = 16;
  static constexpr size_t kMaxRestarts = 512;

  struct IndexEntry {
    std::vector<uchar> first_key;  // First internal key of the block
    uint64 offset;                 // Block offset in the file
    uint32_t length;               // Block length in bytes
  };

  // Flush the current data block and remember its index entry.
  int flush_block();

  File fd_ = -1;     // Output fd
  uint64 size_ = 0;  // Bytes written so far

  std::vector<uchar> block_;  // Current data block, grows past kBlockSize
  size_t block_len_ = 0;      // Bytes used in block_
  size_t block_entries_ = 0;  // entries since the last flush
  uint16_t restart_[kMaxRestarts];
  size_t restart_count_ = 0;

  std::vector<IndexEntry> index_;  // Per-block index entries
  std::vector<std::pair<uint64, uint64>> member_hashes_;  // For the filter
  // Member index: (truncated hash, block) pairs, one per member per
  // block that holds it, deduplicated on adjacent appends.
  std::vector<ZsetMemberIndexEntry> member_index_;
  uint32_t last_index_hash_ = 0;     // Last recorded member hash
  uint32_t last_index_block_ = ~0U;  // Last recorded block (sentinel: none)
  bool finished_ = false;            // finish() completed
};

// Sequential and seeking read access to a written SST.
class ZsetSSTableReader {
 public:
  ZsetSSTableReader();
  ~ZsetSSTableReader();

  ZsetSSTableReader(const ZsetSSTableReader &) = delete;
  ZsetSSTableReader &operator=(const ZsetSSTableReader &) = delete;

  // Open an SST; reads the footer and the whole index block.
  int open(const char *path);

  // Close the sstable and release the loaded index.
  void close();

  // Read the block at (offset, length), serving hits from the in-memory
  // block cache so repeated scans do not pay per-block syscalls.
  bool read_block(uint64_t offset, uint32_t length, std::vector<uchar> *out);

  // True if the member may live in this sstable (bloom filter).
  bool may_contain(const uchar *member, uint len) const;

  // Newest version of the member in this sstable, using the member index
  // to read only the blocks that can hold it (no full scan). Returns
  // false when the member has no version in this file.
  bool find_member(const uchar *member, uint len, uint64 *sequence,
                   ZsetType *type, double *score);

  // One decoded internal key.
  struct Entry {
    double score;               // User key score
    std::vector<uchar> member;  // User key member
    uint64 sequence;            // Version sequence number
    ZsetType type;              // PUT or DELETE
  };

  // Sequential iterator over every entry, in order.
  class Iterator {
   public:
    // Iterate from the start.
    void seekToFirst(ZsetSSTableReader *reader);

    // Iterate from the first entry whose internal key is >= the target.
    void seek(ZsetSSTableReader *reader, double score, const uchar *member,
              uint len, uint64 sequence, ZsetType type);

    // True while the iterator points at a decoded entry.
    bool valid() const { return valid_; }

    // Advance to the next entry.
    void next();

    // The current entry; valid only while valid() is true.
    const Entry &entry() const { return entry_; }

   private:
    // Load the block at (offset, length) and position within it.
    bool load_block(ZsetSSTableReader *reader, uint64 offset, uint32_t length);

    // Decode the entry at entry_pos_, advancing entry_end_.
    bool parse_entry();

    ZsetSSTableReader *reader_ = nullptr;  // Owning reader
    size_t index_pos_ = 0;      // Current block in the reader's index
    std::vector<uchar> block_;  // The loaded block
    size_t block_len_ = 0;      // Entries region, excluding the restart array
    size_t entry_pos_ = 0;      // Offset of the current entry in block_
    size_t entry_end_ = 0;      // End of the current entry
    bool valid_ = false;        // True while entry_ holds a decoded key
    Entry entry_;               // The current decoded entry
  };

 private:
  struct IndexEntry {
    std::vector<uchar> first_key;  // First internal key of the block
    uint64 offset;                 // Block offset in the file
    uint32_t length;               // Block length in bytes
  };

  // Binary search the index for the block whose first key is the largest
  // <= the target internal key.
  size_t find_block(double score, const uchar *member, uint len,
                    uint64 sequence, ZsetType type) const;

  // Scan one data block for every version of the member, keeping the
  // newest. Used by find_member.
  bool scan_block_for_member(uint32_t block, const uchar *member, uint len,
                             uint64 *sequence, ZsetType *type, double *score,
                             bool *found);

  ZsetBloomFilter filter_;                          // Member bloom filter
  File fd_ = -1;                                    // Opened sstable fd
  std::vector<IndexEntry> index_;                   // Loaded index block
  std::vector<ZsetMemberIndexEntry> member_index_;  // Loaded member index
  std::unordered_map<uint64_t, std::vector<uchar>> block_cache_;  // Read blocks
};

#endif  // ZSET_SSTABLE_H
