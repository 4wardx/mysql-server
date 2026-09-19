#ifndef ZSET_LSM_H
#define ZSET_LSM_H

#include <memory>
#include <string>
#include <vector>

#include "my_inttypes.h"

#include "storage/zset/zset_memtable.h"
#include "storage/zset/zset_sstable.h"
#include "storage/zset/zset_wal.h"

// The ZSET engine core: one sorted set per table, held in a versioned
// memtable plus zero or more immutable sstable files, all fronted by the
// write-ahead log.
//
// The handler talks only to this class. Writes are WAL-first: the record
// is appended (and fsynced) before the memtable changes, so a crash
// replays the log. When the memtable grows past a threshold, flush()
// freezes its internal keys into a new <table>-<seq>.sst and resets the
// log.
//
// Point reads go through the memtable's hash table first (O(1) for rows
// still in memory); rows already flushed are found by a merged scan. The
// ordered scan is a k-way merge over the memtable and every sstable, with
// an optional max_seq watermark for snapshot reads.

class ZsetLSM {
 public:
  ZsetLSM();
  ~ZsetLSM();

  ZsetLSM(const ZsetLSM &) = delete;
  ZsetLSM &operator=(const ZsetLSM &) = delete;

  // Load the sstable files for this table and replay the WAL.
  int open(const char *name);

  // Release the sstables and the log.
  void close();

  // Write a version for the member, WAL-first.
  void put(double score, const uchar *member, uint len);

  // Delete the member (writes a tombstone), WAL-first.
  void del(double score, const uchar *member, uint len);

  // Drop every row: a CLEAR marker in the log, empty memtable, and the
  // sstable files are removed.
  int clear();

  // Live score for the member, or false if absent or deleted.
  bool get(const uchar *member, uint len, double *score) const;

  // Number of live rows across the memtable and all sstables.
  size_t count() const;

  // Freeze the memtable's internal keys into a new sstable and reset the
  // memtable and the log.
  int flush();

  // Next free sequence number, used as the scan snapshot watermark.
  uint64 sequence() const { return sequence_; }

  // Internal keys held in the memtable (drives the flush trigger).
  size_t mem_size() const { return mem_->skiplist()->count(); }

  // A decoded read key, from either the memtable or an sstable.
  struct Key {
    double score;
    std::string member;  // std::string keeps short members allocation-free
    uint64 sequence;
    ZsetType type;  // PUT or DELETE
  };

  // Merged live-view iterator over the memtable and all sstables.
  class Iterator {
   public:
    // Position at the first live key as of max_seq.
    void seekToFirst(const ZsetLSM *lsm, uint64 max_seq);

    // Position at the first live key >= the target user key.
    void seek(const ZsetLSM *lsm, double score, const uchar *member, uint len,
              uint64 max_seq);

    // True while the iterator points at a live key.
    bool valid() const { return valid_; }

    // Advance to the next live key.
    void next();

    // The current live key; valid only while valid() is true.
    const Key &key() const { return key_; }

   private:
    struct Source {
      // Memtable cursor over every internal key.
      ZNode *node = nullptr;
      // Per-sstable cursor.
      ZsetSSTableReader::Iterator sst;
      uint64 sst_index = 0;  // 0 = memtable, 1..N = sstable
    };

    // True if the source currently points at an internal key.
    bool valid_source(const Source &s) const;

    // Read the source's current internal key.
    Key current_key(const Source &s) const;

    // Advance the source one internal key.
    void advance(Source *s);

    // The source with the smallest internal key, or nullptr.
    Source *peek_leader();

    // True if the two keys share the same user key (score, member).
    static bool same_user_key(const Key &a, const Key &b);

    // Advance every source past the given user key.
    void skip_user_key(const Key &k);

    // Position every source at the first internal key >= the target.
    void seek_sources(double score, const uchar *member, uint len,
                      uint64 sequence, ZsetType type);

    // Move to the next live key in the merged view.
    void find_next_live();

    const ZsetLSM *lsm_ = nullptr;  // Owning engine core
    // Memtable snapshot. Holding the shared_ptr keeps the memtable (and
    // every ZNode the cursor points at) alive even if a flush replaces
    // the live memtable mid-scan.
    std::shared_ptr<ZsetMemTable> mem_;
    uint64 max_sequence_ = ~0ULL;  // Snapshot watermark
    std::vector<Source> sources_;  // One cursor per source
    bool valid_ = false;           // True while key_ holds the current live key
    Key key_;                      // The current live key
  };

 private:
  // Versioned memtable (skiplist + live-view hash). Replaced, not
  // cleared, by flush(): scans hold the previous one until they finish.
  std::shared_ptr<ZsetMemTable> mem_;
  std::vector<ZsetSSTableReader *> ssts_;  // Loaded sstable readers
  Zset_wal wal_;                           // Write-ahead log
  uint64 sequence_ = 0;                    // Sequence counter for internal keys
  std::string name_;                       // Table path, for sstable file names
};

#endif  // ZSET_LSM_H
