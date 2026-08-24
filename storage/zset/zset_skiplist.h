#ifndef ZSET_SKIPLIST_H
#define ZSET_SKIPLIST_H

#include <atomic>
#include <cassert>
#include <cstring>
#include <random>

#include "my_inttypes.h"

// ZSET skiplist node. Key is (score, member).
struct ZNode {
  double score;     // Sort key
  uchar *member;    // Member bytes, owned by this node
  uint member_len;  // Member length
  int level;        // Node height
  ZNode *backward;  // Predecessor at level 0

  // Next pointer at level n, with acquire load.
  ZNode *next(int n) const;

  // Set next pointer at level n, with release store.
  void setNext(int n, ZNode *x) const;

  // Next pointer at level n, relaxed (single-threaded paths only).
  ZNode *nextRelaxed(int n) const;

  // Set next pointer at level n, relaxed (single-threaded paths only).
  void setNextRelaxed(int n, ZNode *x) const;

 private:
  mutable std::atomic<ZNode *> next_[1];  // Level pointers
};

// Skiplist for the ZSET memtable, ordered by (score, member).
class ZsetSkiplist {
 public:
  ZsetSkiplist();
  ~ZsetSkiplist();

  ZsetSkiplist(const ZsetSkiplist &) = delete;
  ZsetSkiplist &operator=(const ZsetSkiplist &) = delete;

  // Insert a new node and return it. The caller must ensure the key is new.
  ZNode *insert(double score, const uchar *member, uint len);

  // Remove the node with this key. No-op if absent.
  void remove(const uchar *member, uint len, double score);

  // Drop all nodes and reset to empty.
  void clear();

  // First node (smallest), or nullptr if empty.
  ZNode *first() const;

  // Last node (largest), or nullptr if empty.
  ZNode *last() const;

  // Node after n at level 0.
  ZNode *next(const ZNode *n) const;

  // Node before n at level 0.
  ZNode *prev(const ZNode *n) const;

  // Number of nodes.
  size_t count() const;

  // Count nodes whose score is within [min, max].
  size_t countInRange(double min, double max) const;

  // Cursor for sequential traversal.
  class Cursor {
   public:
    explicit Cursor(const ZsetSkiplist *list);

    // True while the cursor points at a node.
    bool valid() const;

    // Node the cursor points at.
    ZNode *current() const;

    // Move to the next node.
    void next();

    // Move to the previous node.
    void prev();

    // Move to the first node with key >= the target.
    void seekTo(double score, const uchar *member, uint len);

    // Move to the first node.
    void seekToFirst();

    // Move to the last node.
    void seekToLast();

   private:
    const ZsetSkiplist *list_;
    ZNode *node_;
  };

 private:
  enum { kMaxHeight = 32 };

  // Random level, about 25% chance to grow.
  int randomLevel();

  // Compare two keys by score, then member.
  static int compare(double score_a, const uchar *member_a, uint len_a,
                     double score_b, const uchar *member_b, uint len_b);

  // Allocate and init a node.
  static ZNode *createNode(double score, const uchar *member, uint len,
                           int level);

  // True if the two keys are equal.
  static bool keysEqual(double score_a, const uchar *member_a, uint len_a,
                        double score_b, const uchar *member_b, uint len_b);

  // True if the key is strictly greater than the given node's key.
  static bool keyGreaterThan(double score, const uchar *member, uint len,
                             ZNode *n);

  // First node with key >= target, filling predecessors per level.
  ZNode *findLowerBound(double score, const uchar *member, uint len,
                        ZNode **pred) const;

  // Last node with key < target.
  ZNode *findPredecessor(double score, const uchar *member, uint len) const;

  // Current list height.
  int currentHeight() const;

  ZNode *const head_;           // Sentinel
  std::atomic<int> top_level_;  // Current height
  std::mt19937 rng_;            // Random generator
  size_t count_;                // Number of nodes
};

#endif  // ZSET_SKIPLIST_H
