#ifndef ZSET_WAL_H
#define ZSET_WAL_H

#include "my_inttypes.h"
#include "my_sys.h"
#include "storage/zset/zset_memtable.h"

// Write-ahead log for the ZSET memtable.
//
// Write ordering follows WAL: every mutation is appended (and fsynced,
// per zset_wal_fsync) BEFORE the memtable is updated, so the log is the
// authority after a crash and startup replay() rebuilds the memtable.
//
// Record format (little-endian):
//   magic(2B, 0x5A53) | type(1B) | seq(8B) | score(8B) |
//   member_len(2B) | member
//   type: 1=PUT 2=DELETE 3=CLEAR
//
// CLEAR marks all prior records as void (TRUNCATE / delete-all): the
// log stays append-only and the clear is crash-durable once fsynced.
class Zset_wal {
 public:
  // Record types.
  enum class Type : uint8_t {
    kPut = 1,
    kDelete = 2,
    kClear = 3,
  };
  static constexpr uint16_t kMagic = 0x5A53;

  Zset_wal() = default;
  ~Zset_wal();

  Zset_wal(const Zset_wal &) = delete;
  Zset_wal &operator=(const Zset_wal &) = delete;

  // Open the log for appending; creates the file if absent.
  int open(const char *path);

  // Close the log.
  int close();

  // Append one mutation record with its sequence number. Returns 0.
  int append(double score, const uchar *member, uint len, uint64 seq,
             Type type);

  // Append a CLEAR record that voids all prior records.
  int append_clear();

  // Rebuild the memtable by replaying the log and set *next_seq to the
  // next free sequence number. Returns the number of records applied;
  // stops at a corrupted or truncated tail.
  size_t replay(ZsetMemTable *mem, uint64 *next_seq);

  // True if the log file is open.
  bool is_open() const { return fd_ >= 0; }

 private:
  File fd_ = -1;
};

#endif  // ZSET_WAL_H
