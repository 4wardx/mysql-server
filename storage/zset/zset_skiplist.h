#ifndef ZSET_SKIPLIST_H
#define ZSET_SKIPLIST_H

#include <atomic>
#include <cassert>
#include <cstring>
#include <random>

#include "my_inttypes.h"

// Value types stored in an internal key.
enum class ZsetType : uint8_t { kPut = 1, kDelete = 2 };

// ZSET skiplist node. Key is (score, member) + seq + type (internal key).
struct ZNode {
  double score;     // Sort key
  uchar *member;    // Member bytes, owned by this node
  uint member_len;  // Member length
  uint64 seq;       // Sequence number, newer first
  ZsetType type;    // PUT or DELETE
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

// Skiplist for the ZSET memtable: an ordered store of internal keys.
// All versions and tombstones are kept; only clear() drops everything,
// while compaction reclaims shadowed entries.
class ZsetSkiplist {
 public:
  ZsetSkiplist();
  ~ZsetSkiplist();

  ZsetSkiplist(const ZsetSkiplist &) = delete;
  ZsetSkiplist &operator=(const ZsetSkiplist &) = delete;

  // Insert an internal key node and return it.
  ZNode *insert(double score, const uchar *member, uint len, uint64 seq,
                ZsetType type);

  // Drop all nodes and reset to empty.
  void clear();

  // First node (smallest key), or nullptr if empty.
  ZNode *first() const;

  // Last node (largest key), or nullptr if empty.
  ZNode *last() const;

  // Node after n at level 0.
  ZNode *next(const ZNode *n) const;

  // Node before n at level 0.
  ZNode *prev(const ZNode *n) const;

  // Total number of nodes, including shadowed versions and tombstones.
  size_t count() const;

  // Count nodes whose score is within [min, max].
  size_t countInRange(double min, double max) const;

  // Cursor for sequential traversal in key order.
  class Cursor {
   public:
    explicit Cursor(const ZsetSkiplist *list);

    // True while the cursor points at a node.
    bool valid() const;

    // Node the cursor currently points at.
    ZNode *current() const;

    // Move one node forward (larger key).
    void next();

    // Move one node backward (smaller key).
    void prev();

    // Move to the first internal key >= (score, member) at max seq.
    void seekTo(double score, const uchar *member, uint len);

    // Move to the first node (smallest key).
    void seekToFirst();

    // Move to the last node (largest key).
    void seekToLast();

   private:
    const ZsetSkiplist *list_;
    ZNode *node_;
  };

 private:
  enum { kMaxHeight = 32 };

  // Random level, about 25% chance to grow.
  int randomLevel();

  // Compare internal keys: score, member, then seq descending, then type.
  static int compare(double score_a, const uchar *member_a, uint len_a,
                     uint64 seq_a, ZsetType type_a, double score_b,
                     const uchar *member_b, uint len_b, uint64 seq_b,
                     ZsetType type_b);

  // Allocate and init a node.
  static ZNode *createNode(double score, const uchar *member, uint len,
                           uint64 seq, ZsetType type, int level);

  // True if the keys have the same user key (score, member).
  static bool keysEqual(double score_a, const uchar *member_a, uint len_a,
                        double score_b, const uchar *member_b, uint len_b);

  // True if the key is strictly greater than the given node's key.
  static bool keyGreaterThan(double score, const uchar *member, uint len,
                             uint64 seq, ZsetType type, ZNode *n);

  // First node with key >= target, filling predecessors per level.
  ZNode *findLowerBound(double score, const uchar *member, uint len, uint64 seq,
                        ZsetType type, ZNode **pred) const;

  // Last node whose user key is strictly before the target.
  ZNode *findPredecessor(double score, const uchar *member, uint len) const;

  // Current list height.
  int currentHeight() const;

  ZNode *const head_;           // Sentinel
  std::atomic<int> top_level_;  // Current height
  std::mt19937 rng_;            // Random generator
  size_t count_;                // Number of nodes
};

#endif  // ZSET_SKIPLIST_H
