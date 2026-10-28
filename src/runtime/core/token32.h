#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <typeindex>
#include <vector>

namespace venture {
template <class T>
struct Token32 {
  std::uint32_t value = 0;
  explicit operator bool() const { return value != 0; }
  bool operator==(const Token32&) const = default;
};
enum class TokenLifetime { local, global, weak };

// Host objects remain host objects. Resolve pins ownership across concurrent
// erase/scope teardown; weak references resolve only while their owner lives.
// Slot generations never wrap: a slot is retired after generation 4095.
class Token32Registry {
  static constexpr std::uint32_t index_bits = 20, index_mask = (1u << index_bits) - 1;
  struct Entry {
    std::uint16_t generation = 1;
    bool live = false;
    std::type_index type{typeid(void)};
    TokenLifetime lifetime = TokenLifetime::global;
    std::uint64_t scope = 0;
    std::thread::id thread;
    std::shared_ptr<void> strong;
    std::weak_ptr<void> weak;
  };
  std::mutex mutex_;
  std::vector<Entry> entries_;
  std::vector<std::uint32_t> free_;
  std::uint64_t next_scope_ = 1;
  Entry* find(std::uint32_t value, std::type_index type) {
    auto index = value & index_mask;
    if (!index || index > entries_.size()) return nullptr;
    auto& e = entries_[index - 1];
    if (!e.live || e.generation != value >> index_bits || e.type != type ||
        (e.lifetime == TokenLifetime::local && e.thread != std::this_thread::get_id()))
      return nullptr;
    return &e;
  }
  void retire(std::uint32_t index, Entry& e) {
    e.live = false;
    e.weak.reset();
    if (e.generation < 4095) {
      ++e.generation;
      free_.push_back(index);
    }
  }

 public:
  class Scope {
    friend class Token32Registry;
    Token32Registry& registry_;
    std::uint64_t id_;
    std::thread::id thread_ = std::this_thread::get_id();

   public:
    explicit Scope(Token32Registry& registry) : registry_(registry) {
      std::lock_guard lock(registry_.mutex_);
      if (!registry_.next_scope_) throw std::overflow_error("Token32 scope exhausted");
      id_ = registry_.next_scope_++;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
      std::vector<std::shared_ptr<void>> released;
      {
        std::lock_guard lock(registry_.mutex_);
        for (std::uint32_t i = 0; i < registry_.entries_.size(); ++i) {
          auto& e = registry_.entries_[i];
          if (e.live && e.lifetime == TokenLifetime::local && e.scope == id_) {
            released.push_back(std::move(e.strong));
            registry_.retire(i, e);
          }
        }
      }  // Object deleters may reenter the registry; never run them under its lock.
    }
  };
  Token32Registry() = default;
  Token32Registry(const Token32Registry&) = delete;
  template <class T>
  Token32<T> insert(std::shared_ptr<T> object, TokenLifetime lifetime = TokenLifetime::global,
                    const Scope* scope = nullptr) {
    if (!object) return {};
    if (lifetime == TokenLifetime::local &&
        (!scope || &scope->registry_ != this || scope->thread_ != std::this_thread::get_id()))
      throw std::invalid_argument("Token32 local reference needs this thread's scope");
    std::lock_guard lock(mutex_);
    std::uint32_t index;
    if (!free_.empty()) {
      index = free_.back();
      free_.pop_back();
    } else {
      if (entries_.size() == index_mask) throw std::overflow_error("Token32 registry exhausted");
      index = static_cast<std::uint32_t>(entries_.size());
      entries_.emplace_back();
    }
    auto& e = entries_[index];
    e.type = typeid(T);
    e.lifetime = lifetime;
    e.live = true;
    e.scope = scope ? scope->id_ : 0;
    e.thread = std::this_thread::get_id();
    if (lifetime == TokenLifetime::weak)
      e.weak = object;
    else
      e.strong = std::move(object);
    return {(static_cast<std::uint32_t>(e.generation) << index_bits) | (index + 1)};
  }
  template <class T>
  std::shared_ptr<T> resolve(Token32<T> token) {
    std::lock_guard lock(mutex_);
    auto* e = find(token.value, typeid(T));
    if (!e) return {};
    return std::static_pointer_cast<T>(e->lifetime == TokenLifetime::weak ? e->weak.lock() : e->strong);
  }
  template <class T>
  bool erase(Token32<T> token) {
    std::shared_ptr<void> released;
    {
      std::lock_guard lock(mutex_);
      auto* e = find(token.value, typeid(T));
      if (!e) return false;
      released = std::move(e->strong);
      retire((token.value & index_mask) - 1, *e);
    }
    return true;
  }
};
}  // namespace venture
