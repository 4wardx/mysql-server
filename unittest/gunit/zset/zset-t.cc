/*
  Unit tests for the ZSET core structures: versioned skiplist, memtable
  and write-ahead log.
*/

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "storage/zset/zset_memtable.h"
#include "storage/zset/zset_wal.h"

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
  EXPECT_EQ(sl.first()->seq, 3U);
  EXPECT_EQ(sl.next(sl.first())->seq, 2U);
  EXPECT_EQ(sl.next(sl.next(sl.first()))->seq, 1U);
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
  EXPECT_EQ(mem.lookup(m, len), nullptr);

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
    if (member_name(n) == "x") x_count++;
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
  EXPECT_EQ(mem.firstLive()->seq, 2U);
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
    if (i % 3 != 0) expected++;
  }
  EXPECT_EQ(mem.count(), expected);
  wal2.close();
  unlink(kWalTestPath);
}
