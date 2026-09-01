#include <cstdlib>
#include <cstring>

#include "storage/zset/zset_skiplist.h"

int zset_compare_keys(double score_a, const uchar *member_a, uint len_a,
                      uint64 seq_a, ZsetType type_a, double score_b,
                      const uchar *member_b, uint len_b, uint64 seq_b,
                      ZsetType type_b) {
  if (score_a < score_b) {
    return -1;
  }
  if (score_a > score_b) {
    return 1;
  }

  uint n = len_a < len_b ? len_a : len_b;
  int r = memcmp(member_a, member_b, n);
  if (r != 0) {
    return r;
  }

  if (len_a < len_b) {
    return -1;
  }
  if (len_a > len_b) {
    return 1;
  }

  // Same (score, member): newer sequence first.
  if (seq_a > seq_b) {
    return -1;
  }
  if (seq_a < seq_b) {
    return 1;
  }

  return static_cast<int>(type_a) < static_cast<int>(type_b) ? -1 : 1;
}

ZNode *ZNode::next(int n) const {
  assert(n >= 0);
  return next_[n].load(std::memory_order_acquire);
}

void ZNode::setNext(int n, ZNode *x) const {
  assert(n >= 0);
  next_[n].store(x, std::memory_order_release);
}

ZNode *ZNode::nextRelaxed(int n) const {
  assert(n >= 0);
  return next_[n].load(std::memory_order_relaxed);
}

void ZNode::setNextRelaxed(int n, ZNode *x) const {
  assert(n >= 0);
  next_[n].store(x, std::memory_order_relaxed);
}

ZsetSkiplist::ZsetSkiplist()
    : head_(createNode(0, nullptr, 0, 0, ZsetType::kPut, kMaxHeight)),
      top_level_(1),
      rng_(std::random_device{}()),
      count_(0) {
  // Sentinel spans all levels; member is empty.
  for (int i = 0; i < kMaxHeight; i++) {
    head_->setNext(i, nullptr);
  }
}

ZsetSkiplist::~ZsetSkiplist() {
  clear();
  free(head_->member);
  free(head_);
}

ZNode *ZsetSkiplist::insert(double score, const uchar *member, uint len,
                            uint64 sequence, ZsetType type) {
  ZNode *pred[kMaxHeight];
  findLowerBound(score, member, len, sequence, type, pred);

  int level = randomLevel();
  if (level > currentHeight()) {
    for (int i = currentHeight(); i < level; i++) {
      pred[i] = head_;
    }
    top_level_.store(level, std::memory_order_relaxed);
  }

  ZNode *node = createNode(score, member, len, sequence, type, level);
  for (int i = 0; i < level; i++) {
    node->setNextRelaxed(i, pred[i]->nextRelaxed(i));
    pred[i]->setNext(i, node);
  }

  node->backward = pred[0] == head_ ? nullptr : pred[0];
  if (node->next(0) != nullptr) {
    node->next(0)->backward = node;
  }

  count_++;

  return node;
}

void ZsetSkiplist::clear() {
  ZNode *x = head_->next(0);
  while (x != nullptr) {
    ZNode *next = x->next(0);
    free(x->member);
    free(x);
    x = next;
  }
  for (int i = 0; i < kMaxHeight; i++) {
    head_->setNext(i, nullptr);
  }
  head_->backward = nullptr;
  top_level_.store(1, std::memory_order_relaxed);
  count_ = 0;
}

ZNode *ZsetSkiplist::first() const { return head_->next(0); }

ZNode *ZsetSkiplist::last() const {
  ZNode *x = head_;
  for (int i = currentHeight() - 1; i >= 0; i--) {
    while (x->next(i) != nullptr) {
      x = x->next(i);
    }
  }
  return x == head_ ? nullptr : x;
}

ZNode *ZsetSkiplist::next(const ZNode *n) const {
  return n == nullptr ? nullptr : n->next(0);
}

ZNode *ZsetSkiplist::prev(const ZNode *n) const {
  return n == nullptr ? nullptr : n->backward;
}

size_t ZsetSkiplist::count() const { return count_; }

size_t ZsetSkiplist::countInRange(double min, double max) const {
  size_t n = 0;
  for (ZNode *x = head_->next(0); x != nullptr; x = x->next(0)) {
    if (x->score >= min && x->score <= max) {
      n++;
    }
  }

  return n;
}

ZsetSkiplist::Cursor::Cursor(const ZsetSkiplist *list)
    : list_(list), node_(nullptr) {}

bool ZsetSkiplist::Cursor::valid() const { return node_ != nullptr; }

ZNode *ZsetSkiplist::Cursor::current() const {
  assert(valid());
  return node_;
}

void ZsetSkiplist::Cursor::next() {
  assert(valid());
  node_ = node_->next(0);
}

void ZsetSkiplist::Cursor::prev() {
  assert(valid());
  node_ = node_->backward;
}

void ZsetSkiplist::Cursor::seekTo(double score, const uchar *member, uint len) {
  // Seek to the newest version of the first (score, member) >= target.
  node_ =
      list_->findLowerBound(score, member, len, ~0ULL, ZsetType::kPut, nullptr);
}

void ZsetSkiplist::Cursor::seekToFirst() { node_ = list_->first(); }

void ZsetSkiplist::Cursor::seekToLast() { node_ = list_->last(); }

int ZsetSkiplist::randomLevel() {
  int level = 1;
  static const int kBranch = 4;
  std::uniform_int_distribution<int> dist(0, kBranch - 1);
  while (level < kMaxHeight && dist(rng_) == 0) {
    level++;
  }
  return level;
}

ZNode *ZsetSkiplist::createNode(double score, const uchar *member, uint len,
                                uint64 sequence, ZsetType type, int level) {
  size_t bytes = sizeof(ZNode) + sizeof(std::atomic<ZNode *>) * (level - 1);
  ZNode *node = (ZNode *)malloc(bytes);
  node->score = score;
  node->member = nullptr;
  node->member_len = len;
  node->sequence = sequence;
  node->type = type;
  node->level = level;
  node->backward = nullptr;

  if (len > 0) {
    node->member = (uchar *)malloc(len);
    memcpy(node->member, member, len);
  }

  for (int i = 0; i < level; i++) {
    node->setNextRelaxed(i, nullptr);
  }

  return node;
}

bool ZsetSkiplist::keysEqual(double score_a, const uchar *member_a, uint len_a,
                             double score_b, const uchar *member_b,
                             uint len_b) {
  return score_a == score_b && len_a == len_b &&
         (len_a == 0 || memcmp(member_a, member_b, len_a) == 0);
}

bool ZsetSkiplist::keyGreaterThan(double score, const uchar *member, uint len,
                                  uint64 sequence, ZsetType type, ZNode *n) {
  return n != nullptr &&
         zset_compare_keys(n->score, n->member, n->member_len, n->sequence,
                           n->type, score, member, len, sequence, type) < 0;
}

ZNode *ZsetSkiplist::findLowerBound(double score, const uchar *member, uint len,
                                    uint64 sequence, ZsetType type,
                                    ZNode **pred) const {
  ZNode *x = head_;
  for (int i = currentHeight() - 1; i >= 0; i--) {
    while (keyGreaterThan(score, member, len, sequence, type, x->next(i))) {
      x = x->next(i);
    }
    if (pred != nullptr) {
      pred[i] = x;
    }
  }

  return x->next(0);
}

ZNode *ZsetSkiplist::findPredecessor(double score, const uchar *member,
                                     uint len) const {
  ZNode *x = head_;
  for (int i = currentHeight() - 1; i >= 0; i--) {
    while (x->next(i) != nullptr &&
           zset_compare_keys(x->next(i)->score, x->next(i)->member,
                             x->next(i)->member_len, x->next(i)->sequence,
                             x->next(i)->type, score, member, len,
                             x->next(i)->sequence, x->next(i)->type) < 0) {
      x = x->next(i);
    }
  }

  return x == head_ ? nullptr : x;
}

int ZsetSkiplist::currentHeight() const {
  return top_level_.load(std::memory_order_relaxed);
}
