/*
  Unit tests for the ZSET core structures: versioned skiplist, memtable
  and write-ahead log.
*/

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "my_dir.h"
#include "my_io.h"

#include "storage/zset/zset_lsm.h"
#include "storage/zset/zset_memtable.h"
#include "storage/zset/zset_sstable.h"
#include "storage/zset/zset_wal.h"

using std::pair;
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

// Next sequence number for a chain of inserts.
uint64 g_seq = 1;
uint64 next_seq() { return g_seq++; }

// Test log path; removed by each test.
const char *kWalTestPath = "/tmp/zset_wal_test.log";

}  // namespace

// Skiplist is ordered by (score, member), newest seq first.
TEST(ZsetSkiplistTest, Ordering) {
  ZsetSkiplist sl;

  EXPECT_EQ(sl.count(), 0U);
  EXPECT_EQ(sl.first(), nullptr);
  EXPECT_EQ(sl.last(), nullptr);

  uint len;
  const uchar *m;
  m = member("apple", &len);
  sl.insert(3.0, m, len, next_seq(), ZsetType::kPut);
  m = member("banana", &len);
  sl.insert(1.0, m, len, next_seq(), ZsetType::kPut);
  m = member("cherry", &len);
  sl.insert(1.0, m, len, next_seq(), ZsetType::kPut);
  m = member("durian", &len);
  sl.insert(2.5, m, len, next_seq(), ZsetType::kPut);
  m = member("egg", &len);
  sl.insert(1.0, m, len, next_seq(), ZsetType::kPut);

  EXPECT_EQ(sl.count(), 5U);

  // Expected (score, member) order.
  const vector<string> expected = {"banana", "cherry", "egg", "durian",
                                   "apple"};
  size_t i = 0;
  for (ZNode *n = sl.first(); n != nullptr; n = sl.next(n), i++) {
    EXPECT_EQ(member_name(n), expected[i]);
    EXPECT_EQ(n->type, ZsetType::kPut);
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

// Multiple versions of the same (score, member): newest seq sorts first.
TEST(ZsetSkiplistTest, NewestSeqFirst) {
  ZsetSkiplist sl;

  uint len;
  const uchar *m = member("a", &len);
  sl.insert(1.0, m, len, 1, ZsetType::kPut);
  sl.insert(1.0, m, len, 2, ZsetType::kPut);
  sl.insert(1.0, m, len, 3, ZsetType::kPut);

  // Same (score, member): seq 3 must come first.
  ASSERT_NE(sl.first(), nullptr);
  EXPECT_EQ(sl.first()->sequence, 3U);
  EXPECT_EQ(sl.next(sl.first())->sequence, 2U);
  EXPECT_EQ(sl.next(sl.next(sl.first()))->sequence, 1U);
}

// Random insertion with a sorted gold standard.
TEST(ZsetSkiplistTest, RandomizedLargeInsert) {
  ZsetSkiplist sl;

  vector<std::pair<double, string>> gold;
  std::mt19937 rng(12345);
  std::uniform_real_distribution<double> score_dist(0.0, 1000.0);

  const int kNum = 10000;
  for (int i = 0; i < kNum; i++) {
    double score = score_dist(rng);
    string m = "member_" + std::to_string(i);
    sl.insert(score, reinterpret_cast<const uchar *>(m.data()), m.size(), i + 1,
              ZsetType::kPut);
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
    EXPECT_EQ(n->score, gold[i].first);
    EXPECT_EQ(member_name(n), gold[i].second);
  }
  EXPECT_EQ(i, gold.size());
}

// Cursor traversal and seek.
TEST(ZsetSkiplistTest, Cursor) {
  ZsetSkiplist sl;

  uint len;
  const uchar *m;
  m = member("x", &len);
  sl.insert(1.0, m, len, next_seq(), ZsetType::kPut);
  m = member("y", &len);
  sl.insert(2.0, m, len, next_seq(), ZsetType::kPut);

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

// Memtable: a PUT then a tombstone removes the member from the live view.
TEST(ZsetMemTableTest, TombstoneVoidsLiveView) {
  ZsetMemTable mem;

  uint len;
  const uchar *m = member("a", &len);
  mem.put(1.0, m, len, 1);
  EXPECT_EQ(mem.count(), 1U);
  double score;
  ASSERT_TRUE(mem.get(m, len, &score));
  EXPECT_EQ(score, 1.0);

  mem.tombstone(1.0, m, len, 2);
  EXPECT_EQ(mem.count(), 0U);
  EXPECT_FALSE(mem.get(m, len, &score));
  // The hash keeps the newest version, so a deleted member is told apart
  // from one that never existed by the tombstone node it returns.
  ZNode *deleted = mem.lookup(m, len);
  ASSERT_NE(deleted, nullptr);
  EXPECT_EQ(deleted->type, ZsetType::kDelete);

  // Both nodes still live in the skiplist (pure LSM keeps history).
  EXPECT_EQ(mem.skiplist()->count(), 2U);
  EXPECT_EQ(mem.firstLive(), nullptr);  // the group is tombstoned
}

// Score change: tombstone old key + PUT new key, member appears once live.
TEST(ZsetMemTableTest, ScoreChangeSingleLiveNode) {
  ZsetMemTable mem;

  uint len;
  const uchar *m = member("x", &len);
  mem.put(1.0, m, len, 1);
  mem.tombstone(1.0, m, len, 2);  // void the old score
  mem.put(2.0, m, len, 3);

  EXPECT_EQ(mem.count(), 1U);
  double score;
  ASSERT_TRUE(mem.get(m, len, &score));
  EXPECT_EQ(score, 2.0);
  int x_count = 0;
  for (ZNode *n = mem.skiplist()->first(); n != nullptr;
       n = mem.skiplist()->next(n)) {
    if (member_name(n) == "x") {
      x_count++;
    }
  }
  EXPECT_EQ(x_count, 3);  // PUT(1) + DELETE(1) + PUT(2), all kept
  ASSERT_NE(mem.firstLive(), nullptr);
  EXPECT_EQ(member_name(mem.firstLive()), "x");
  EXPECT_EQ(mem.firstLive()->score, 2.0);
  EXPECT_EQ(mem.nextLive(mem.firstLive()), nullptr);
}

// Same score re-put: only a PUT, no tombstone needed; newest wins.
TEST(ZsetMemTableTest, SameScoreReput) {
  ZsetMemTable mem;

  uint len;
  const uchar *m = member("x", &len);
  mem.put(5.0, m, len, 1);
  mem.put(5.0, m, len, 2);  // same score, just a newer PUT

  EXPECT_EQ(mem.count(), 1U);
  double score;
  ASSERT_TRUE(mem.get(m, len, &score));
  EXPECT_EQ(score, 5.0);
  EXPECT_EQ(mem.skiplist()->count(), 2U);  // two versions kept
  ASSERT_NE(mem.firstLive(), nullptr);
  EXPECT_EQ(mem.firstLive()->sequence, 2U);
  EXPECT_EQ(mem.nextLive(mem.firstLive()), nullptr);
}

// Ordered merged scan: ties by member, tombstones skipped.
TEST(ZsetMemTableTest, MergedScan) {
  ZsetMemTable mem;
  uint len;
  const uchar *m;
  const char *names[] = {"pear", "apple", "grape", "banana", "cherry"};
  for (const char *name : names) {
    m = member(name, &len);
    mem.put(1.0, m, len, next_seq());
  }
  // Delete grape.
  m = member("grape", &len);
  mem.tombstone(1.0, m, len, next_seq());

  const vector<string> expected = {"apple", "banana", "cherry", "pear"};
  size_t i = 0;
  for (ZNode *n = mem.firstLive(); n != nullptr; n = mem.nextLive(n), i++) {
    EXPECT_EQ(member_name(n), expected[i]);
  }
  EXPECT_EQ(i, expected.size());
  EXPECT_EQ(mem.count(), 4U);
}

// Reverse merged scan.
TEST(ZsetMemTableTest, ReverseScan) {
  ZsetMemTable mem;
  uint len;
  const uchar *m;
  m = member("a", &len);
  mem.put(1.0, m, len, 1);
  m = member("b", &len);
  mem.put(2.0, m, len, 2);
  m = member("c", &len);
  mem.put(3.0, m, len, 3);
  m = member("b", &len);
  mem.tombstone(2.0, m, len, 4);

  const vector<string> expected = {"c", "a"};
  size_t i = 0;
  for (ZNode *n = mem.lastLive(); n != nullptr; n = mem.prevLive(n), i++) {
    EXPECT_EQ(member_name(n), expected[i]);
  }
  EXPECT_EQ(i, expected.size());
}

// seekLive lands on the first live node >= the target user key.
TEST(ZsetMemTableTest, SeekLive) {
  ZsetMemTable mem;
  uint len;
  const uchar *m;
  const char *names[] = {"b", "d", "f", "h"};
  const double scores[] = {2.0, 4.0, 6.0, 8.0};
  for (int i = 0; i < 4; i++) {
    m = member(names[i], &len);
    mem.put(scores[i], m, len, i + 1);
  }
  // Tombstone (4, "d").
  m = member("d", &len);
  mem.tombstone(4.0, m, len, 5);

  // Exact live hit.
  ZNode *n = mem.seekLive(4.0, nullptr, 0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(member_name(n), "f");  // (4,d) tombstoned, skip to (6,f)

  // Gap -> next.
  n = mem.seekLive(5.0, nullptr, 0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(member_name(n), "f");

  // Past end.
  EXPECT_EQ(mem.seekLive(9.0, nullptr, 0), nullptr);
}

// Large mixed workload: live count matches the expected survivors.
TEST(ZsetMemTableTest, CountConsistency) {
  ZsetMemTable mem;
  std::mt19937 rng(777);
  std::uniform_real_distribution<double> dist(0.0, 100.0);

  int deleted = 0;
  for (int i = 0; i < 5000; i++) {
    string m = "m_" + std::to_string(i);
    uint64 seq = next_seq();
    mem.put(dist(rng), reinterpret_cast<const uchar *>(m.data()), m.size(),
            seq);
    if (i % 4 == 0) {
      mem.tombstone(dist(rng), reinterpret_cast<const uchar *>(m.data()),
                    m.size(), next_seq());
      deleted++;
    }
  }
  EXPECT_EQ(mem.count(), 5000U - deleted);
}

// Snapshot scan: live iterators with a max_seq watermark ignore versions
// written after the scan started, so rows updated mid-scan are not
// re-visited (the fix for the UPDATE re-scan loop).
TEST(ZsetMemTableTest, SnapshotScan) {
  ZsetMemTable mem;

  uint len;
  const uchar *m;
  m = member("a", &len);
  mem.put(1.0, m, len, 1);
  m = member("b", &len);
  mem.put(2.0, m, len, 2);
  m = member("c", &len);
  mem.put(3.0, m, len, 3);

  const uint64 watermark = 3;  // snapshot taken right after the inserts

  // Score changes during the "scan": tombstone the old key, put the new.
  m = member("a", &len);
  mem.tombstone(1.0, m, len, 4);
  mem.put(11.0, m, len, 5);
  m = member("c", &len);
  mem.tombstone(3.0, m, len, 6);
  mem.put(13.0, m, len, 7);

  // Full view sees the new values.
  double score;
  m = member("a", &len);
  ASSERT_TRUE(mem.get(m, len, &score));
  EXPECT_EQ(score, 11.0);
  EXPECT_EQ(mem.count(), 3U);

  // Snapshot view as of the watermark still returns each row once, with
  // its original value.
  vector<double> scores;
  for (ZNode *n = mem.firstLive(watermark); n != nullptr;
       n = mem.nextLive(n, watermark)) {
    scores.push_back(n->score);
  }
  const vector<double> expected = {1.0, 2.0, 3.0};
  EXPECT_EQ(scores, expected);
}

// The exact UPDATE re-scan scenario: every row is moved to a higher
// score several times while the scan runs. The snapshot view still
// visits each original row exactly once and terminates.
TEST(ZsetMemTableTest, SnapshotUpdateLoop) {
  ZsetMemTable mem;

  uint len;
  const uchar *m;
  const char *names[] = {"a", "b", "c", "d", "e"};
  uint64 seq = 0;
  for (const char *name : names) {
    m = member(name, &len);
    seq++;
    mem.put(static_cast<double>(seq), m, len, seq);  // a=1..e=5
  }
  const uint64 watermark = 5;

  // Three rounds of score=score+10 over all rows.
  for (int round = 0; round < 3; round++) {
    for (const char *name : names) {
      m = member(name, &len);
      double old_score;
      ASSERT_TRUE(mem.get(m, len, &old_score));
      mem.tombstone(old_score, m, len, ++seq);
      mem.put(old_score + 10.0, m, len, ++seq);
    }
  }

  int rows = 0;
  vector<double> scores;
  for (ZNode *n = mem.firstLive(watermark); n != nullptr;
       n = mem.nextLive(n, watermark)) {
    rows++;
    scores.push_back(n->score);
  }
  EXPECT_EQ(rows, 5);
  const vector<double> expected = {1.0, 2.0, 3.0, 4.0, 5.0};
  EXPECT_EQ(scores, expected);

  // The full view, in contrast, has moved on.
  double score;
  m = member("a", &len);
  ASSERT_TRUE(mem.get(m, len, &score));
  EXPECT_EQ(score, 31.0);
}

// A delete during the scan: the tombstone is newer than the watermark, so
// the snapshot view still shows the row; the full view does not.
TEST(ZsetMemTableTest, SnapshotDeleteMidScan) {
  ZsetMemTable mem;

  uint len;
  const uchar *m;
  m = member("a", &len);
  mem.put(1.0, m, len, 1);
  m = member("b", &len);
  mem.put(2.0, m, len, 2);

  const uint64 watermark = 2;
  m = member("a", &len);
  mem.tombstone(1.0, m, len, 3);  // delete a during the scan

  double score;
  EXPECT_FALSE(mem.get(m, len, &score));  // gone from the full view
  EXPECT_EQ(mem.count(), 1U);

  vector<double> scores;
  for (ZNode *n = mem.firstLive(watermark); n != nullptr;
       n = mem.nextLive(n, watermark)) {
    scores.push_back(n->score);
  }
  const vector<double> expected = {1.0, 2.0};  // still visible in the snapshot
  EXPECT_EQ(scores, expected);
}

// A brand-new member inserted during the scan is invisible to the
// snapshot (it did not exist when the scan started).
TEST(ZsetMemTableTest, SnapshotNewKeyMidScan) {
  ZsetMemTable mem;

  uint len;
  const uchar *m;
  m = member("a", &len);
  mem.put(1.0, m, len, 1);

  const uint64 watermark = 1;
  m = member("b", &len);
  mem.put(2.0, m, len, 2);  // new key during the scan

  vector<double> scores;
  for (ZNode *n = mem.firstLive(watermark); n != nullptr;
       n = mem.nextLive(n, watermark)) {
    scores.push_back(n->score);
  }
  const vector<double> expected = {1.0};
  EXPECT_EQ(scores, expected);

  double score;
  m = member("b", &len);
  ASSERT_TRUE(mem.get(m, len, &score));  // but it is in the full view
  EXPECT_EQ(score, 2.0);
}

// The watermark applies to reverse scans and seekLive as well.
TEST(ZsetMemTableTest, SnapshotReverseAndSeek) {
  ZsetMemTable mem;

  uint len;
  const uchar *m;
  m = member("a", &len);
  mem.put(1.0, m, len, 1);
  m = member("b", &len);
  mem.put(2.0, m, len, 2);
  m = member("c", &len);
  mem.put(3.0, m, len, 3);

  const uint64 watermark = 3;
  m = member("c", &len);
  mem.tombstone(3.0, m, len, 4);
  mem.put(13.0, m, len, 5);  // update c during the "scan"

  vector<double> scores;
  for (ZNode *n = mem.lastLive(watermark); n != nullptr;
       n = mem.prevLive(n, watermark)) {
    scores.push_back(n->score);
  }
  const vector<double> expected = {3.0, 2.0, 1.0};
  EXPECT_EQ(scores, expected);

  m = member("b", &len);
  ZNode *node = mem.seekLive(2.0, m, len, watermark);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->score, 2.0);

  // A key that only exists above the watermark is not found.
  m = member("c", &len);
  node = mem.seekLive(13.0, m, len, watermark);
  EXPECT_EQ(node, nullptr);
}

// Default iterators (max_seq = ~0ULL) see the full live view: the newest
// version wins.
TEST(ZsetMemTableTest, SnapshotNoFilter) {
  ZsetMemTable mem;

  uint len;
  const uchar *m;
  m = member("a", &len);
  mem.put(1.0, m, len, 1);
  m = member("a", &len);
  mem.tombstone(1.0, m, len, 2);  // update a: 1 -> 11
  mem.put(11.0, m, len, 3);

  double score;
  m = member("a", &len);
  ASSERT_TRUE(mem.get(m, len, &score));
  EXPECT_EQ(score, 11.0);

  vector<double> scores;
  for (ZNode *n = mem.firstLive(); n != nullptr; n = mem.nextLive(n)) {
    scores.push_back(n->score);
  }
  const vector<double> expected = {11.0};
  EXPECT_EQ(scores, expected);
}

// A row deleted before the scan started stays absent from the snapshot
// (the watermark does not resurrect pre-scan tombstones), but a re-added
// version is present.
TEST(ZsetMemTableTest, SnapshotPreScanDelete) {
  ZsetMemTable mem;

  uint len;
  const uchar *m;
  m = member("a", &len);
  mem.put(1.0, m, len, 1);
  m = member("b", &len);
  mem.put(2.0, m, len, 2);
  m = member("a", &len);
  mem.tombstone(1.0, m, len, 3);  // delete a
  m = member("a", &len);
  mem.put(5.0, m, len, 4);  // and re-add it

  const uint64 watermark = 4;  // scan starts now: a=5, b=2

  vector<double> scores;
  for (ZNode *n = mem.firstLive(watermark); n != nullptr;
       n = mem.nextLive(n, watermark)) {
    scores.push_back(n->score);
  }
  const vector<double> expected = {2.0, 5.0};
  EXPECT_EQ(scores, expected);
}

// WAL: append PUT/DELETE with seq, replay restores the live view + seq.
TEST(ZsetWalTest, AppendReplay) {
  unlink(kWalTestPath);
  Zset_wal wal;
  ASSERT_EQ(wal.open(kWalTestPath), 0);

  uint len;
  const uchar *m;
  m = member("a", &len);
  wal.append(1.0, m, len, 1, Zset_wal::Type::kPut);
  m = member("b", &len);
  wal.append(2.0, m, len, 2, Zset_wal::Type::kPut);
  m = member("a", &len);
  wal.append(1.0, m, len, 3, Zset_wal::Type::kDelete);  // void (1,a)
  m = member("a", &len);
  wal.append(10.0, m, len, 4, Zset_wal::Type::kPut);
  m = member("b", &len);
  wal.append(2.0, m, len, 5, Zset_wal::Type::kDelete);
  wal.close();

  Zset_wal wal2;
  ASSERT_EQ(wal2.open(kWalTestPath), 0);
  ZsetMemTable mem;
  uint64 next_seq = 0;
  wal2.replay(&mem, &next_seq);
  EXPECT_EQ(next_seq, 6U);
  EXPECT_EQ(mem.count(), 1U);  // only 'a' at 10 survives

  m = member("a", &len);
  double score;
  ASSERT_TRUE(mem.get(m, len, &score));
  EXPECT_EQ(score, 10.0);
  wal2.close();
  unlink(kWalTestPath);
}

// CLEAR voids all prior records.
TEST(ZsetWalTest, Clear) {
  unlink(kWalTestPath);
  Zset_wal wal;
  ASSERT_EQ(wal.open(kWalTestPath), 0);
  uint len;
  const uchar *m;
  m = member("a", &len);
  wal.append(1.0, m, len, 1, Zset_wal::Type::kPut);
  m = member("b", &len);
  wal.append(2.0, m, len, 2, Zset_wal::Type::kPut);
  ASSERT_EQ(wal.append_clear(), 0);
  m = member("c", &len);
  wal.append(3.0, m, len, 3, Zset_wal::Type::kPut);
  wal.close();

  Zset_wal wal2;
  ASSERT_EQ(wal2.open(kWalTestPath), 0);
  ZsetMemTable mem;
  uint64 next_seq = 0;
  wal2.replay(&mem, &next_seq);
  EXPECT_EQ(mem.count(), 1U);
  ASSERT_NE(mem.firstLive(), nullptr);
  EXPECT_EQ(member_name(mem.firstLive()), "c");
  wal2.close();
  unlink(kWalTestPath);
}

// A torn/corrupted tail stops replay but keeps the valid prefix.
TEST(ZsetWalTest, CorruptTail) {
  unlink(kWalTestPath);
  Zset_wal wal;
  ASSERT_EQ(wal.open(kWalTestPath), 0);
  uint len;
  const uchar *m = member("a", &len);
  wal.append(1.0, m, len, 1, Zset_wal::Type::kPut);
  wal.close();

  int fd = open(kWalTestPath, O_WRONLY | O_APPEND);
  ASSERT_GE(fd, 0);
  const uchar garbage[] = {0x01, 0x02, 0x03, 0x04};
  ASSERT_EQ(write(fd, garbage, sizeof(garbage)),
            static_cast<ssize_t>(sizeof(garbage)));
  close(fd);

  Zset_wal wal2;
  ASSERT_EQ(wal2.open(kWalTestPath), 0);
  ZsetMemTable mem;
  uint64 next_seq = 0;
  wal2.replay(&mem, &next_seq);
  EXPECT_EQ(mem.count(), 1U);
  wal2.close();
  unlink(kWalTestPath);
}

// Many records across multiple append calls, then a full replay.
TEST(ZsetWalTest, ManyRecordsReplay) {
  unlink(kWalTestPath);
  Zset_wal wal;
  ASSERT_EQ(wal.open(kWalTestPath), 0);
  const int kNum = 2000;

  for (int i = 0; i < kNum; i++) {
    string m = "key_" + std::to_string(i);
    ASSERT_EQ(wal.append(static_cast<double>(i),
                         reinterpret_cast<const uchar *>(m.data()), m.size(),
                         i + 1, Zset_wal::Type::kPut),
              0);
  }

  // Delete every third key.
  for (int i = 0; i < kNum; i += 3) {
    string m = "key_" + std::to_string(i);
    ASSERT_EQ(wal.append(static_cast<double>(i),
                         reinterpret_cast<const uchar *>(m.data()), m.size(),
                         kNum + i + 1, Zset_wal::Type::kDelete),
              0);
  }
  wal.close();

  Zset_wal wal2;
  ASSERT_EQ(wal2.open(kWalTestPath), 0);
  ZsetMemTable mem;
  uint64 next_seq = 0;
  wal2.replay(&mem, &next_seq);
  EXPECT_GT(next_seq, static_cast<uint64>(kNum));

  size_t expected = 0;
  for (int i = 0; i < kNum; i++) {
    if (i % 3 != 0) {
      expected++;
    }
  }
  EXPECT_EQ(mem.count(), expected);
  wal2.close();
  unlink(kWalTestPath);
}

// Score encode/decode round-trips, including negatives and zero.
TEST(ZsetSSTableTest, ScoreEncode) {
  const double vals[] = {-1e6, -3.5, -0.0, 0.0, 0.5, 1.0, 2.5, 100.0, 1e6};
  for (double v : vals) {
    EXPECT_EQ(zset_decode_score(zset_encode_score(v)), v);
  }
  // Byte order of the encoding matches numeric order.
  const uint64 a = zset_encode_score(-1.0);
  const uint64 b = zset_encode_score(0.0);
  const uint64 c = zset_encode_score(1.0);
  EXPECT_LT(a, b);
  EXPECT_LT(b, c);
}

// Write a sorted sequence, read it back in order with correct values.
TEST(ZsetSSTableTest, RoundTrip) {
  const char *path = "/tmp/zset_sst_test.sst";
  unlink(path);
  ZsetSSTableWriter w;
  ASSERT_EQ(w.open(path), 0);

  uint len;
  const uchar *m;
  m = member("a", &len);
  ASSERT_EQ(w.append(1.0, m, len, 2, ZsetType::kPut),
            0);  // newer version sorts first
  m = member("a", &len);
  ASSERT_EQ(w.append(1.0, m, len, 1, ZsetType::kPut), 0);
  m = member("b", &len);
  ASSERT_EQ(w.append(2.0, m, len, 1, ZsetType::kPut), 0);
  m = member("c", &len);
  ASSERT_EQ(w.append(2.0, m, len, 2, ZsetType::kDelete),
            0);  // DELETE, newest of (2,c)
  m = member("c", &len);
  ASSERT_EQ(w.append(2.0, m, len, 1, ZsetType::kPut), 0);
  ASSERT_EQ(w.finish(), 0);

  ZsetSSTableReader r;
  ASSERT_EQ(r.open(path), 0);
  ZsetSSTableReader::Iterator it;
  it.seekToFirst(&r);

  // Expected order: (1,a,s2,P) (1,a,s1,P) (2,b,s1,P) (2,c,s2,D) (2,c,s1,P).
  struct Expect {
    double score;
    const char *member;
    uint64 seq;
    ZsetType type;
  };
  const Expect exp[] = {{1.0, "a", 2, ZsetType::kPut},
                        {1.0, "a", 1, ZsetType::kPut},
                        {2.0, "b", 1, ZsetType::kPut},
                        {2.0, "c", 2, ZsetType::kDelete},
                        {2.0, "c", 1, ZsetType::kPut}};
  for (const Expect &e : exp) {
    ASSERT_TRUE(it.valid());
    EXPECT_EQ(it.entry().score, e.score);
    EXPECT_EQ(string(it.entry().member.begin(), it.entry().member.end()),
              e.member);
    EXPECT_EQ(it.entry().sequence, e.seq);
    EXPECT_EQ(it.entry().type, e.type);
    it.next();
  }
  EXPECT_FALSE(it.valid());
  r.close();
  unlink(path);
}

// Seek lands on the first entry whose internal key is >= the target.
TEST(ZsetSSTableTest, Seek) {
  const char *path = "/tmp/zset_sst_test.sst";
  unlink(path);
  ZsetSSTableWriter w;
  ASSERT_EQ(w.open(path), 0);
  uint len;
  const uchar *m;
  m = member("a", &len);
  ASSERT_EQ(w.append(1.0, m, len, 1, ZsetType::kPut), 0);
  m = member("b", &len);
  ASSERT_EQ(w.append(2.0, m, len, 1, ZsetType::kPut), 0);
  m = member("c", &len);
  ASSERT_EQ(w.append(3.0, m, len, 1, ZsetType::kPut), 0);
  m = member("d", &len);
  ASSERT_EQ(w.append(4.0, m, len, 1, ZsetType::kPut), 0);
  ASSERT_EQ(w.finish(), 0);

  ZsetSSTableReader r;
  ASSERT_EQ(r.open(path), 0);
  ZsetSSTableReader::Iterator it;

  // Exact user key at max seq.
  m = member("b", &len);
  it.seek(&r, 2.0, m, len, ~0ULL, ZsetType::kPut);
  ASSERT_TRUE(it.valid());
  EXPECT_EQ(it.entry().score, 2.0);
  EXPECT_EQ(string(it.entry().member.begin(), it.entry().member.end()), "b");

  // A key between existing ones lands on the next entry.
  m = member("bb", &len);
  it.seek(&r, 2.5, m, len, ~0ULL, ZsetType::kPut);
  ASSERT_TRUE(it.valid());
  EXPECT_EQ(string(it.entry().member.begin(), it.entry().member.end()), "c");

  // Past the end: invalid.
  m = member("z", &len);
  it.seek(&r, 10.0, m, len, ~0ULL, ZsetType::kPut);
  EXPECT_FALSE(it.valid());
  r.close();
  unlink(path);
}

// Many entries across several data blocks and restart points.
TEST(ZsetSSTableTest, MultiBlock) {
  const char *path = "/tmp/zset_sst_test.sst";
  unlink(path);
  ZsetSSTableWriter w;
  ASSERT_EQ(w.open(path), 0);
  const int kNum = 3000;
  for (int i = 0; i < kNum; i++) {
    string m = "member_" + std::to_string(i);
    // Give every 10th row a second (newer) version.
    const uchar *mb = reinterpret_cast<const uchar *>(m.data());
    if (i % 10 == 0) {
      ASSERT_EQ(
          w.append(static_cast<double>(i), mb, m.size(), 2, ZsetType::kDelete),
          0);
    }
    ASSERT_EQ(w.append(static_cast<double>(i), mb, m.size(), 1, ZsetType::kPut),
              0);
  }
  ASSERT_EQ(w.finish(), 0);
  EXPECT_GT(w.size(), 4096U);  // several blocks

  ZsetSSTableReader r;
  ASSERT_EQ(r.open(path), 0);
  ZsetSSTableReader::Iterator it;
  it.seekToFirst(&r);
  int n = 0;
  for (; it.valid(); it.next(), n++) {
    const double score = it.entry().score;
    const int i = static_cast<int>(score);
    const string m = string(it.entry().member.begin(), it.entry().member.end());
    EXPECT_EQ(m, "member_" + std::to_string(i));
    if (i % 10 == 0) {
      // The DELETE (type 2) of this row is written at seq 2 and sorts
      // before the PUT at seq 1, so it must appear first.
      EXPECT_EQ(it.entry().type, ZsetType::kDelete);
      EXPECT_EQ(it.entry().sequence, 2);
      it.next();
      EXPECT_TRUE(it.valid());
      EXPECT_EQ(it.entry().type, ZsetType::kPut);
      EXPECT_EQ(it.entry().sequence, 1);
      n++;
    } else {
      EXPECT_EQ(it.entry().type, ZsetType::kPut);
    }
  }
  EXPECT_EQ(n, kNum + kNum / 10);
  r.close();
  unlink(path);
}

// Extreme score ordering in the sortable encoding.
TEST(ZsetSSTableTest, ScoreOrderingExtremes) {
  const double vals[] = {-1e308, -1e100, -3.5, -1.0,  -0.0,
                         0.0,    1.0,    2.5,  1e100, 1e308};
  // Numeric order == byte order of the encoding.
  for (size_t i = 1; i < sizeof(vals) / sizeof(vals[0]); i++) {
    EXPECT_LT(zset_encode_score(vals[i - 1]), zset_encode_score(vals[i]));
  }
  // -inf and +inf are the extremes.
  EXPECT_LT(zset_encode_score(-std::numeric_limits<double>::infinity()),
            zset_encode_score(-1e308));
  EXPECT_LT(zset_encode_score(1e308),
            zset_encode_score(std::numeric_limits<double>::infinity()));
}

// Round-trip through the encoding preserves the exact bits, including
// the NaN payload.
TEST(ZsetSSTableTest, ScoreEncodeBits) {
  const uint64 bits[] = {0x7FF8000000000000ULL,   // quiet NaN
                         0xFFF8000000000001ULL,   // signaling NaN
                         0x8000000000000000ULL,   // -0.0
                         0x0000000000000000ULL,   // +0.0
                         0x7FF0000000000000ULL,   // +inf
                         0xFFF0000000000000ULL};  // -inf
  for (uint64 b : bits) {
    double d;
    memcpy(&d, &b, sizeof(d));
    const uint64 round =
        zset_encode_score(zset_decode_score(zset_encode_score(d)));
    EXPECT_EQ(round, zset_encode_score(d));
  }
}

// An sstable with no entries.
TEST(ZsetSSTableTest, EmptySst) {
  const char *path = "/tmp/zset_sst_edge.sst";
  unlink(path);
  ZsetSSTableWriter w;
  ASSERT_EQ(w.open(path), 0);
  ASSERT_EQ(w.finish(), 0);
  ZsetSSTableReader r;
  ASSERT_EQ(r.open(path), 0);
  ZsetSSTableReader::Iterator it;
  it.seekToFirst(&r);
  EXPECT_FALSE(it.valid());
  r.close();
  unlink(path);
}

// Members with an empty byte string and with embedded NUL bytes.
TEST(ZsetSSTableTest, OddMembers) {
  const char *path = "/tmp/zset_sst_edge.sst";
  unlink(path);
  ZsetSSTableWriter w;
  ASSERT_EQ(w.open(path), 0);
  const uchar empty[] = "";
  ASSERT_EQ(w.append(1.0, empty, 0, 1, ZsetType::kPut), 0);
  const uchar bin[] = {0x00, 0x01, 0x00, 'x', 0x00};
  ASSERT_EQ(w.append(2.0, bin, sizeof(bin), 1, ZsetType::kPut), 0);
  const char *ab = "a";
  ASSERT_EQ(
      w.append(3.0, reinterpret_cast<const uchar *>(ab), 1, 1, ZsetType::kPut),
      0);
  ASSERT_EQ(w.finish(), 0);

  ZsetSSTableReader r;
  ASSERT_EQ(r.open(path), 0);
  ZsetSSTableReader::Iterator it;
  it.seekToFirst(&r);
  ASSERT_TRUE(it.valid());
  EXPECT_EQ(it.entry().member.size(), 0U);
  EXPECT_EQ(it.entry().score, 1.0);
  it.next();
  ASSERT_TRUE(it.valid());
  EXPECT_EQ(it.entry().score, 2.0);
  EXPECT_EQ(it.entry().member.size(), sizeof(bin));
  EXPECT_EQ(memcmp(it.entry().member.data(), bin, sizeof(bin)), 0);
  it.next();
  ASSERT_TRUE(it.valid());
  EXPECT_EQ(it.entry().score, 3.0);
  it.next();
  EXPECT_FALSE(it.valid());
  r.close();
  unlink(path);
}

// A single entry larger than the block target (the block buffer grows).
TEST(ZsetSSTableTest, OversizedEntry) {
  const char *path = "/tmp/zset_sst_edge.sst";
  unlink(path);
  const size_t kLen = 5000;  // bigger than kBlockSize
  std::vector<uchar> big(kLen);
  for (size_t i = 0; i < kLen; i++) {
    big[i] = static_cast<uchar>(i % 251);
  }
  ZsetSSTableWriter w;
  ASSERT_EQ(w.open(path), 0);
  ASSERT_EQ(w.append(1.0, big.data(), big.size(), 1, ZsetType::kPut), 0);
  ASSERT_EQ(w.append(2.0, big.data(), big.size(), 1, ZsetType::kPut), 0);
  ASSERT_EQ(w.finish(), 0);

  ZsetSSTableReader r;
  ASSERT_EQ(r.open(path), 0);
  ZsetSSTableReader::Iterator it;
  it.seekToFirst(&r);
  ASSERT_TRUE(it.valid());
  EXPECT_EQ(it.entry().score, 1.0);
  EXPECT_EQ(it.entry().member.size(), kLen);
  EXPECT_EQ(memcmp(it.entry().member.data(), big.data(), kLen), 0);
  it.next();
  ASSERT_TRUE(it.valid());
  EXPECT_EQ(it.entry().score, 2.0);
  r.close();
  unlink(path);
}

// Helpers for driving a ZsetLSM directly.

static void lsm_put(ZsetLSM *lsm, const char *name, double score) {
  uint len;
  const uchar *m = member(name, &len);
  lsm->put(score, m, len);
}

// Point read by member name.
static bool lsm_get(ZsetLSM *lsm, const char *name, double *score) {
  uint len;
  const uchar *m = member(name, &len);
  return lsm->get(m, len, score);
}

// Delete a member from the lsm.
static void lsm_del(ZsetLSM *lsm, const char *name, double score) {
  uint len;
  const uchar *m = member(name, &len);
  lsm->del(score, m, len);
}

// The merged live view as a (member, score) list.
static vector<pair<string, double>> lsm_live(ZsetLSM *lsm, uint64 watermark) {
  vector<pair<string, double>> out;
  ZsetLSM::Iterator it;
  it.seekToFirst(lsm, watermark);
  while (it.valid()) {
    out.push_back({string(it.key().member.begin(), it.key().member.end()),
                   it.key().score});
    it.next();
  }
  return out;
}

// Remove every file this lsm writes (the wal plus any flushed sstables).
static void lsm_cleanup(const char *name) {
  char path[FN_REFLEN];
  snprintf(path, sizeof(path), "%s.zlog", name);
  unlink(path);
  char dir[FN_REFLEN];
  size_t dir_len = 0;
  dirname_part(dir, name, &dir_len);
  MY_DIR *d = my_dir(dir, MYF(MY_WME));
  if (d != nullptr) {
    const char *base = base_name(name);
    const std::string prefix = std::string(base) + "-";
    for (size_t i = 0; i < d->number_off_files; i++) {
      const char *fn = d->dir_entry[i].name;
      const size_t flen = strlen(fn);
      if (flen > prefix.size() + 4 &&
          strncmp(fn, prefix.c_str(), prefix.size()) == 0 &&
          strcmp(fn + flen - 4, ".sst") == 0) {
        char fp[FN_REFLEN];
        snprintf(fp, sizeof(fp), "%s/%s", dir, fn);
        unlink(fp);
      }
    }
    my_dirend(d);
  }
}

// A fresh lsm (wal + any sstables removed) with a default open().
static void lsm_open_clean(ZsetLSM *lsm, const char *name) {
  lsm_cleanup(name);
  ASSERT_EQ(lsm->open(name), 0);
}

// Merged live view over the memtable plus one flushed sstable.
TEST(ZsetLSMTest, MergedView) {
  const char *name = "/tmp/zset_lsm_test";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);
  lsm_put(&lsm, "b", 2.0);

  // Flush the memtable into an sstable; new writes land only in the
  // memtable.
  ASSERT_EQ(lsm.flush(), 0);
  lsm_put(&lsm, "c", 3.0);
  lsm_del(&lsm, "a", 1.0);

  // Live view: b(2), c(3); a is tombstoned across mem+sst.
  const vector<pair<string, double>> exp = {{"b", 2.0}, {"c", 3.0}};
  EXPECT_EQ(lsm_live(&lsm, ~0ULL), exp);
  lsm.close();
  lsm_cleanup(name);
}

// Merge across two flushed sstables plus the memtable, with versions
// split between sources.
TEST(ZsetLSMTest, MergeAcrossSources) {
  const char *name = "/tmp/zset_lsm_test";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);

  // Source 1: a(1), b(2).
  lsm_put(&lsm, "a", 1.0);
  lsm_put(&lsm, "b", 2.0);
  ASSERT_EQ(lsm.flush(), 0);

  // Source 2: b updated to 5.
  lsm_del(&lsm, "b", 2.0);
  lsm_put(&lsm, "b", 5.0);
  ASSERT_EQ(lsm.flush(), 0);

  // Memtable: c(10), delete a.
  lsm_put(&lsm, "c", 10.0);
  lsm_del(&lsm, "a", 1.0);

  // b(5) wins over b(2); a is dead; c(10).
  const vector<pair<string, double>> exp = {{"b", 5.0}, {"c", 10.0}};
  EXPECT_EQ(lsm_live(&lsm, ~0ULL), exp);

  // Seek from the merged view.
  ZsetLSM::Iterator it;
  uint len;
  const uchar *m = member("c", &len);
  it.seek(&lsm, 10.0, m, len, ~0ULL);
  ASSERT_TRUE(it.valid());
  EXPECT_EQ(it.key().score, 10.0);
  m = member("b", &len);
  it.seek(&lsm, 2.0, m, len, ~0ULL);
  ASSERT_TRUE(it.valid());
  EXPECT_EQ(it.key().score, 5.0);
  lsm.close();
  lsm_cleanup(name);
}

// The watermark filters versions written after the scan started across
// the merged view too.
TEST(ZsetLSMTest, MergedSnapshot) {
  const char *name = "/tmp/zset_lsm_test";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);  // seq 1
  lsm_put(&lsm, "b", 2.0);  // seq 2
  ASSERT_EQ(lsm.flush(), 0);
  lsm_del(&lsm, "b", 2.0);   // seq 4
  lsm_put(&lsm, "b", 20.0);  // seq 5

  // Snapshot as of the flush: a(1), b(2) - the newer b is invisible.
  const uint64 watermark = 3;
  const vector<pair<string, double>> exp = {{"a", 1.0}, {"b", 2.0}};
  EXPECT_EQ(lsm_live(&lsm, watermark), exp);
  lsm.close();
  lsm_cleanup(name);
}

// Stress the merged live view: many members with versions split between
// the memtable and the flushed sstable, some deletes, and a snapshot
// watermark that hides the newest round.
TEST(ZsetLSMTest, Stress) {
  const char *name = "/tmp/zset_lsm_test";
  const int kNum = 1000;
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);

  // Round 1 (seq 1..kNum) goes into an sstable.
  for (int i = 0; i < kNum; i++) {
    string m = "m_" + std::to_string(i);
    lsm.put(static_cast<double>(i), reinterpret_cast<const uchar *>(m.data()),
            m.size());
  }
  ASSERT_EQ(lsm.flush(), 0);

  // Round 2 (seq kNum+1..) in the memtable: update every 5th, delete
  // every 10th.
  for (int i = 0; i < kNum; i++) {
    string m = "m_" + std::to_string(i);
    const uchar *mb = reinterpret_cast<const uchar *>(m.data());
    if (i % 10 == 0) {
      lsm.del(static_cast<double>(i), mb, m.size());
    } else if (i % 5 == 0) {
      lsm.del(static_cast<double>(i), mb, m.size());
      lsm.put(static_cast<double>(i) + 1000.0, mb, m.size());
    }
  }

  // Live view without watermark: updated members moved, deleted gone.
  vector<pair<string, double>> live = lsm_live(&lsm, ~0ULL);
  EXPECT_EQ(live.size(), static_cast<size_t>(kNum - kNum / 10));
  for (const auto &p : live) {
    const int score = static_cast<int>(p.second);
    const bool updated = score >= kNum;
    const int i = updated ? score - kNum : score;
    EXPECT_EQ(p.first, "m_" + std::to_string(i));
  }

  // Snapshot as of the first round: all original values, all rows.
  vector<pair<string, double>> snap = lsm_live(&lsm, kNum + 1);
  EXPECT_EQ(snap.size(), static_cast<size_t>(kNum));
  for (size_t i = 0; i < snap.size(); i++) {
    EXPECT_EQ(snap[i].second, static_cast<double>(i));
  }
  lsm.close();
  lsm_cleanup(name);
}

// Merge with no sstables (pure memtable path).
TEST(ZsetLSMTest, MergeOnlyMem) {
  const char *name = "/tmp/zset_lsm_test";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);
  lsm_put(&lsm, "b", 2.0);
  lsm_del(&lsm, "a", 1.0);

  const vector<pair<string, double>> exp = {{"b", 2.0}};
  EXPECT_EQ(lsm_live(&lsm, ~0ULL), exp);
  lsm.close();
  lsm_cleanup(name);
}

// Merge with an empty memtable and a tombstone in the sstable that
// removes a member that the memtable re-adds.
TEST(ZsetLSMTest, ReAddAfterDelete) {
  const char *name = "/tmp/zset_lsm_test";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);  // seq 1
  lsm_del(&lsm, "a", 1.0);  // seq 2
  lsm_put(&lsm, "b", 2.0);  // seq 3
  ASSERT_EQ(lsm.flush(), 0);
  lsm_put(&lsm, "a", 5.0);  // seq 4: re-add a

  // b(2) sorts before the re-added a(5).
  const vector<pair<string, double>> exp = {{"b", 2.0}, {"a", 5.0}};
  EXPECT_EQ(lsm_live(&lsm, ~0ULL), exp);
  lsm.close();
  lsm_cleanup(name);
}

// The same user key written before and after the flush is emitted once.
TEST(ZsetLSMTest, DuplicateInternalKey) {
  const char *name = "/tmp/zset_lsm_test";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);  // seq 1 in the sstable after the flush
  ASSERT_EQ(lsm.flush(), 0);
  lsm_put(&lsm, "a", 1.0);  // seq 2 in the memtable

  // The duplicate user key is emitted once.
  const vector<pair<string, double>> exp = {{"a", 1.0}};
  EXPECT_EQ(lsm_live(&lsm, ~0ULL), exp);
  lsm.close();
  lsm_cleanup(name);
}

// Everything deleted across sources: the merged view is empty.
TEST(ZsetLSMTest, AllDeleted) {
  const char *name = "/tmp/zset_lsm_test";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);  // seq 1
  ASSERT_EQ(lsm.flush(), 0);
  lsm_del(&lsm, "a", 1.0);  // seq 2 tombstone

  // Nothing survives: the merged view is empty.
  EXPECT_TRUE(lsm_live(&lsm, ~0ULL).empty());
  lsm.close();
  lsm_cleanup(name);
}

// Seek to a user key that is dead (tombstoned) must land on the next
// live key.
TEST(ZsetLSMTest, SeekSkipsDeadKey) {
  const char *name = "/tmp/zset_lsm_test";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);
  lsm_put(&lsm, "b", 2.0);
  lsm_put(&lsm, "c", 3.0);
  ASSERT_EQ(lsm.flush(), 0);
  lsm_del(&lsm, "b", 2.0);

  // Seek to b: the dead key is skipped, landing on the next live one.
  ZsetLSM::Iterator it;
  uint len;
  const uchar *m = member("b", &len);
  it.seek(&lsm, 2.0, m, len, ~0ULL);
  ASSERT_TRUE(it.valid());
  EXPECT_EQ(it.key().score, 3.0);
  EXPECT_EQ(string(it.key().member.begin(), it.key().member.end()), "c");
  lsm.close();
  lsm_cleanup(name);
}

// A tombstone with seq exactly at the watermark voids the key in the
// snapshot. Sequences start at 1 on a fresh log, so the put is seq 2 and
// the tombstone seq 3.
TEST(ZsetLSMTest, WatermarkTombstoneBoundary) {
  const char *name = "/tmp/zset_lsm_test";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);  // seq 2
  ASSERT_EQ(lsm.flush(), 0);
  lsm_del(&lsm, "a", 1.0);  // seq 3 tombstone

  // As of seq 3 the tombstone is visible: a is gone.
  EXPECT_TRUE(lsm_live(&lsm, 3).empty());

  // As of seq 2 the delete had not happened yet: a is live.
  const vector<pair<string, double>> exp = {{"a", 1.0}};
  EXPECT_EQ(lsm_live(&lsm, 2), exp);
  lsm.close();
  lsm_cleanup(name);
}

// Bloom filter basics: present members match, absent ones are rejected.
TEST(ZsetBloomTest, Basic) {
  ZsetBloomFilter f;
  f.init(100);
  std::string a = "apple";
  std::string b = "banana";
  f.add(reinterpret_cast<const uchar *>(a.data()), a.size());
  f.add(reinterpret_cast<const uchar *>(b.data()), b.size());
  EXPECT_TRUE(
      f.may_contain(reinterpret_cast<const uchar *>(a.data()), a.size()));
  EXPECT_TRUE(
      f.may_contain(reinterpret_cast<const uchar *>(b.data()), b.size()));
  int fps = 0;
  for (int i = 0; i < 1000; i++) {
    std::string q = "absent_" + std::to_string(i);
    if (f.may_contain(reinterpret_cast<const uchar *>(q.data()), q.size())) {
      fps++;
    }
  }
  EXPECT_LT(fps, 100);  // false positives are rare
}

// A member that is not in the filter is rejected quickly, while present
// members still match.
TEST(ZsetSSTableTest, FilterSkipsAbsent) {
  const char *path = "/tmp/zset_sst_filter.sst";
  unlink(path);
  ZsetSSTableWriter w;
  ASSERT_EQ(w.open(path), 0);
  uint len;
  const uchar *m;
  m = member("apple", &len);
  ASSERT_EQ(w.append(1.0, m, len, 1, ZsetType::kPut), 0);
  m = member("banana", &len);
  ASSERT_EQ(w.append(2.0, m, len, 1, ZsetType::kPut), 0);
  ASSERT_EQ(w.finish(), 0);

  ZsetSSTableReader r;
  ASSERT_EQ(r.open(path), 0);
  m = member("apple", &len);
  EXPECT_TRUE(r.may_contain(m, len));
  m = member("banana", &len);
  EXPECT_TRUE(r.may_contain(m, len));
  m = member("cherry", &len);
  EXPECT_FALSE(r.may_contain(m, len));
  r.close();
  unlink(path);
}

// The member index resolves a member's newest version without a scan.
TEST(ZsetSSTableTest, MemberIndexFind) {
  const char *path = "/tmp/zset_sst_member.sst";
  unlink(path);
  ZsetSSTableWriter w;
  ASSERT_EQ(w.open(path), 0);
  uint len;
  const uchar *m;

  // a: put, tombstone (re-score), put, tombstone (delete). b: one put.
  m = member("a", &len);
  ASSERT_EQ(w.append(1.0, m, len, 1, ZsetType::kPut), 0);
  ASSERT_EQ(w.append(1.0, m, len, 2, ZsetType::kDelete), 0);
  ASSERT_EQ(w.append(5.0, m, len, 3, ZsetType::kPut), 0);
  ASSERT_EQ(w.append(5.0, m, len, 4, ZsetType::kDelete), 0);
  m = member("b", &len);
  ASSERT_EQ(w.append(2.0, m, len, 5, ZsetType::kPut), 0);
  ASSERT_EQ(w.finish(), 0);

  ZsetSSTableReader r;
  ASSERT_EQ(r.open(path), 0);

  uint64 seq;
  ZsetType type;
  double score;

  // a: the newest version is the seq-4 tombstone.
  m = member("a", &len);
  ASSERT_TRUE(r.find_member(m, len, &seq, &type, &score));
  EXPECT_EQ(seq, 4U);
  EXPECT_EQ(type, ZsetType::kDelete);

  // b: the seq-5 put, with its score.
  m = member("b", &len);
  ASSERT_TRUE(r.find_member(m, len, &seq, &type, &score));
  EXPECT_EQ(seq, 5U);
  EXPECT_EQ(type, ZsetType::kPut);
  EXPECT_EQ(score, 2.0);

  // A member that is not in the file.
  m = member("zzz", &len);
  EXPECT_FALSE(r.find_member(m, len, &seq, &type, &score));

  r.close();
  unlink(path);
}

// The member index spans data blocks: members in later blocks resolve.
TEST(ZsetSSTableTest, MemberIndexMultiBlock) {
  const char *path = "/tmp/zset_sst_mindex.sst";
  unlink(path);
  ZsetSSTableWriter w;
  ASSERT_EQ(w.open(path), 0);
  const int kNum = 5000;  // several 4K blocks
  for (int i = 0; i < kNum; i++) {
    string mb = "m_" + std::to_string(i);
    ASSERT_EQ(w.append(static_cast<double>(i),
                       reinterpret_cast<const uchar *>(mb.data()), mb.size(), 1,
                       ZsetType::kPut),
              0);
  }
  ASSERT_EQ(w.finish(), 0);
  EXPECT_GT(w.size(), 4096U);

  ZsetSSTableReader r;
  ASSERT_EQ(r.open(path), 0);
  const int probes[] = {0, 1, 1234, kNum - 2, kNum - 1};
  for (int i : probes) {
    string mb = "m_" + std::to_string(i);
    uint64 seq;
    ZsetType type;
    double score;
    ASSERT_TRUE(r.find_member(reinterpret_cast<const uchar *>(mb.data()),
                              mb.size(), &seq, &type, &score))
        << "member " << i;
    EXPECT_EQ(score, static_cast<double>(i));
    EXPECT_EQ(type, ZsetType::kPut);
  }

  string miss = "not_a_member";
  uint64 seq;
  ZsetType type;
  double score;
  EXPECT_FALSE(r.find_member(reinterpret_cast<const uchar *>(miss.data()),
                             miss.size(), &seq, &type, &score));

  r.close();
  unlink(path);
}

// Point reads over flushed rows resolve through the member index:
// present, deleted and absent members all answer without a scan.
TEST(ZsetLSMTest, GetAfterFlush) {
  const char *name = "/tmp/zset_lsm_getflush";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);
  lsm_put(&lsm, "b", 2.0);
  lsm_put(&lsm, "c", 3.0);
  ASSERT_EQ(lsm.flush(), 0);
  lsm_del(&lsm, "b", 2.0);  // tombstone lands in the new memtable
  lsm_put(&lsm, "d", 4.0);

  double score;
  ASSERT_TRUE(lsm_get(&lsm, "a", &score));
  EXPECT_EQ(score, 1.0);
  ASSERT_TRUE(lsm_get(&lsm, "c", &score));
  EXPECT_EQ(score, 3.0);
  EXPECT_FALSE(lsm_get(&lsm, "b", &score));    // deleted after the flush
  EXPECT_FALSE(lsm_get(&lsm, "zzz", &score));  // never existed
  ASSERT_TRUE(lsm_get(&lsm, "d", &score));     // only in the memtable
  EXPECT_EQ(score, 4.0);

  lsm.close();
  lsm_cleanup(name);
}

// A tombstone that only the sstable holds marks the member as deleted.
TEST(ZsetLSMTest, GetFlushedTombstone) {
  const char *name = "/tmp/zset_lsm_getdel";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);
  lsm_put(&lsm, "b", 2.0);
  lsm_del(&lsm, "a", 1.0);
  ASSERT_EQ(lsm.flush(), 0);  // the tombstone reaches the sstable

  double score;
  EXPECT_FALSE(lsm_get(&lsm, "a", &score));  // newest version is a tombstone
  ASSERT_TRUE(lsm_get(&lsm, "b", &score));
  EXPECT_EQ(score, 2.0);
  lsm.close();
  lsm_cleanup(name);
}

// The newest version wins when a member spans several sstables.
TEST(ZsetLSMTest, GetAcrossSstables) {
  const char *name = "/tmp/zset_lsm_getmulti";
  ZsetLSM lsm;
  lsm_open_clean(&lsm, name);
  lsm_put(&lsm, "a", 1.0);
  lsm_put(&lsm, "b", 2.0);
  ASSERT_EQ(lsm.flush(), 0);
  lsm_put(&lsm, "a", 10.0);  // newer version in the memtable
  lsm_del(&lsm, "b", 2.0);   // newer tombstone in the memtable
  ASSERT_EQ(lsm.flush(), 0);
  lsm_put(&lsm, "a", 100.0);  // newest version, currently in the memtable

  double score;
  ASSERT_TRUE(lsm_get(&lsm, "a", &score));
  EXPECT_EQ(score, 100.0);
  EXPECT_FALSE(lsm_get(&lsm, "b", &score));

  lsm.close();
  lsm_cleanup(name);
}
