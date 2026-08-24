#include <cstdlib>
#include <cstring>

#include "storage/zset/zset_skiplist.h"

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
    : head_(createNode(0, nullptr, 0, kMaxHeight)),
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

ZNode *ZsetSkiplist::insert(double score, const uchar *member, uint len) {
  // Predecessor node at each level, for rewiring next pointers.
  ZNode *pred[kMaxHeight];
  ZNode *x = findLowerBound(score, member, len, pred);
  assert(x == nullptr ||
         !keysEqual(score, member, len, x->score, x->member, x->member_len));

  int level = randomLevel();
  if (level > currentHeight()) {
    for (int i = currentHeight(); i < level; i++) {
      pred[i] = head_;
    }
    top_level_.store(level, std::memory_order_relaxed);
  }

  ZNode *node = createNode(score, member, len, level);
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

void ZsetSkiplist::remove(const uchar *member, uint len, double score) {
  ZNode *pred[kMaxHeight];
  ZNode *x = findLowerBound(score, member, len, pred);
  if (x == nullptr ||
      !keysEqual(score, member, len, x->score, x->member, x->member_len)) {
    return;
  }

  for (int i = 0; i < currentHeight(); i++) {
    if (pred[i]->next(i) != x) {
      break;
    }
    pred[i]->setNext(i, x->next(i));
  }

  if (x->next(0) != nullptr) {
    x->next(0)->backward = x->backward;
  }

  free(x->member);
  free(x);

  while (currentHeight() > 1 && head_->next(currentHeight() - 1) == nullptr) {
    top_level_.store(currentHeight() - 1, std::memory_order_relaxed);
  }
  count_--;
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
  node_ = list_->findLowerBound(score, member, len, nullptr);
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

int ZsetSkiplist::compare(double score_a, const uchar *member_a, uint len_a,
                          double score_b, const uchar *member_b, uint len_b) {
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

  return 0;
}

ZNode *ZsetSkiplist::createNode(double score, const uchar *member, uint len,
                                int level) {
  // Flexible array: one slot is embedded, allocate level-1 more.
  size_t bytes = sizeof(ZNode) + (sizeof(std::atomic<ZNode *>) * (level - 1));
  ZNode *node = (ZNode *)malloc(bytes);
  node->score = score;
  node->member = nullptr;
  node->member_len = len;
  node->level = level;
  node->backward = nullptr;

  if (len > 0) {
    node->member = static_cast<uchar *>(malloc(len));
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
  return compare(score_a, member_a, len_a, score_b, member_b, len_b) == 0;
}

bool ZsetSkiplist::keyGreaterThan(double score, const uchar *member, uint len,
                                  ZNode *n) {
  return n != nullptr &&
         compare(n->score, n->member, n->member_len, score, member, len) < 0;
}

ZNode *ZsetSkiplist::findLowerBound(double score, const uchar *member, uint len,
                                    ZNode **pred) const {
  ZNode *x = head_;
  for (int i = currentHeight() - 1; i >= 0; i--) {
    while (keyGreaterThan(score, member, len, x->next(i))) {
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
           compare(x->next(i)->score, x->next(i)->member,
                   x->next(i)->member_len, score, member, len) < 0) {
      x = x->next(i);
    }
  }

  return x == head_ ? nullptr : x;
}

int ZsetSkiplist::currentHeight() const {
  return top_level_.load(std::memory_order_relaxed);
}
