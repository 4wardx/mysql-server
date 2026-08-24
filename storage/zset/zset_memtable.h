#ifndef ZSET_MEMTABLE_H
#define ZSET_MEMTABLE_H

#include "my_inttypes.h"

#include "storage/zset/zset_hashtable.h"
#include "storage/zset/zset_skiplist.h"

// MemTable for ZSET: dual indexes like Redis.
// Skiplist ordered by (score, member) for range scans,
// hashtable member -> ZNode for O(1) point lookups.
// Both indexes are kept consistent on every mutation.
class ZsetMemTable {
 public:
  ZsetMemTable();
  ~ZsetMemTable();

  ZsetMemTable(const ZsetMemTable &) = delete;
  ZsetMemTable &operator=(const ZsetMemTable &) = delete;

  // Add a node to both indexes. The caller must ensure member is new.
  ZNode *add(double score, const uchar *member, uint len);

  // Point lookup by member. Returns node or nullptr.
  ZNode *get(const uchar *member, uint len) const;

  // Remove by member. Uses the hashtable to find the node, then erases
  // from both indexes. No-op if absent.
  void remove(const uchar *member, uint len);

  // Clear both indexes.
  void clear();

  // Number of entries.
  size_t count() const;

  // Skiplist access for ordered scans.
  ZsetSkiplist *skiplist() { return &skiplist_; }
  const ZsetSkiplist *skiplist() const { return &skiplist_; }

  // Hashtable access for point lookups.
  ZsetHashtable *hashtable() { return &hashtable_; }
  const ZsetHashtable *hashtable() const { return &hashtable_; }

 private:
  ZsetSkiplist skiplist_;    // Ordered index
  ZsetHashtable hashtable_;  // Point index
};

#endif  // ZSET_MEMTABLE_H
