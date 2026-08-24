/*
  Unit tests for the ZSET core structures: skiplist, hashtable and memtable.
*/

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "storage/zset/zset_memtable.h"

using std::string;
using std::vector;

namespace {

// Member bytes for a C string, with its length.
const uchar *member(const char *s, uint *len) {
  *len = static_cast<uint>(strlen(s));
  return reinterpret_cast<const uchar *>(s);
}

string member_name(const ZNode *node) {
  return string(reinterpret_cast<const char *>(node->member), node->member_len);
}

}  // namespace

// Skiplist is ordered by (score, member).
TEST(ZsetSkiplistTest, Ordering) {
  ZsetSkiplist sl;

  EXPECT_EQ(sl.count(), 0U);
  EXPECT_EQ(sl.first(), nullptr);
  EXPECT_EQ(sl.last(), nullptr);

  uint len;
  const uchar *m;
  m = member("apple", &len);
  sl.insert(3.0, m, len);
  m = member("banana", &len);
  sl.insert(1.0, m, len);
  m = member("cherry", &len);
  sl.insert(1.0, m, len);
  m = member("durian", &len);
  sl.insert(2.5, m, len);
  m = member("egg", &len);
  sl.insert(1.0, m, len);

  EXPECT_EQ(sl.count(), 5U);

  // Expected (score, member) order.
  const vector<string> expected = {"banana", "cherry", "egg", "durian",
                                   "apple"};
  size_t i = 0;
  for (ZNode *n = sl.first(); n != nullptr; n = sl.next(n)) {
    EXPECT_EQ(member_name(n), expected[i]);
    i++;
  }
  EXPECT_EQ(i, expected.size());

  // last() is the largest (score, member).
  EXPECT_NE(sl.last(), nullptr);
  EXPECT_EQ(member_name(sl.last()), "apple");

  // Range count by score.
  EXPECT_EQ(sl.countInRange(1.0, 1.0), 3U);
  EXPECT_EQ(sl.countInRange(1.0, 2.5), 4U);
  EXPECT_EQ(sl.countInRange(2.6, 3.0), 1U);
}

// Remove by (score, member), including absent keys.
TEST(ZsetSkiplistTest, Remove) {
  ZsetSkiplist sl;

  uint len;
  const uchar *m;
  m = member("a", &len);
  sl.insert(1.0, m, len);
  m = member("b", &len);
  sl.insert(2.0, m, len);
  m = member("c", &len);
  sl.insert(3.0, m, len);

  m = member("b", &len);
  sl.remove(m, len, 2.0);
  EXPECT_EQ(sl.count(), 2U);
  EXPECT_EQ(member_name(sl.first()), "a");
  EXPECT_EQ(member_name(sl.last()), "c");

  // Removing an absent key is a no-op.
  m = member("b", &len);
  sl.remove(m, len, 2.0);
  EXPECT_EQ(sl.count(), 2U);

  m = member("a", &len);
  sl.remove(m, len, 1.0);
  m = member("c", &len);
  sl.remove(m, len, 3.0);
  EXPECT_EQ(sl.count(), 0U);
  EXPECT_EQ(sl.first(), nullptr);
  EXPECT_EQ(sl.last(), nullptr);
}

// Cursor traversal and seek.
TEST(ZsetSkiplistTest, Cursor) {
  ZsetSkiplist sl;

  uint len;
  const uchar *m;
  m = member("x", &len);
  sl.insert(1.0, m, len);
  m = member("y", &len);
  sl.insert(2.0, m, len);

  ZsetSkiplist::Cursor cur(&sl);
  EXPECT_FALSE(cur.valid());
  cur.seekToFirst();
  EXPECT_TRUE(cur.valid());
  EXPECT_EQ(member_name(cur.current()), "x");
  cur.next();
  EXPECT_TRUE(cur.valid());
  EXPECT_EQ(member_name(cur.current()), "y");
  cur.next();
  EXPECT_FALSE(cur.valid());

  // seekTo finds the first node with key >= (2.0, "").
  cur.seekTo(2.0, nullptr, 0);
  EXPECT_TRUE(cur.valid());
  EXPECT_EQ(member_name(cur.current()), "y");
}

// Hash table point lookups.
TEST(ZsetHashtableTest, Basic) {
  ZsetHashtable ht;

  ZNode fake1;
  ZNode fake2;
  uint len;
  const uchar *m;
  m = member("apple", &len);
  ht.insert(m, len, &fake1);
  m = member("banana", &len);
  ht.insert(m, len, &fake2);

  EXPECT_EQ(ht.count(), 2U);
  m = member("apple", &len);
  EXPECT_EQ(ht.lookup(m, len), &fake1);
  m = member("banana", &len);
  EXPECT_EQ(ht.lookup(m, len), &fake2);
  m = member("cherry", &len);
  EXPECT_EQ(ht.lookup(m, len), nullptr);

  // Overwrite and remove.
  m = member("apple", &len);
  ht.insert(m, len, &fake2);
  m = member("apple", &len);
  EXPECT_EQ(ht.lookup(m, len), &fake2);
  m = member("apple", &len);
  ht.remove(m, len);
  m = member("apple", &len);
  EXPECT_EQ(ht.lookup(m, len), nullptr);
  EXPECT_EQ(ht.count(), 1U);

  ht.clear();
  EXPECT_EQ(ht.count(), 0U);
}

// Memtable keeps both indexes consistent.
TEST(ZsetMemTableTest, Basic) {
  ZsetMemTable mem;

  uint len;
  const uchar *m;
  m = member("apple", &len);
  mem.add(3.0, m, len);
  m = member("banana", &len);
  mem.add(1.0, m, len);

  EXPECT_EQ(mem.count(), 2U);

  // Point lookup via the hash table.
  m = member("apple", &len);
  ZNode *n = mem.get(m, len);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->score, 3.0);
  m = member("missing", &len);
  EXPECT_EQ(mem.get(m, len), nullptr);

  // Ordered scan via the skiplist.
  EXPECT_NE(mem.skiplist()->first(), nullptr);
  EXPECT_EQ(member_name(mem.skiplist()->first()), "banana");

  // Remove.
  m = member("apple", &len);
  mem.remove(m, len);
  EXPECT_EQ(mem.count(), 1U);
  m = member("apple", &len);
  EXPECT_EQ(mem.get(m, len), nullptr);

  mem.clear();
  EXPECT_EQ(mem.count(), 0U);
}

// Member bytes may contain NUL; length is explicit.
TEST(ZsetSkiplistTest, BinaryMember) {
  ZsetSkiplist sl;

  const uchar raw[] = {0x61, 0x00, 0x62, 0x63};  // "a\0bc"
  sl.insert(1.0, raw, sizeof(raw));
  EXPECT_EQ(sl.count(), 1U);
  ASSERT_NE(sl.first(), nullptr);
  EXPECT_EQ(sl.first()->member_len, sizeof(raw));
  EXPECT_EQ(memcmp(sl.first()->member, raw, sizeof(raw)), 0);
}

// Cursor operations on an empty list must be invalid.
TEST(ZsetSkiplistTest, EmptyCursor) {
  ZsetSkiplist sl;
  ZsetSkiplist::Cursor cur(&sl);

  EXPECT_FALSE(cur.valid());
  cur.seekToFirst();
  EXPECT_FALSE(cur.valid());
  cur.seekToLast();
  EXPECT_FALSE(cur.valid());
  cur.seekTo(1.0, nullptr, 0);
  EXPECT_FALSE(cur.valid());
}

// seekTo semantics: exact match, gap, lower boundary and past-the-end.
TEST(ZsetSkiplistTest, Seek) {
  ZsetSkiplist sl;

  uint len;
  const uchar *m;
  const char *names[] = {"b", "d", "f", "h"};
  const double scores[] = {2.0, 4.0, 6.0, 8.0};
  for (int i = 0; i < 4; i++) {
    m = member(names[i], &len);
    sl.insert(scores[i], m, len);
  }

  ZsetSkiplist::Cursor cur(&sl);

  // Exact match.
  cur.seekTo(4.0, nullptr, 0);
  ASSERT_TRUE(cur.valid());
  EXPECT_EQ(cur.current()->score, 4.0);
  EXPECT_EQ(member_name(cur.current()), "d");

  // Gap: seek to 5 lands on the next node (6).
  cur.seekTo(5.0, nullptr, 0);
  ASSERT_TRUE(cur.valid());
  EXPECT_EQ(cur.current()->score, 6.0);

  // Lower boundary: seek to 1 lands on the first node (2).
  cur.seekTo(1.0, nullptr, 0);
  ASSERT_TRUE(cur.valid());
  EXPECT_EQ(cur.current()->score, 2.0);

  // Past the end: nothing >= 9.
  cur.seekTo(9.0, nullptr, 0);
  EXPECT_FALSE(cur.valid());

  // Past the last member of the top score: (8, "h") is last, seek (8, "zz").
  cur.seekTo(8.0, reinterpret_cast<const uchar *>("zz"), 2);
  EXPECT_FALSE(cur.valid());
}

// Reverse iteration from the last node down to before the first.
TEST(ZsetSkiplistTest, ReverseIteration) {
  ZsetSkiplist sl;

  uint len;
  const uchar *m;
  m = member("a", &len);
  sl.insert(1.0, m, len);
  m = member("b", &len);
  sl.insert(2.0, m, len);
  m = member("c", &len);
  sl.insert(3.0, m, len);

  // last() / prev() on the list itself.
  EXPECT_EQ(member_name(sl.last()), "c");
  EXPECT_EQ(member_name(sl.prev(sl.last())), "b");
  EXPECT_EQ(sl.prev(sl.first()), nullptr);

  // Cursor reverse traversal.
  ZsetSkiplist::Cursor cur(&sl);
  cur.seekToLast();
  ASSERT_TRUE(cur.valid());
  EXPECT_EQ(member_name(cur.current()), "c");
  cur.prev();
  ASSERT_TRUE(cur.valid());
  EXPECT_EQ(member_name(cur.current()), "b");
  cur.prev();
  ASSERT_TRUE(cur.valid());
  EXPECT_EQ(member_name(cur.current()), "a");
  cur.prev();
  EXPECT_FALSE(cur.valid());
}

// Randomized large insertion, verified against a sorted gold standard.
TEST(ZsetSkiplistTest, RandomizedLargeInsert) {
  ZsetSkiplist sl;

  std::vector<std::pair<double, string>> gold;
  std::mt19937 rng(12345);
  std::uniform_real_distribution<double> score_dist(0.0, 1000.0);
  const int kNum = 10000;

  for (int i = 0; i < kNum; i++) {
    double score = score_dist(rng);
    string m = "member_" + std::to_string(i);
    sl.insert(score, reinterpret_cast<const uchar *>(m.data()), m.size());
    gold.emplace_back(score, m);
  }

  std::sort(gold.begin(), gold.end(),
            [](const std::pair<double, string> &a,
               const std::pair<double, string> &b) {
              return a.first < b.first ||
                     (a.first == b.first && a.second < b.second);
            });

  EXPECT_EQ(sl.count(), static_cast<size_t>(kNum));

  size_t i = 0;
  for (ZNode *n = sl.first(); n != nullptr; n = sl.next(n), i++) {
    ASSERT_LT(i, gold.size());
    EXPECT_EQ(n->score, gold[i].first);
    EXPECT_EQ(member_name(n), gold[i].second);
  }
  EXPECT_EQ(i, gold.size());
}
