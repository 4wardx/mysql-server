#include "storage/zset/zset_memtable.h"

ZsetMemTable::ZsetMemTable() = default;
ZsetMemTable::~ZsetMemTable() = default;

ZNode *ZsetMemTable::add(double score, const uchar *member, uint len) {
  ZNode *node = skiplist_.insert(score, member, len);
  hashtable_.insert(member, len, node);
  return node;
}

ZNode *ZsetMemTable::get(const uchar *member, uint len) const {
  return hashtable_.lookup(member, len);
}

void ZsetMemTable::remove(const uchar *member, uint len) {
  ZNode *node = hashtable_.lookup(member, len);
  if (node == nullptr) {
    return;
  }
  skiplist_.remove(member, len, node->score);
  hashtable_.remove(member, len);
}

void ZsetMemTable::clear() {
  hashtable_.clear();
  skiplist_.clear();
}

size_t ZsetMemTable::count() const { return hashtable_.count(); }
