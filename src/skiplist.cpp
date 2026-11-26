#include "skiplist.h"

#include <new>

namespace mdb {

namespace {
const int kBranching = 4;
}

SkipList::SkipList()
    : head_(nullptr),
      max_height_(1),
      count_(0),
      memory_(0),
      max_seq_(0),
      rng_state_(0x9e3779b9u) {
  head_ = NewNode(std::string(), 0, kTypeValue, std::string(), kMaxHeight);
  for (int i = 0; i < kMaxHeight; ++i) {
    head_->next[i].store(nullptr, std::memory_order_relaxed);
  }
}

SkipList::~SkipList() {
  SkipListNode* node = head_;
  while (node != nullptr) {
    SkipListNode* next = node->next[0].load(std::memory_order_relaxed);
    DestroyNode(node);
    node = next;
  }
}

SkipListNode* SkipList::NewNode(const std::string& key, uint64_t seq, ValueType type,
                                const std::string& value, int height) {
  size_t bytes = sizeof(SkipListNode) + sizeof(std::atomic<SkipListNode*>) * static_cast<size_t>(height - 1);
  void* memory = ::operator new(bytes);
  SkipListNode* node = static_cast<SkipListNode*>(memory);
  new (&node->key) std::string(key);
  new (&node->value) std::string(value);
  node->seq = seq;
  node->type = type;
  node->height = height;
  for (int i = 0; i < height; ++i) {
    new (&node->next[i]) std::atomic<SkipListNode*>(nullptr);
  }
  return node;
}

void SkipList::DestroyNode(SkipListNode* node) {
  node->key.~basic_string();
  node->value.~basic_string();
  ::operator delete(static_cast<void*>(node));
}

int SkipList::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight) {
    rng_state_ = rng_state_ * 1103515245u + 12345u;
    if (((rng_state_ >> 16) & 0xFFFFu) % static_cast<uint32_t>(kBranching) != 0) break;
    ++height;
  }
  return height;
}

bool SkipList::NodeIsBefore(const SkipListNode* node, const std::string& key, uint64_t seq) {
  int compare = CompareStrings(node->key, key);
  if (compare != 0) return compare < 0;
  return node->seq > seq;
}

bool SkipList::Insert(const std::string& key, uint64_t seq, ValueType type, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  SkipListNode* previous[kMaxHeight];
  SkipListNode* node = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  for (; level >= 0; --level) {
    SkipListNode* next = node->next[level].load(std::memory_order_acquire);
    while (next != nullptr && NodeIsBefore(next, key, seq)) {
      node = next;
      next = node->next[level].load(std::memory_order_acquire);
    }
    previous[level] = node;
  }

  int height = RandomHeight();
  int current_max = max_height_.load(std::memory_order_relaxed);
  if (height > current_max) {
    for (int i = current_max; i < height; ++i) previous[i] = head_;
    max_height_.store(height, std::memory_order_release);
  }

  SkipListNode* created = NewNode(key, seq, type, value, height);
  for (int i = 0; i < height; ++i) {
    created->next[i].store(previous[i]->next[i].load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
    previous[i]->next[i].store(created, std::memory_order_release);
  }

  count_.fetch_add(1, std::memory_order_relaxed);
  memory_.fetch_add(sizeof(SkipListNode) + key.size() + value.size() +
                        sizeof(std::atomic<SkipListNode*>) * static_cast<size_t>(height - 1),
                    std::memory_order_relaxed);
  uint64_t observed = max_seq_.load(std::memory_order_relaxed);
  while (seq > observed && !max_seq_.compare_exchange_weak(observed, seq)) {
  }
  return true;
}

bool SkipList::Get(const std::string& key, LookupResult* result) const {
  const SkipListNode* node = head_;
  int level = max_height_.load(std::memory_order_acquire) - 1;
  for (; level >= 0; --level) {
    const SkipListNode* next = node->next[level].load(std::memory_order_acquire);
    while (next != nullptr && CompareStrings(next->key, key) < 0) {
      node = next;
      next = node->next[level].load(std::memory_order_acquire);
    }
  }
  const SkipListNode* candidate = node->next[0].load(std::memory_order_acquire);
  if (candidate == nullptr || CompareStrings(candidate->key, key) != 0) return false;
  if (result != nullptr) {
    result->type = candidate->type;
    result->seq = candidate->seq;
    result->value = candidate->value;
  }
  return true;
}

}
