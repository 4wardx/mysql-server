#ifndef ZSET_HASHTABLE_H
#define ZSET_HASHTABLE_H

#include <string>
#include <unordered_map>

#include "my_inttypes.h"

struct ZNode;

// Hash table member -> ZNode for O(1) point lookups.
class ZsetHashtable {
 public:
  ZsetHashtable() = default;
  ~ZsetHashtable() = default;

  ZsetHashtable(const ZsetHashtable &) = delete;
  ZsetHashtable &operator=(const ZsetHashtable &) = delete;

  // Add or replace the entry for member. Copies member bytes.
  void insert(const uchar *member, uint len, ZNode *node);

  // Fetch the node for member, or nullptr if absent.
  ZNode *lookup(const uchar *member, uint len) const;

  // Remove the entry for member. No-op if absent.
  void remove(const uchar *member, uint len);

  // Clear all entries.
  void clear();

  // Number of entries, tombstones included.
  size_t count() const;

  // Number of entries whose newest version is a PUT.
  size_t count_live() const;

 private:
  std::unordered_map<std::string, ZNode *> entries_;  // Member -> node
};

#endif  // ZSET_HASHTABLE_H
