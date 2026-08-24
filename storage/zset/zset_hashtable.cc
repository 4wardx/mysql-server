#include <string>

#include "storage/zset/zset_hashtable.h"
#include "storage/zset/zset_skiplist.h"

void ZsetHashtable::insert(const uchar *member, uint len, ZNode *node) {
  std::string key((const char *)member, len);
  entries_[key] = node;
}

ZNode *ZsetHashtable::lookup(const uchar *member, uint len) const {
  std::string key((const char *)member, len);
  auto it = entries_.find(key);
  return it == entries_.end() ? nullptr : it->second;
}

void ZsetHashtable::remove(const uchar *member, uint len) {
  std::string key((const char *)member, len);
  entries_.erase(key);
}

void ZsetHashtable::clear() { entries_.clear(); }

size_t ZsetHashtable::count() const { return entries_.size(); }
