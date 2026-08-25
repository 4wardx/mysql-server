#ifndef ZSET_MEMTABLE_H
#define ZSET_MEMTABLE_H

#include "my_inttypes.h"
#include "storage/zset/zset_hashtable.h"
#include "storage/zset/zset_skiplist.h"

// Versioned memtable for ZSET.
//
// Every write appends a versioned internal key to the skiplist; nothing
// is overwritten in place. An internal key is (score, member, seq, type):
//   - score, member: the user key (sorted-set member and its score)
//   - seq:           write sequence number, strictly increasing per table
//   - type:          kPut or kDelete (a delete tombstone)
//
// All versions of a member, including old values and delete tombstones,
// stay in the skiplist, ordered by user key with the newest version of
// each user key first. Deletion therefore never erases history: it
// writes a kDelete tombstone whose seq supersedes every earlier version,
// and a scan treats a user key as gone only when its newest version is a
// tombstone. Old versions are only reclaimed by compaction.
//
// The hashtable mirrors the current live view (member -> newest kPut
// node), giving O(1) point lookups, duplicate detection, and the score
// that an update must tombstone. The *_Live iterators expose the merged
// view: one node per live user key, in key order, newest version wins.
//
// Example after: PUT(1,a) PUT(2,b) PUT(3,c) DEL(3,c) PUT(30,c) DEL(2,b)
//
//   skiplist (all versions, ordered by score, member, seq desc):
//     head -> (1,a,s1,P) -> (2,b,s6,D) -> (2,b,s2,P) -> (3,c,s4,D)
//          -> (3,c,s3,P) -> (30,c,s5,P) -> null
//     newest version of each user key first; b's tombstone (s6) and
//     c's tombstone (s4) shadow every earlier version of that key.
//
//   hashtable (live view, member -> newest PUT node):
//     a -> (1,a,s1,P)      b -> removed       c -> (30,c,s5,P)
//
//   merged scan (firstLive/nextLive): a(1) -> c(30) -> null
//     b is skipped because its newest version is a tombstone.
class ZsetMemTable {
 public:
  ZsetMemTable() = default;
  ~ZsetMemTable() = default;

  ZsetMemTable(const ZsetMemTable &) = delete;
  ZsetMemTable &operator=(const ZsetMemTable &) = delete;

  // Record a PUT for the member and return its node.
  ZNode *put(double score, const uchar *member, uint len, uint64 seq);

  // Record a delete tombstone for the member.
  void tombstone(double score, const uchar *member, uint len, uint64 seq);

  // Live score for the member, or false if absent or deleted.
  bool get(const uchar *member, uint len, double *score) const;

  // Live node for the member, or nullptr if absent or deleted.
  ZNode *lookup(const uchar *member, uint len) const;

  // Number of live rows.
  size_t count() const;

  // Drop all versions and tombstones.
  void clear();

  // First live node in key order, or nullptr.
  ZNode *firstLive() const;

  // Next live node after cur in key order, or nullptr.
  ZNode *nextLive(const ZNode *cur) const;

  // Last live node in key order, or nullptr.
  ZNode *lastLive() const;

  // Previous live node before cur in key order, or nullptr.
  ZNode *prevLive(const ZNode *cur) const;

  // First live node whose key is at or after the target, or nullptr.
  ZNode *seekLive(double score, const uchar *member, uint len) const;

  // Live rows whose score falls in the range, for the optimizer.
  size_t countLiveInRange(double min, double max) const;

  // Ordered index holding every version and tombstone.
  ZsetSkiplist *skiplist() { return &skiplist_; }
  const ZsetSkiplist *skiplist() const { return &skiplist_; }

  // Live-view hash table (member -> newest PUT node).
  ZsetHashtable *hashtable() { return &hashtable_; }
  const ZsetHashtable *hashtable() const { return &hashtable_; }

 private:
  // True if the two nodes share the same user key (score, member).
  static bool sameUserKey(const ZNode *a, const ZNode *b);

  // Newest internal key sharing n's user key, i.e. the highest seq
  // among all versions of the same (score, member). Returns n itself
  // when the versioned walk lands on an older version first.
  ZNode *newestVersion(ZNode *n) const;

  ZsetSkiplist skiplist_;    // All versions and tombstones
  ZsetHashtable hashtable_;  // Live view: member -> newest PUT node
};

#endif  // ZSET_MEMTABLE_H
