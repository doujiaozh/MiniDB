#ifndef MINIDB_SKIPLIST_H
#define MINIDB_SKIPLIST_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "util.h"

namespace mdb {

struct SkipListNode {
  std::string key;
  std::string value;
  uint64_t seq;
  ValueType type;
  int height;
  std::atomic<SkipListNode*> next[1];
};

class SkipList {
 public:
  static const int kMaxHeight = 20;

  SkipList();
  ~SkipList();

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  bool Insert(const std::string& key, uint64_t seq, ValueType type, const std::string& value);
  bool Get(const std::string& key, LookupResult* result) const;

  uint64_t Count() const { return count_.load(std::memory_order_relaxed); }
  size_t ApproximateMemoryUsage() const { return memory_.load(std::memory_order_relaxed); }
  uint64_t MaxSeq() const { return max_seq_.load(std::memory_order_relaxed); }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : node_(list->head_) {}
    bool Valid() const { return node_ != nullptr; }
    void Next() {
      if (node_ != nullptr) node_ = node_->next[0].load(std::memory_order_acquire);
    }
    const std::string& key() const { return node_->key; }
    const std::string& value() const { return node_->value; }
    uint64_t seq() const { return node_->seq; }
    ValueType type() const { return node_->type; }

   private:
    const SkipListNode* node_;
  };

  Iterator NewIterator() const {
    Iterator iterator(this);
    iterator.Next();
    return iterator;
  }

 private:
  int RandomHeight();
  static bool NodeIsBefore(const SkipListNode* node, const std::string& key, uint64_t seq);
  SkipListNode* NewNode(const std::string& key, uint64_t seq, ValueType type,
                        const std::string& value, int height);
  static void DestroyNode(SkipListNode* node);

  SkipListNode* head_;
  std::atomic<int> max_height_;
  std::atomic<uint64_t> count_;
  std::atomic<size_t> memory_;
  std::atomic<uint64_t> max_seq_;
  mutable std::mutex mutex_;
  uint32_t rng_state_;
};

}

#endif
