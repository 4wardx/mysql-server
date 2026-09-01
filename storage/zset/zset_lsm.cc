#include "storage/zset/zset_lsm.h"

#include <cstring>

#include "my_dir.h"
#include "my_io.h"
#include "my_sys.h"
#include "storage/zset/zset_skiplist.h"

ZsetLSM::ZsetLSM() = default;

ZsetLSM::~ZsetLSM() { close(); }

int ZsetLSM::open(const char *name) {
  close();
  name_ = name;

  // Open every <table>-<N>.sst next to the table.
  char dir[FN_REFLEN];
  size_t dir_len = 0;
  dirname_part(dir, name, &dir_len);
  const char *base = base_name(name);
  const std::string prefix = std::string(base) + "-";
  const std::string suffix = ".sst";
  MY_DIR *d = my_dir(dir, MYF(MY_WME));

  if (d != nullptr) {
    for (size_t i = 0; i < d->number_off_files; i++) {
      const char *fn = d->dir_entry[i].name;
      const size_t flen = strlen(fn);

      if (flen <= prefix.size() + suffix.size() ||
          strncmp(fn, prefix.c_str(), prefix.size()) != 0 ||
          strcmp(fn + flen - suffix.size(), suffix.c_str()) != 0) {
        continue;
      }

      char path[FN_REFLEN];
      snprintf(path, sizeof(path), "%s/%s", dir, fn);
      ZsetSSTableReader *r = new ZsetSSTableReader();
      if (r->open(path) == 0) {
        ssts_.push_back(r);
      } else {
        delete r;
      }
    }

    my_dirend(d);
  }

  // Replay the WAL into the memtable; the hash then covers the recent
  // writes, older data stays in the sstables.
  char wal_path[FN_REFLEN];
  fn_format(wal_path, name, "", ".zlog", MY_REPLACE_EXT | MY_UNPACK_FILENAME);
  if (wal_.open(wal_path) != 0) {
    return 1;
  }
  wal_.replay(&mem_, &sequence_);

  return 0;
}

void ZsetLSM::close() {
  for (ZsetSSTableReader *r : ssts_) {
    delete r;
  }
  ssts_.clear();
  wal_.close();
}

void ZsetLSM::put(double score, const uchar *member, uint len) {
  const uint64 sequence = ++sequence_;
  wal_.append(score, member, len, sequence, Zset_wal::Type::kPut);
  mem_.put(score, member, len, sequence);
}

void ZsetLSM::del(double score, const uchar *member, uint len) {
  const uint64 sequence = ++sequence_;
  wal_.append(score, member, len, sequence, Zset_wal::Type::kDelete);
  mem_.tombstone(score, member, len, sequence);
}

int ZsetLSM::clear() {
  if (wal_.append_clear() != 0) {
    return 1;
  }

  mem_.clear();
  for (ZsetSSTableReader *r : ssts_) {
    delete r;
  }
  ssts_.clear();

  // Remove the sstable files themselves.
  char dir[FN_REFLEN];
  size_t dir_len = 0;
  dirname_part(dir, name_.c_str(), &dir_len);
  MY_DIR *d = my_dir(dir, MYF(MY_WME));

  if (d != nullptr) {
    const char *base = base_name(name_.c_str());
    const std::string prefix = std::string(base) + "-";

    for (size_t i = 0; i < d->number_off_files; i++) {
      const char *fn = d->dir_entry[i].name;
      const size_t flen = strlen(fn);

      if (flen > prefix.size() + 4 &&
          strncmp(fn, prefix.c_str(), prefix.size()) == 0 &&
          strcmp(fn + flen - 4, ".sst") == 0) {
        char fp[FN_REFLEN];
        snprintf(fp, sizeof(fp), "%s/%s", dir, fn);
        my_delete(fp, MYF(0));
      }
    }

    my_dirend(d);
  }

  return 0;
}

bool ZsetLSM::get(const uchar *member, uint len, double *score) const {
  // Fast path: the row is still in the memtable.
  if (mem_.get(member, len, score)) {
    return true;
  }

  // The row may live in an sstable. Skip the files whose bloom filter
  // rules the member out; only when some file may hold it do we scan the
  // merged live view.
  bool any_may = false;
  for (ZsetSSTableReader *r : ssts_) {
    if (r->may_contain(member, len)) {
      any_may = true;
      break;
    }
  }
  if (!any_may) {
    return false;
  }

  Iterator it;
  it.seekToFirst(this, ~0ULL);
  while (it.valid()) {
    const Key &k = it.key();
    if (k.member.size() == len && memcmp(k.member.data(), member, len) == 0) {
      *score = k.score;
      return true;
    }
    it.next();
  }

  return false;
}

size_t ZsetLSM::count() const {
  size_t n = 0;
  Iterator it;
  it.seekToFirst(this, ~0ULL);
  while (it.valid()) {
    n++;
    it.next();
  }
  return n;
}

int ZsetLSM::flush() {
  if (name_.empty()) {
    return 1;
  }

  // Freeze the memtable's internal keys into a new sstable. The seq in
  // the file name keeps each flush's file unique.
  char path[FN_REFLEN];
  snprintf(path, sizeof(path), "%s-%06llu.sst", name_.c_str(),
           static_cast<unsigned long long>(sequence_));

  // Write the memtable's internal keys, in order, into the sstable.
  ZsetSSTableWriter w;
  if (w.open(path) != 0) {
    return 1;
  }

  for (ZNode *n = mem_.skiplist()->first(); n != nullptr;
       n = mem_.skiplist()->next(n)) {
    if (w.append(n->score, n->member, n->member_len, n->sequence, n->type) !=
        0) {
      return 1;
    }
  }

  if (w.finish() != 0) {
    return 1;
  }

  ZsetSSTableReader *r = new ZsetSSTableReader();
  if (r->open(path) != 0) {
    delete r;
    return 1;
  }

  ssts_.push_back(r);

  // Drop the memtable (and its live-view hash); reads now come from the
  // sstables via the merged iterator.
  mem_.clear();

  // Reset the log: its records are now safe in the sstable.
  wal_.reset();
  return 0;
}

// ---------------------------------------------------------------------------
// Merged iterator
// ---------------------------------------------------------------------------

void ZsetLSM::Iterator::seekToFirst(const ZsetLSM *lsm, uint64 max_seq) {
  lsm_ = lsm;

  max_sequence_ = max_seq;
  sources_.clear();
  sources_.resize(1 + lsm_->ssts_.size());
  sources_[0].sst_index = 0;
  sources_[0].node = lsm_->mem_.skiplist()->first();

  for (size_t i = 0; i < lsm_->ssts_.size(); i++) {
    Source &s = sources_[i + 1];
    s.sst_index = i + 1;
    s.sst.seekToFirst(lsm_->ssts_[i]);
  }

  valid_ = false;
  find_next_live();
}

void ZsetLSM::Iterator::seek(const ZsetLSM *lsm, double score,
                             const uchar *member, uint len, uint64 max_seq) {
  lsm_ = lsm;

  max_sequence_ = max_seq;
  sources_.clear();
  sources_.resize(1 + lsm_->ssts_.size());
  for (size_t i = 0; i < sources_.size(); i++) {
    sources_[i].sst_index = i;
  }

  seek_sources(score, member, len, ~0ULL, ZsetType::kPut);
  valid_ = false;
  find_next_live();
}

void ZsetLSM::Iterator::next() {
  if (!valid_) {
    return;
  }
  // Skip older versions of the key we just returned.
  skip_user_key(key_);
  find_next_live();
}

bool ZsetLSM::Iterator::valid_source(const Source &s) const {
  if (s.sst_index == 0) {
    return s.node != nullptr;
  }
  return s.sst.valid();
}

ZsetLSM::Key ZsetLSM::Iterator::current_key(const Source &s) const {
  Key k;
  if (s.sst_index == 0) {
    k.score = s.node->score;
    k.member.assign(reinterpret_cast<const char *>(s.node->member),
                    s.node->member_len);
    k.sequence = s.node->sequence;
    k.type = s.node->type;
  } else {
    const ZsetSSTableReader::Entry &e = s.sst.entry();
    k.score = e.score;
    k.member.assign(reinterpret_cast<const char *>(e.member.data()),
                    e.member.size());
    k.sequence = e.sequence;
    k.type = e.type;
  }
  return k;
}

void ZsetLSM::Iterator::advance(Source *s) {
  if (s->sst_index == 0) {
    s->node =
        s->node == nullptr ? nullptr : lsm_->mem_.skiplist()->next(s->node);
  } else {
    s->sst.next();
  }
}

ZsetLSM::Iterator::Source *ZsetLSM::Iterator::peek_leader() {
  Source *leader = nullptr;
  Key leader_key;
  for (Source &s : sources_) {
    if (!valid_source(s)) {
      continue;
    }

    // Decode each source's current key exactly once and reuse it for the
    // comparison.
    const Key cur = current_key(s);
    if (leader == nullptr ||
        zset_compare_keys(
            cur.score, reinterpret_cast<const uchar *>(cur.member.data()),
            cur.member.size(), cur.sequence, cur.type, leader_key.score,
            reinterpret_cast<const uchar *>(leader_key.member.data()),
            leader_key.member.size(), leader_key.sequence,
            leader_key.type) < 0) {
      leader = &s;
      leader_key = cur;
    }
  }

  return leader;
}

bool ZsetLSM::Iterator::same_user_key(const Key &a, const Key &b) {
  return a.score == b.score && a.member.size() == b.member.size() &&
         memcmp(a.member.data(), b.member.data(), a.member.size()) == 0;
}

void ZsetLSM::Iterator::skip_user_key(const Key &k) {
  for (Source &s : sources_) {
    while (valid_source(s)) {
      const Key cur = current_key(s);
      if (cur.score != k.score || cur.member.size() != k.member.size() ||
          memcmp(cur.member.data(), k.member.data(), k.member.size()) != 0) {
        break;
      }

      advance(&s);
    }
  }
}

void ZsetLSM::Iterator::seek_sources(double score, const uchar *member,
                                     uint len, uint64 sequence, ZsetType type) {
  // Memtable: the skiplist cursor seeks to the first internal key >= the
  // target at max seq.
  ZsetSkiplist::Cursor cur(lsm_->mem_.skiplist());
  cur.seekTo(score, member, len);
  sources_[0].node = cur.valid() ? cur.current() : nullptr;

  // Sstable cursors.
  for (size_t i = 1; i < sources_.size(); i++) {
    sources_[i].sst.seek(lsm_->ssts_[i - 1], score, member, len, sequence,
                         type);
  }
}

void ZsetLSM::Iterator::find_next_live() {
  for (;;) {
    Source *leader = peek_leader();
    if (leader == nullptr) {
      valid_ = false;
      return;
    }
    // The newest internal key of the current user key: versions of the
    // same user key sort before any other key, so the leader's user key
    // owns this position.
    const Key first = current_key(*leader);

    // Walk the user key's versions (newest first across sources) to find
    // the newest one with seq <= max_seq, mirroring the memtable
    // watermark.
    Key active;
    bool found = false;
    Source *s = leader;
    for (;;) {
      const Key k = current_key(*s);
      if (!same_user_key(k, first)) {
        break;  // walked past the user key; every version is newer than max_seq
      }
      if (k.sequence <= max_sequence_) {
        active = k;
        found = true;
        break;
      }
      advance(s);
      s = peek_leader();
      if (s == nullptr) {
        break;
      }
    }

    if (found) {
      if (active.type == ZsetType::kPut) {
        key_ = active;
        valid_ = true;
        return;
      }
      // The active version is a tombstone: the user key is dead.
    }

    // Drop every remaining version of the user key and keep looking.
    skip_user_key(first);
  }
}
