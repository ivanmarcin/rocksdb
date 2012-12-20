// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Decodes the blocks generated by block_builder.cc.

#include "table/block.h"

#include <vector>
#include <algorithm>
#include "leveldb/db.h"
#include "leveldb/comparator.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/logging.h"

namespace {
uint32_t kBytesPerRestart = 2;
}

namespace leveldb {

inline uint32_t Block::NumRestarts() const {
  assert(size_ >= 2*sizeof(uint32_t));
  return DecodeFixed32(data_ + size_ - sizeof(uint32_t));
}

Block::Block(const BlockContents& contents)
    : data_(contents.data.data()),
      size_(contents.data.size()),
      owned_(contents.heap_allocated) {
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;  // Error marker
  } else {
    restart_offset_ = size_ - (1 + NumRestarts()) * sizeof(uint32_t);
    if (restart_offset_ > size_ - sizeof(uint32_t)) {
      // The size is too small for NumRestarts() and therefore
      // restart_offset_ wrapped around.
      size_ = 0;
    }
  }
}

Block::~Block() {
  if (owned_) {
    delete[] data_;
  }
}

// Helper routine: decode the next block entry starting at "p",
// storing the number of shared key bytes, non_shared key bytes,
// and the length of the value in "*shared", "*non_shared", and
// "*value_length", respectively.  Will not derefence past "limit".
//
// If any errors are detected, returns NULL.  Otherwise, returns a
// pointer to the key delta (just past the three decoded values).
static inline const char* DecodeEntry(const char* p, const char* limit,
                                      uint32_t* shared,
                                      uint32_t* non_shared,
                                      uint32_t* value_length) {
  if (limit - p < 3) return NULL;
  *shared = reinterpret_cast<const unsigned char*>(p)[0];
  *non_shared = reinterpret_cast<const unsigned char*>(p)[1];
  *value_length = reinterpret_cast<const unsigned char*>(p)[2];
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: all three values are encoded in one byte each
    p += 3;
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == NULL) return NULL;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == NULL) return NULL;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == NULL) return NULL;
  }

  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
    return NULL;
  }
  return p;
}

class Block::Iter : public Iterator {
 protected:
  const Comparator* const comparator_;
  const char* const data_;      // underlying block contents
  uint32_t const restarts_;     // Offset of restart array (list of fixed32)
  uint32_t const num_restarts_; // Number of uint32_t entries in restart array

  // current_ is offset in data_ of current entry.  >= restarts_ if !Valid
  uint32_t current_;
  uint32_t restart_index_;  // Index of restart block in which current_ falls
  uint32_t restart_offset_; // Offset from last restart in number of keys.
  std::string key_;
  Slice value_;
  Status status_;

  inline int Compare(const Slice& a, const Slice& b) const {
    return comparator_->Compare(a, b);
  }

  // Return the offset in data_ just past the end of the current entry.
  inline uint32_t NextEntryOffset() const {
    return (value_.data() + value_.size()) - data_;
  }

  uint32_t GetRestartPoint(uint32_t index) {
    assert(index < num_restarts_);
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    restart_offset_ = static_cast<uint32_t>(-1); // ParseNextKey() will
                                                 // increment by 1
    // current_ will be fixed by ParseNextKey();

    // ParseNextKey() starts at the end of value_, so set value_ accordingly
    uint32_t offset = GetRestartPoint(index);
    value_ = Slice(data_ + offset, 0);
  }

 public:
  Iter(const Comparator* comparator,
       const char* data,
       uint32_t restarts,
       uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts_),
        restart_index_(num_restarts_),
        restart_offset_(0) {
    assert(num_restarts_ > 0);
  }

  virtual ~Iter() {
  }

  virtual bool Valid() const { return current_ < restarts_; }
  virtual Status status() const { return status_; }
  virtual Slice key() const {
    assert(Valid());
    return key_;
  }
  virtual Slice value() const {
    assert(Valid());
    return value_;
  }

  virtual void Next() {
    assert(Valid());
    ParseNextKey();
  }

  virtual void Prev() {
    assert(Valid());

    // Scan backwards to a restart point before current_
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) {
        // No more entries
        current_ = restarts_;
        restart_index_ = num_restarts_;
        restart_offset_ = 0;
        return;
      }
      restart_index_--;
    }

    SeekToRestartPoint(restart_index_);
    do {
      // Loop until end of current entry hits the start of original entry
    } while (ParseNextKey() && NextEntryOffset() < original);
  }

  virtual void Seek(const Slice& target) {
    // Binary search in restart array to find the first restart point
    // with a key >= target
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      uint32_t mid = (left + right + 1) / 2;
      uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared, non_shared, value_length;
      const char* key_ptr = DecodeEntry(data_ + region_offset,
                                        data_ + restarts_,
                                        &shared, &non_shared, &value_length);
      if (key_ptr == NULL || (shared != 0)) {
        CorruptionError();
        return;
      }
      Slice mid_key(key_ptr, non_shared);
      if (Compare(mid_key, target) < 0) {
        // Key at "mid" is smaller than "target".  Therefore all
        // blocks before "mid" are uninteresting.
        left = mid;
      } else {
        // Key at "mid" is >= "target".  Therefore all blocks at or
        // after "mid" are uninteresting.
        right = mid - 1;
      }
    }

    // Linear search (within restart block) for first key >= target
    SeekToRestartPoint(left);
    while (true) {
      if (!ParseNextKey()) {
        return;
      }
      if (Compare(key_, target) >= 0) {
        return;
      }
    }
  }

  virtual void SeekToFirst() {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  virtual void SeekToLast() {
    SeekToRestartPoint(num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < restarts_) {
      // Keep skipping
    }
  }

 private:
  friend class Block;

  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    restart_offset_ = 0;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_.clear();
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    ++restart_offset_;
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;  // Restarts come right after data
    if (p >= limit) {
      // No more entries to return.  Mark as invalid.
      current_ = restarts_;
      restart_index_ = num_restarts_;
      restart_offset_ = 0;
      return false;
    }

    // Decode next entry
    uint32_t shared, non_shared, value_length;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == NULL || key_.size() < shared) {
      CorruptionError();
      return false;
    } else {
      key_.resize(shared);
      key_.append(p, non_shared);
      value_ = Slice(p + non_shared, value_length);
      while (restart_index_ + 1 < num_restarts_ &&
             GetRestartPoint(restart_index_ + 1) < current_) {
        ++restart_index_;
        restart_offset_ = 0;
      }
      return true;
    }
  }
};

class Block::MetricsIter : public Block::Iter {
 private:
  BlockMetrics* metrics_;

 public:
  MetricsIter(const Comparator* comparator,
              const char* data,
              uint32_t restarts,
              uint32_t num_restarts,
              BlockMetrics* metrics)
      : Block::Iter(comparator, data, restarts, num_restarts),
        metrics_(metrics) {
  }

  virtual ~MetricsIter() {
  }

  virtual void Next() {
    Block::Iter::Next();
    RecordAccess();
  }

  virtual void Prev() {
    Block::Iter::Prev();
    RecordAccess();
  }

  virtual void Seek(const Slice& target) {
    Block::Iter::Seek(target);
    RecordAccess();
  }

  virtual void SeekToFirst() {
    Block::Iter::SeekToFirst();
    RecordAccess();
  }

  virtual void SeekToLast() {
    Block::Iter::SeekToLast();
    RecordAccess();
  }

 private:
  void RecordAccess() {
    if (metrics_ != NULL && Valid()) {
      metrics_->RecordAccess(restart_index_, restart_offset_);
    }
  }
};

Iterator* Block::NewIterator(const Comparator* cmp) {
  if (size_ < 2*sizeof(uint32_t)) {
    return NewErrorIterator(Status::Corruption("bad block contents"));
  }
  const uint32_t num_restarts = NumRestarts();
  if (num_restarts == 0) {
    return NewEmptyIterator();
  } else {
    return new Iter(cmp, data_, restart_offset_, num_restarts);
  }
}

Iterator* Block::NewMetricsIterator(const Comparator* cmp,
                                    uint64_t file_number,
                                    uint64_t block_offset,
                                    BlockMetrics** metrics) {
  assert(metrics != NULL);

  *metrics = NULL;
  if (size_ < 2*sizeof(uint32_t)) {
    return NewErrorIterator(Status::Corruption("bad block contents"));
  }
  const uint32_t num_restarts = NumRestarts();
  if (num_restarts == 0) {
    return NewEmptyIterator();
  } else {
    *metrics = new BlockMetrics(file_number, block_offset, num_restarts, kBytesPerRestart);
    return new MetricsIter(cmp, data_, restart_offset_, num_restarts,
                           *metrics);
  }
}

bool Block::IsHot(const Iterator* iter, const BlockMetrics& bm) const {
  assert(iter != NULL);
  assert(dynamic_cast<const Iter*>(iter) != NULL);

  const Iter* it = reinterpret_cast<const Iter*>(iter);

  assert(it->data_ == data_);
  assert(it->Valid());
  assert(NumRestarts() == bm.num_restarts_);

  return bm.IsHot(it->restart_index_, it->restart_offset_);
}

BlockMetrics::BlockMetrics(uint64_t file_number, uint64_t block_offset,
                           uint32_t num_restarts, uint32_t bytes_per_restart)
  : file_number_(file_number),
    block_offset_(block_offset),
    num_restarts_(num_restarts),
    bytes_per_restart_(bytes_per_restart) {
  metrics_ = new char[num_restarts_*bytes_per_restart_]();
}

BlockMetrics::BlockMetrics(uint64_t file_number, uint64_t block_offset,
                           uint32_t num_restarts, uint32_t bytes_per_restart,
                           const std::string& data)
  : file_number_(file_number),
    block_offset_(block_offset),
    num_restarts_(num_restarts),
    bytes_per_restart_(bytes_per_restart) {
  assert(data.size() == num_restarts_*bytes_per_restart_);
  metrics_ = new char[num_restarts_*bytes_per_restart_];
  memcpy(metrics_, data.data(), data.size());
}

BlockMetrics::~BlockMetrics() {
  delete[] metrics_;
}

BlockMetrics* BlockMetrics::Create(uint64_t file_number, uint64_t block_offset,
                                   const std::string& db_value) {
  Slice data(db_value);

  uint32_t num_restarts;
  uint32_t bytes_per_restart;

  if (!GetVarint32(&data, &num_restarts)) return NULL;
  if (!GetVarint32(&data, &bytes_per_restart)) return NULL;

  return new BlockMetrics(file_number, block_offset, num_restarts,
                          bytes_per_restart, data.ToString());
}

BlockMetrics* BlockMetrics::Create(const std::string& db_key,
                                   const std::string& db_value) {
  if (db_key.size() != 16) return NULL;

  uint64_t file_number = DecodeFixed64(db_key.data());
  uint64_t block_offset = DecodeFixed64(db_key.data()+8);

  return Create(file_number, block_offset, db_value);
}

void BlockMetrics::RecordAccess(uint32_t restart_index,
                                uint32_t restart_offset) {
  unsigned char* metrics = reinterpret_cast<unsigned char*>(metrics_);
  size_t bitIdx = restart_offset % (bytes_per_restart_ * 8u);
  size_t byteIdx = restart_index*bytes_per_restart_ + bitIdx/8;

  metrics[byteIdx] |= 1 << (bitIdx%8);
}

bool BlockMetrics::IsHot(uint32_t restart_index,
                         uint32_t restart_offset) const {
  unsigned char* metrics = reinterpret_cast<unsigned char*>(metrics_);
  size_t bitIdx = restart_offset % (bytes_per_restart_ * 8u);
  size_t byteIdx = restart_index*bytes_per_restart_ + bitIdx/8;

  return (metrics[byteIdx] & (1 << (bitIdx%8))) != 0;
}

std::string BlockMetrics::GetDBKey() const {
  std::string key;
  PutFixed64(&key, file_number_);
  PutFixed64(&key, bytes_per_restart_);
  return key;
}

std::string BlockMetrics::GetDBValue() const {
  std::string value;
  PutVarint32(&value, num_restarts_);
  PutVarint32(&value, bytes_per_restart_);
  value.append(metrics_, num_restarts_*bytes_per_restart_);

  return value;
}

bool BlockMetrics::IsCompatible(const BlockMetrics* bm) const {
  return (bm != NULL && bm->num_restarts_ == num_restarts_ &&
          bm->bytes_per_restart_ == bytes_per_restart_ &&
          bm->file_number_ == file_number_ &&
          bm->block_offset_ == block_offset_);
}

void BlockMetrics::Join(const BlockMetrics* bm) {
  assert(IsCompatible(bm));

  for (size_t i = 0; i < num_restarts_ * bytes_per_restart_; ++i) {
    metrics_[i] |= bm->metrics_[i];
  }
}

}  // namespace leveldb
