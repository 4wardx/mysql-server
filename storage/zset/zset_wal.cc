#include <fcntl.h>
#include <cstring>

#include "my_byteorder.h"
#include "storage/zset/zset_wal.h"

// Header size: magic(2) + type(1) + sequence(8).
static constexpr size_t kWalHeaderLen = 11;

// fsync policy: 0 = fsync after every record, 1 = batched by the caller.
ulong zset_wal_fsync = 0;

Zset_wal::~Zset_wal() {
  if (fd_ >= 0) {
    close();
  }
}

int Zset_wal::open(const char *path) {
  if (fd_ >= 0) {
    close();
  }

  path_ = path;
  fd_ = my_open(path, O_CREAT | O_RDWR, MYF(MY_WME));
  if (fd_ < 0) {
    return 1;
  }

  if (my_seek(fd_, 0, MY_SEEK_END, MYF(0)) == MY_FILEPOS_ERROR) {
    close();
    return 1;
  }

  return 0;
}

int Zset_wal::close() {
  if (fd_ < 0) {
    return 0;
  }

  int rc = my_close(fd_, MYF(0));
  fd_ = -1;

  return rc;
}

int Zset_wal::reset() {
  if (fd_ < 0) {
    return 1;
  }

  my_close(fd_, MYF(0));
  fd_ = my_open(path_.c_str(), O_CREAT | O_RDWR | O_TRUNC, MYF(MY_WME));
  if (fd_ < 0) {
    return 1;
  }

  return 0;
}

int Zset_wal::append(double score, const uchar *member, uint len,
                     uint64 sequence, Type type) {
  if (fd_ < 0) {
    return 1;
  }

  uchar header[kWalHeaderLen];
  int2store(header, kMagic);
  header[2] = static_cast<uint8_t>(type);
  int8store(header + 3, sequence);

  if (my_write(fd_, header, sizeof(header), MYF(MY_WME)) != sizeof(header)) {
    return 1;
  }

  uchar score_buf[8];
  float8store(score_buf, score);
  if (my_write(fd_, score_buf, sizeof(score_buf), MYF(MY_WME)) !=
      sizeof(score_buf)) {
    return 1;
  }

  uchar len_buf[2];
  int2store(len_buf, static_cast<uint16>(len));
  if (my_write(fd_, len_buf, sizeof(len_buf), MYF(MY_WME)) != sizeof(len_buf)) {
    return 1;
  }
  if (len > 0 && my_write(fd_, member, len, MYF(MY_WME)) != len) {
    return 1;
  }

  if (zset_wal_fsync == 0 && my_sync(fd_, MYF(MY_WME))) {
    return 1;
  }

  return 0;
}

int Zset_wal::append_clear() {
  if (fd_ < 0) {
    return 1;
  }

  uchar header[kWalHeaderLen];
  int2store(header, kMagic);
  header[2] = static_cast<uint8_t>(Type::kClear);
  int8store(header + 3, 0);  // sequence unused for CLEAR
  if (my_write(fd_, header, sizeof(header), MYF(MY_WME)) != sizeof(header)) {
    return 1;
  }

  if (zset_wal_fsync == 0 && my_sync(fd_, MYF(MY_WME))) {
    return 1;
  }

  return 0;
}

size_t Zset_wal::replay(ZsetMemTable *mem, uint64 *next_seq) {
  if (fd_ < 0) {
    return 0;
  }
  if (my_seek(fd_, 0, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    return 0;
  }

  uint64 max_seq = 0;
  size_t applied = 0;
  for (;;) {
    uchar header[kWalHeaderLen];
    uint read_len = my_read(fd_, header, sizeof(header), MYF(MY_WME));
    if (read_len == 0) {
      break;  // clean end
    }
    if (read_len != sizeof(header) || uint2korr(header) != kMagic) {
      break;  // torn or corrupted tail
    }

    const Type type = static_cast<Type>(header[2]);
    const uint64 sequence = uint8korr(header + 3);

    if (type == Type::kClear) {
      mem->clear();
      applied++;
      continue;
    }

    uchar score_buf[8];
    if (my_read(fd_, score_buf, sizeof(score_buf), MYF(MY_WME)) !=
        sizeof(score_buf)) {
      break;
    }
    const double score = float8get(score_buf);

    uchar len_buf[2];
    if (my_read(fd_, len_buf, sizeof(len_buf), MYF(MY_WME)) !=
        sizeof(len_buf)) {
      break;
    }
    const uint member_len = uint2korr(len_buf);

    uchar *member = nullptr;
    if (member_len > 0) {
      member = new uchar[member_len];
      if (my_read(fd_, member, member_len, MYF(MY_WME)) != member_len) {
        delete[] member;
        break;
      }
    }

    if (type == Type::kPut) {
      mem->put(score, member, member_len, sequence);
    } else if (type == Type::kDelete) {
      mem->tombstone(score, member, member_len, sequence);
    }
    delete[] member;

    if (sequence > max_seq) {
      max_seq = sequence;
    }
    applied++;
  }

  *next_seq = max_seq + 1;

  return applied;
}
