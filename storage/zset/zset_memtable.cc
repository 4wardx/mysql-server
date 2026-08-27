#include "storage/zset/zset_memtable.h"

#include <cstring>

ZNode *ZsetMemTable::put(double score, const uchar *member, uint len,
                         uint64 seq) {
  ZNode *node = skiplist_.insert(score, member, len, seq, ZsetType::kPut);
  hashtable_.insert(member, len, node);
  return node;
}

void ZsetMemTable::tombstone(double score, const uchar *member, uint len,
                             uint64 seq) {
  skiplist_.insert(score, member, len, seq, ZsetType::kDelete);
  hashtable_.remove(member, len);
}

bool ZsetMemTable::get(const uchar *member, uint len, double *score) const {
  ZNode *node = hashtable_.lookup(member, len);
  if (node == nullptr) {
    return false;
  }
  *score = node->score;
  return true;
}

ZNode *ZsetMemTable::lookup(const uchar *member, uint len) const {
  return hashtable_.lookup(member, len);
}

size_t ZsetMemTable::count() const { return hashtable_.count(); }

void ZsetMemTable::clear() {
  hashtable_.clear();
  skiplist_.clear();
}

ZNode *ZsetMemTable::firstLive(uint64 max_seq) const {
  ZNode *n = skiplist_.first();
  while (n != nullptr) {
    ZNode *active = activeVersion(n, max_seq);
    if (active != nullptr && active->type == ZsetType::kPut) {
      return active;
    }
    // Tombstoned or entirely newer than max_seq: skip the whole group.
    ZNode *run = n;
    n = skiplist_.next(n);
    while (n != nullptr && sameUserKey(n, run)) {
      n = skiplist_.next(n);
    }
  }
  return nullptr;
}

ZNode *ZsetMemTable::nextLive(const ZNode *cur, uint64 max_seq) const {
  ZNode *n = cur == nullptr ? skiplist_.first() : skiplist_.next(cur);
  while (n != nullptr) {
    // Skip shadowed older versions of the current user key.
    if (cur != nullptr && sameUserKey(n, cur)) {
      n = skiplist_.next(n);
      continue;
    }
    // n is the newest internal key of its user key.
    ZNode *active = activeVersion(n, max_seq);
    if (active != nullptr && active->type == ZsetType::kPut) {
      return active;
    }
    // Tombstoned or entirely newer than max_seq: skip the whole group.
    ZNode *run = n;
    n = skiplist_.next(n);
    while (n != nullptr && sameUserKey(n, run)) {
      n = skiplist_.next(n);
    }
  }
  return nullptr;
}

ZNode *ZsetMemTable::lastLive(uint64 max_seq) const {
  ZNode *n = skiplist_.last();
  while (n != nullptr) {
    ZNode *newest = newestVersion(n);
    ZNode *active = activeVersion(newest, max_seq);
    if (active != nullptr && active->type == ZsetType::kPut) {
      return active;
    }
    n = skiplist_.prev(newest);
  }
  return nullptr;
}

ZNode *ZsetMemTable::prevLive(const ZNode *cur, uint64 max_seq) const {
  ZNode *n = cur == nullptr ? skiplist_.last() : skiplist_.prev(cur);
  while (n != nullptr) {
    // n may be a newer-than-max_seq version of cur's own user key (the
    // group front sits before cur in the chain); skip to the previous
    // user key.
    if (cur != nullptr && sameUserKey(n, cur)) {
      ZNode *front = newestVersion(n);
      n = skiplist_.prev(front);
      continue;
    }
    ZNode *newest = newestVersion(n);
    ZNode *active = activeVersion(newest, max_seq);
    if (active != nullptr && active->type == ZsetType::kPut) {
      return active;
    }
    n = skiplist_.prev(newest);
  }
  return nullptr;
}

ZNode *ZsetMemTable::seekLive(double score, const uchar *member, uint len,
                              uint64 max_seq) const {
  ZsetSkiplist::Cursor cur(&skiplist_);
  // Seek to the first internal key >= (score, member) at max seq, then
  // advance to the first live node.
  cur.seekTo(score, member, len);
  ZNode *n = cur.valid() ? cur.current() : nullptr;
  while (n != nullptr) {
    ZNode *active = activeVersion(n, max_seq);
    if (active != nullptr && active->type == ZsetType::kPut) {
      return active;
    }
    // Tombstoned or entirely newer than max_seq: skip the whole group.
    ZNode *run = n;
    n = skiplist_.next(n);
    while (n != nullptr && sameUserKey(n, run)) {
      n = skiplist_.next(n);
    }
  }

  return nullptr;
}

size_t ZsetMemTable::countLiveInRange(double min, double max) const {
  size_t n = 0;
  for (ZNode *x = firstLive(); x != nullptr; x = nextLive(x)) {
    if (x->score >= min && x->score <= max) {
      n++;
    }
  }
  return n;
}

bool ZsetMemTable::sameUserKey(const ZNode *a, const ZNode *b) {
  return a->score == b->score && a->member_len == b->member_len &&
         memcmp(a->member, b->member, a->member_len) == 0;
}

ZNode *ZsetMemTable::newestVersion(ZNode *n) const {
  ZNode *newest = n;
  ZNode *p = skiplist_.prev(newest);
  while (p != nullptr && sameUserKey(newest, p)) {
    newest = p;
    p = skiplist_.prev(newest);
  }
  return newest;
}

ZNode *ZsetMemTable::activeVersion(ZNode *n, uint64 max_seq) const {
  ZNode *active = n;
  while (active != nullptr && active->seq > max_seq) {
    active = skiplist_.next(active);
    if (active != nullptr && !sameUserKey(active, n)) {
      return nullptr;  // every version is newer than max_seq
    }
  }
  return active;
}
