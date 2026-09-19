#include "storage/zset/zset_sstable.h"

#include <algorithm>
#include <cstring>

#include <fcntl.h>

namespace {

// Footer: index_offset(8) | index_len(8) | filter_offset(8) |
// filter_len(8) | member_index_offset(8) | member_index_len(8) |
// magic(4) | version(4).
constexpr size_t kFooterLen = 56;
constexpr uint32_t kMagic = 0x5A53;
constexpr uint32_t kVersion = 2;

constexpr size_t kScoreLen = 8;
constexpr size_t kMemberLenLen = 2;
constexpr size_t kSeqLen = 8;

// Big-endian append/read so the byte order of a key sorts numerically.
void append_be16(std::vector<uchar> *out, uint16_t v) {
  out->push_back(static_cast<uchar>(v >> 8));
  out->push_back(static_cast<uchar>(v & 0xFF));
}

void append_be32(std::vector<uchar> *out, uint32_t v) {
  for (int i = 3; i >= 0; i--) {
    out->push_back(static_cast<uchar>((v >> (i * 8)) & 0xFF));
  }
}

void append_be64(std::vector<uchar> *out, uint64_t v) {
  for (int i = 7; i >= 0; i--) {
    out->push_back(static_cast<uchar>((v >> (i * 8)) & 0xFF));
  }
}

void append_be32(uchar *out, size_t at, uint32_t v) {
  for (int i = 3; i >= 0; i--) {
    out[at + (3 - i)] = static_cast<uchar>((v >> (i * 8)) & 0xFF);
  }
}

void append_be64(uchar *out, size_t at, uint64_t v) {
  for (int i = 7; i >= 0; i--) {
    out[at + (7 - i)] = static_cast<uchar>((v >> (i * 8)) & 0xFF);
  }
}

uint16_t read_be16(const uchar *p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read_be32(const uchar *p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t read_be64(const uchar *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    v = (v << 8) | p[i];
  }
  return v;
}

// Encode an internal key as its on-disk byte string. The fields are
// big-endian so that memcmp over the bytes yields the engine's key order:
// score, then member, then sequence descending (~sequence), then type
// ascending.
std::vector<uchar> encode_key(double score, const uchar *member, uint len,
                              uint64 sequence, ZsetType type) {
  std::vector<uchar> key;
  append_be64(&key, zset_encode_score(score));
  append_be16(&key, static_cast<uint16_t>(len));
  key.insert(key.end(), member, member + len);
  append_be64(&key, ~sequence);
  key.push_back(static_cast<uint8_t>(type));

  return key;
}

// Byte-wise comparison of two internal keys; the encoding is sortable, so
// memcmp order is the engine's key order (with length-prefix awareness).
int compare_keys(const uchar *a, size_t alen, const uchar *b, size_t blen) {
  const size_t n = std::min(alen, blen);
  const int c = memcmp(a, b, n);
  if (c != 0) {
    return c;
  }
  return static_cast<int>(alen) - static_cast<int>(blen);
}

}  // namespace

ZsetSSTableWriter::ZsetSSTableWriter() = default;
ZsetSSTableWriter::~ZsetSSTableWriter() {
  if (fd_ >= 0) {
    my_close(fd_, MYF(0));
  }
}

int ZsetSSTableWriter::open(const char *path) {
  if (fd_ >= 0) {
    my_close(fd_, MYF(0));
  }

  fd_ = my_open(path, O_CREAT | O_RDWR | O_TRUNC, MYF(MY_WME));
  if (fd_ < 0) {
    return 1;
  }

  size_ = 0;
  finished_ = false;
  return 0;
}

int ZsetSSTableWriter::append(double score, const uchar *member, uint len,
                              uint64 sequence, ZsetType type) {
  if (fd_ < 0 || finished_) {
    return 1;
  }

  // One entry is the 2-byte length prefix plus the encoded internal key.
  const std::vector<uchar> key = encode_key(score, member, len, sequence, type);
  const size_t entry_size = 2 + key.size();

  // If adding this entry would grow the block past the target size and
  // the block already has entries, flush it first and start a new one.
  if (block_len_ > 0 && block_len_ + entry_size > kBlockSize) {
    if (flush_block() != 0) {
      return 1;
    }
  }

  // A block's first key goes into the index so reads can binary-search
  // for the block that contains a target key.
  if (block_len_ == 0) {
    index_.push_back({key, 0, 0});
  }

  // Record a restart point at every kRestartEvery-th entry; the restart
  // array lets the reader binary-search inside the block instead of
  // scanning from the start.
  if (block_entries_ % kRestartEvery == 0) {
    if (restart_count_ >= kMaxRestarts) {
      return 1;
    }
    restart_[restart_count_++] = static_cast<uint16_t>(block_len_);
  }

  // Buffer the member hashes for the bloom filter written at finish().
  uint64 h1, h2;
  ZsetBloomFilter::hash(member, len, &h1, &h2);
  member_hashes_.push_back({h1, h2});

  // Member index: remember which block holds the member (one entry per
  // member per block; adjacent appends of the same member are merged).
  const uint32_t block = static_cast<uint32_t>(index_.size() - 1);
  const uint32_t hash = static_cast<uint32_t>(h1);
  if (hash != last_index_hash_ || block != last_index_block_) {
    member_index_.push_back({hash, block});
    last_index_hash_ = hash;
    last_index_block_ = block;
  }

  // Append the length-prefixed internal key at the end of the block.
  block_.resize(block_len_ + entry_size);
  block_[block_len_] = static_cast<uchar>(key.size() >> 8);
  block_[block_len_ + 1] = static_cast<uchar>(key.size() & 0xFF);
  memcpy(block_.data() + block_len_ + 2, key.data(), key.size());
  block_len_ += entry_size;
  block_entries_++;

  return 0;
}

int ZsetSSTableWriter::finish() {
  if (fd_ < 0 || finished_) {
    return 1;
  }
  if (flush_block() != 0) {
    return 1;
  }

  // Index block: one entry per data block, in order:
  //   first_key_len(2) | first_key | block_offset(8) | block_len(4)
  const uint64_t index_offset = size_;
  std::vector<uchar> ib;
  for (const IndexEntry &e : index_) {
    append_be16(&ib, static_cast<uint16_t>(e.first_key.size()));
    ib.insert(ib.end(), e.first_key.begin(), e.first_key.end());
    append_be64(&ib, e.offset);
    append_be32(&ib, e.length);
  }

  if (my_write(fd_, ib.data(), ib.size(), MYF(MY_WME)) != ib.size()) {
    return 1;
  }
  size_ += ib.size();

  // Fixed-size footer at the very end points at the index block and
  // carries the format magic/version so the reader can validate the file.
  // Filter block: a bloom filter over every member, so point lookups and
  // duplicate checks can skip this file for absent members.
  const uint64_t filter_offset = size_;
  ZsetBloomFilter filter;
  filter.init(member_hashes_.size());
  for (const auto &h : member_hashes_) {
    filter.add_hash(h.first, h.second);
  }
  std::vector<uchar> fb(filter.byte_size());
  filter.serialize(fb.data());
  if (my_write(fd_, fb.data(), fb.size(), MYF(MY_WME)) != fb.size()) {
    return 1;
  }
  size_ += fb.size();

  // Member index block: (hash(4) | block(4)) entries sorted by hash, so
  // a point lookup binary-searches to the blocks that can hold a member.
  std::sort(member_index_.begin(), member_index_.end(),
            [](const ZsetMemberIndexEntry &a, const ZsetMemberIndexEntry &b) {
              return a.hash != b.hash ? a.hash < b.hash : a.block < b.block;
            });
  const uint64_t member_index_offset = size_;
  std::vector<uchar> mb;
  append_be32(&mb, static_cast<uint32_t>(member_index_.size()));
  for (const ZsetMemberIndexEntry &e : member_index_) {
    append_be32(&mb, e.hash);
    append_be32(&mb, e.block);
  }
  if (my_write(fd_, mb.data(), mb.size(), MYF(MY_WME)) != mb.size()) {
    return 1;
  }
  size_ += mb.size();

  uchar footer[kFooterLen] = {0};
  append_be64(footer, 0, index_offset);
  append_be64(footer, 8, ib.size());
  append_be64(footer, 16, filter_offset);
  append_be64(footer, 24, fb.size());
  append_be64(footer, 32, member_index_offset);
  append_be64(footer, 40, mb.size());
  append_be32(footer, 48, kMagic);
  append_be32(footer, 52, kVersion);
  if (my_write(fd_, footer, kFooterLen, MYF(MY_WME)) != kFooterLen) {
    return 1;
  }
  size_ += kFooterLen;

  my_close(fd_, MYF(0));
  fd_ = -1;
  finished_ = true;

  return 0;
}

int ZsetSSTableWriter::flush_block() {
  if (block_len_ == 0) {
    return 0;
  }

  const uint64_t offset = size_;

  // The block data goes first; its offset is remembered for the index.
  if (my_write(fd_, block_.data(), block_len_, MYF(MY_WME)) != block_len_) {
    return 1;
  }
  size_ += block_len_;

  // Trailing restart array: the offsets of every kRestartEvery-th entry,
  // then the count. The reader reads the count from the last 2 bytes to
  // work out where the entries region ends.
  const size_t tail_len = 2 + (2 * restart_count_);
  std::vector<uchar> tail;
  tail.reserve(tail_len);
  for (size_t i = 0; i < restart_count_; i++) {
    append_be16(&tail, restart_[i]);
  }
  append_be16(&tail, static_cast<uint16_t>(restart_count_));
  if (my_write(fd_, tail.data(), tail.size(), MYF(MY_WME)) != tail.size()) {
    return 1;
  }
  size_ += tail.size();

  // Fill in the index entry for this block (it was pushed with the
  // block's first key when the block started).
  index_.back().offset = offset;
  index_.back().length = static_cast<uint32_t>(block_len_ + tail_len);

  block_len_ = 0;
  restart_count_ = 0;
  block_entries_ = 0;

  return 0;
}

ZsetSSTableReader::ZsetSSTableReader() = default;
ZsetSSTableReader::~ZsetSSTableReader() { close(); }

int ZsetSSTableReader::open(const char *path) {
  close();
  fd_ = my_open(path, O_RDONLY, MYF(MY_WME));
  if (fd_ < 0) {
    return 1;
  }
  if (my_seek(fd_, -static_cast<my_off_t>(kFooterLen), MY_SEEK_END, MYF(0)) ==
      MY_FILEPOS_ERROR) {
    close();
    return 1;
  }

  uchar footer[kFooterLen];
  if (my_read(fd_, footer, kFooterLen, MYF(MY_WME)) != kFooterLen ||
      read_be32(footer + 48) != kMagic || read_be32(footer + 52) != kVersion) {
    close();
    return 1;
  }
  const uint64_t index_offset = read_be64(footer);
  const uint64_t index_len = read_be64(footer + 8);
  const uint64_t filter_offset = read_be64(footer + 16);
  const uint64_t filter_len = read_be64(footer + 24);
  const uint64_t member_index_offset = read_be64(footer + 32);
  const uint64_t member_index_len = read_be64(footer + 40);

  // The index block is small; load it in one read.
  std::vector<uchar> ib(index_len);
  if (my_seek(fd_, index_offset, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    close();
    return 1;
  }
  if (my_read(fd_, ib.data(), ib.size(), MYF(MY_WME)) != ib.size()) {
    close();
    return 1;
  }

  // Parse each index entry: first key, then the block's offset/length.
  size_t pos = 0;
  while (pos < ib.size()) {
    IndexEntry e;
    const uint16_t key_len = read_be16(&ib[pos]);
    pos += 2;
    if (pos + key_len + 12 > ib.size()) {
      close();
      return 1;
    }

    // Index entry format: first_key_len(2) | first_key | offset(8) | len(4).
    e.first_key.assign(&ib[pos], &ib[pos] + key_len);
    pos += key_len;
    e.offset = read_be64(&ib[pos]);
    pos += 8;
    e.length = read_be32(&ib[pos]);
    pos += 4;
    index_.push_back(std::move(e));
  }

  // Load the bloom filter block.
  std::vector<uchar> fb(filter_len);
  if (filter_len > 0 &&
      (my_seek(fd_, filter_offset, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR ||
       my_read(fd_, fb.data(), fb.size(), MYF(MY_WME)) != fb.size())) {
    close();
    return 1;
  }
  if (filter_.deserialize(fb.data(), fb.size()) == 0) {
    close();
    return 1;
  }

  // Load the member index block: count(4) then (hash(4) | block(4)) each.
  std::vector<uchar> mb(member_index_len);
  if (member_index_len > 0 &&
      (my_seek(fd_, member_index_offset, MY_SEEK_SET, MYF(0)) ==
           MY_FILEPOS_ERROR ||
       my_read(fd_, mb.data(), mb.size(), MYF(MY_WME)) != mb.size())) {
    close();
    return 1;
  }
  if (mb.size() >= 4) {
    const uint32_t count = read_be32(mb.data());
    size_t mpos = 4;
    for (uint32_t i = 0; i < count; i++) {
      if (mpos + 8 > mb.size()) {
        close();
        return 1;
      }
      ZsetMemberIndexEntry e;
      e.hash = read_be32(&mb[mpos]);
      mpos += 4;
      e.block = read_be32(&mb[mpos]);
      mpos += 4;
      member_index_.push_back(e);
    }
  }

  return 0;
}

bool ZsetSSTableReader::may_contain(const uchar *member, uint len) const {
  return filter_.may_contain(member, len);
}

bool ZsetSSTableReader::find_member(const uchar *member, uint len,
                                    uint64 *sequence, ZsetType *type,
                                    double *score) {
  // Every version of the member has an index entry, so a hash with no
  // entry means the member is absent from this file.
  uint64 h1, h2;
  ZsetBloomFilter::hash(member, len, &h1, &h2);
  const uint32_t target = static_cast<uint32_t>(h1);

  auto it = std::lower_bound(
      member_index_.begin(), member_index_.end(), target,
      [](const ZsetMemberIndexEntry &e, uint32_t h) { return e.hash < h; });

  bool found = false;
  for (; it != member_index_.end() && it->hash == target; ++it) {
    if (!scan_block_for_member(it->block, member, len, sequence, type, score,
                               &found)) {
      return false;
    }
  }

  return found;
}

void ZsetSSTableReader::close() {
  if (fd_ >= 0) {
    my_close(fd_, MYF(0));
    fd_ = -1;
  }
  index_.clear();
}

bool ZsetSSTableReader::read_block(uint64_t offset, uint32_t length,
                                   std::vector<uchar> *out) {
  auto it = block_cache_.find(offset);
  if (it != block_cache_.end()) {
    *out = it->second;
    return true;
  }

  out->resize(length);
  if (my_seek(fd_, offset, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    return false;
  }
  if (my_read(fd_, out->data(), out->size(), MYF(MY_WME)) != out->size()) {
    return false;
  }

  // Keep blocks read by a full scan in memory. Cap the cache so a
  // compaction that leaves many large sstables does not pin them all.
  constexpr size_t kMaxBlocks = 4096;
  if (block_cache_.size() >= kMaxBlocks) {
    block_cache_.clear();
  }
  block_cache_.emplace(offset, *out);
  return true;
}

bool ZsetSSTableReader::Iterator::load_block(ZsetSSTableReader *reader,
                                             uint64_t offset, uint32_t length) {
  if (!reader->read_block(offset, length, &block_)) {
    return false;
  }

  // Last 2 bytes are the restart count; the entries region is everything
  // before the restart array.
  const size_t restart_count = read_be16(&block_[length - 2]);
  block_len_ = length - 2 - (2 * restart_count);

  entry_pos_ = 0;
  entry_end_ = 0;

  return true;
}

void ZsetSSTableReader::Iterator::seekToFirst(ZsetSSTableReader *reader) {
  reader_ = reader;
  valid_ = false;
  if (reader->index_.empty()) {
    return;
  }

  index_pos_ = 0;
  if (!load_block(reader, reader->index_[0].offset, reader->index_[0].length)) {
    return;
  }

  valid_ = parse_entry();
}

void ZsetSSTableReader::Iterator::seek(ZsetSSTableReader *reader, double score,
                                       const uchar *member, uint len,
                                       uint64 sequence, ZsetType type) {
  reader_ = reader;
  valid_ = false;
  if (reader->index_.empty()) {
    return;
  }
  const std::vector<uchar> target =
      encode_key(score, member, len, sequence, type);

  index_pos_ = reader->find_block(score, member, len, sequence, type);
  if (!load_block(reader, reader->index_[index_pos_].offset,
                  reader->index_[index_pos_].length)) {
    return;
  }

  // Walk the block from its start until the first entry >= the target.
  // (A restart-array binary search would skip the linear scan, but the
  // block holds only a few hundred entries, so linear is fine.)
  for (;;) {
    if (entry_pos_ >= block_len_) {
      next();  // target is past this block: continue in the next one
      return;
    }
    if (!parse_entry()) {
      return;
    }
    if (compare_keys(target.data(), target.size(), &block_[entry_pos_ + 2],
                     entry_end_ - entry_pos_ - 2) <= 0) {
      valid_ = true;
      return;
    }

    entry_pos_ = entry_end_;
  }
}

bool ZsetSSTableReader::Iterator::parse_entry() {
  if (entry_pos_ + 2 > block_len_) {
    return false;
  }
  const uint16_t key_len = read_be16(&block_[entry_pos_]);
  if (entry_pos_ + 2 + key_len > block_len_) {
    return false;
  }
  const uchar *key = &block_[entry_pos_ + 2];

  // Internal key layout: encoded score(8) | member_len(2) | member |
  // ~sequence(8) | type(1). The score and sequence are order-encoded, so decode
  // reverses the transform (~ for sequence, the sortable mapping for score).
  entry_.score = zset_decode_score(read_be64(key));
  const uint16_t member_len = read_be16(key + kScoreLen);
  entry_.member.assign(key + kScoreLen + kMemberLenLen,
                       key + kScoreLen + kMemberLenLen + member_len);
  entry_.sequence = ~read_be64(key + kScoreLen + kMemberLenLen + member_len);
  entry_.type = static_cast<ZsetType>(
      key[kScoreLen + kMemberLenLen + member_len + kSeqLen]);
  entry_end_ = entry_pos_ + 2 + key_len;

  return true;
}

void ZsetSSTableReader::Iterator::next() {
  if (!valid_) {
    return;
  }

  // Within the current block: the next entry starts right after this one.
  entry_pos_ = entry_end_;
  if (entry_pos_ < block_len_) {
    valid_ = parse_entry();
    return;
  }

  // Block exhausted: load the following block, if there is one.
  if (index_pos_ + 1 < reader_->index_.size()) {
    index_pos_++;
    if (load_block(reader_, reader_->index_[index_pos_].offset,
                   reader_->index_[index_pos_].length)) {
      valid_ = parse_entry();
      return;
    }
  }

  valid_ = false;
}

size_t ZsetSSTableReader::find_block(double score, const uchar *member,
                                     uint len, uint64 sequence,
                                     ZsetType type) const {
  const std::vector<uchar> target =
      encode_key(score, member, len, sequence, type);

  size_t lo = 0;
  size_t hi = index_.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c =
        compare_keys(target.data(), target.size(), index_[mid].first_key.data(),
                     index_[mid].first_key.size());
    if (c < 0) {
      hi = mid;  // target sorts before this block's first key
    } else {
      lo = mid + 1;
    }
  }

  return lo == 0 ? 0 : lo - 1;
}

bool ZsetSSTableReader::scan_block_for_member(uint32_t block,
                                              const uchar *member, uint len,
                                              uint64 *sequence, ZsetType *type,
                                              double *score, bool *found) {
  if (block >= index_.size()) {
    return false;
  }

  std::vector<uchar> buf;
  if (!read_block(index_[block].offset, index_[block].length, &buf)) {
    return false;
  }

  // Last 2 bytes are the restart count; entries precede the array.
  const size_t restart_count = read_be16(&buf[buf.size() - 2]);
  const size_t entries_len = buf.size() - 2 - 2 * restart_count;

  size_t pos = 0;
  while (pos + 2 <= entries_len) {
    const uint16_t key_len = read_be16(&buf[pos]);
    if (pos + 2 + key_len > entries_len) {
      return false;
    }
    const uchar *key = &buf[pos + 2];

    const uint16_t member_len = read_be16(key + kScoreLen);
    if (member_len == len &&
        memcmp(key + kScoreLen + kMemberLenLen, member, len) == 0) {
      const uint64 seq =
          ~read_be64(key + kScoreLen + kMemberLenLen + member_len);
      const ZsetType t = static_cast<ZsetType>(
          key[kScoreLen + kMemberLenLen + member_len + kSeqLen]);
      if (!*found || seq > *sequence) {
        *found = true;
        *sequence = seq;
        *type = t;
        *score = zset_decode_score(read_be64(key));
      }
    }

    pos += 2 + key_len;
  }

  return true;
}
