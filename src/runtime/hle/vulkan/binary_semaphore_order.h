#pragma once
// Submission-order bookkeeping, not a replacement for native semaphore waits.
// The backend retains actual Vulkan synchronization and reports issue() only
// after native submission succeeds. GPU completion is tracked separately.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace venture::vulkan {
class BinarySemaphoreOrder {
  struct Counters {
    std::uint64_t signals{}, waits{}, issued_signals{}, issued_waits{};
  };
  struct Operation { std::uint64_t payload, generation; };
  struct Change { std::uint64_t payload, signals, waits, next_signals, next_waits; };
  struct Data {
    std::uint64_t domain;
    std::vector<Operation> waits, signals, require_signals, require_waits;
    std::vector<Change> changes;
    bool committed = false, issued = false;
  };
 public:
  class Plan {
    friend class BinarySemaphoreOrder;
    explicit Plan(std::shared_ptr<Data> data) : data_(std::move(data)) {}
    std::shared_ptr<Data> data_;
  };
  BinarySemaphoreOrder() : domain_(next_domain_.fetch_add(1)) {
    if (!domain_) throw std::overflow_error("binary submission domain exhausted");
  }
  BinarySemaphoreOrder(const BinarySemaphoreOrder&) = delete;
  // IDs identify payload lifetimes. Temporary imports require their own payload
  // ID, with restoration selecting the original payload after consumption.
  void add(std::uint64_t payload) {
    if (!payload || !states_.emplace(payload, Counters{}).second)
      throw std::invalid_argument("invalid or duplicate binary payload");
  }
  void remove(std::uint64_t payload) { states_.erase(payload); }
  // Caller rolls unissued plans back in reverse admission order.
  bool rollback(const Plan& plan) noexcept {
    if (!plan.data_ || plan.data_->domain != domain_ || !plan.data_->committed || plan.data_->issued) return false;
    auto& p = *plan.data_;
    for (const auto& c : p.changes) {
      const auto it = states_.find(c.payload);
      if (it == states_.end() || it->second.signals != c.next_signals || it->second.waits != c.next_waits) return false;
    }
    for (const auto& c : p.changes) {
      auto& state = states_.find(c.payload)->second;
      state.signals = c.signals; state.waits = c.waits;
    }
    p.committed = false;
    return true;
  }

  Plan prepare(std::span<const std::uint64_t> waits,
               std::span<const std::uint64_t> signals) const {
    auto data = std::make_shared<Data>();
    data->domain = domain_;
    auto change = [&](std::uint64_t payload) -> Change& {
      for (auto& c : data->changes) if (c.payload == payload) return c;
      const auto& state = states_.at(payload);
      data->changes.push_back({payload, state.signals, state.waits, state.signals, state.waits});
      return data->changes.back();
    };
    for (auto payload : waits) {
      if (std::ranges::any_of(data->waits, [=](auto op) { return op.payload == payload; }))
        throw std::invalid_argument("duplicate binary wait");
      auto& c = change(payload);
      const auto generation = c.next_waits + 1;
      if (!generation || generation > c.next_signals)
        throw std::invalid_argument("binary wait has no submitted signal");
      c.next_waits = generation;
      data->waits.push_back({payload, generation});
      data->require_signals.push_back({payload, generation});
    }
    for (auto payload : signals) {
      if (std::ranges::any_of(data->signals, [=](auto op) { return op.payload == payload; }))
        throw std::invalid_argument("duplicate binary signal");
      auto& c = change(payload);
      const auto generation = c.next_signals + 1;
      if (!generation || generation - 1 > c.next_waits)
        throw std::invalid_argument("binary signal reused without a submitted wait");
      c.next_signals = generation;
      data->signals.push_back({payload, generation});
      const auto same_batch_wait = std::ranges::any_of(data->waits, [=](auto op) {
        return op.payload == payload && op.generation == generation - 1;
      });
      if (generation > 1 && !same_batch_wait)
        data->require_waits.push_back({payload, generation - 1});
    }
    return Plan(std::move(data));
  }

  // prepare allocates first; commit changes no counters unless every expected
  // version still matches. Caller serializes this with logical-queue enqueue.
  bool commit(const Plan& plan) {
    auto& p = checked(plan);
    if (p.committed) return false;
    for (const auto& c : p.changes) {
      const auto& s = states_.at(c.payload);
      if (s.signals != c.signals || s.waits != c.waits) return false;
    }
    for (const auto& c : p.changes) {
      auto& s = states_.at(c.payload);
      s.signals = c.next_signals; s.waits = c.next_waits;
    }
    p.committed = true;
    return true;
  }
  bool ready(const Plan& plan) const {
    const auto& p = checked(plan);
    if (!p.committed || p.issued) return false;
    for (auto op : p.require_signals)
      if (states_.at(op.payload).issued_signals < op.generation) return false;
    for (auto op : p.require_waits)
      if (states_.at(op.payload).issued_waits < op.generation) return false;
    return true;
  }
  bool issue(const Plan& plan) {
    auto& p = checked(plan);
    if (!ready(plan)) return false;
    for (auto op : p.waits)
      if (states_.at(op.payload).issued_waits + 1 != op.generation) return false;
    for (auto op : p.signals)
      if (states_.at(op.payload).issued_signals + 1 != op.generation) return false;
    for (auto op : p.waits) states_.at(op.payload).issued_waits = op.generation;
    for (auto op : p.signals) states_.at(op.payload).issued_signals = op.generation;
    p.issued = true;
    return true;
  }
  void external_signal_issued(std::uint64_t payload) {
    auto& s = states_.at(payload);
    if (s.signals != s.waits || s.signals != s.issued_signals || s.waits != s.issued_waits ||
        s.signals == UINT64_MAX)
      throw std::invalid_argument("external signal would replace a live binary payload");
    ++s.signals; ++s.issued_signals;
  }

 private:
  Data& checked(const Plan& plan) const {
    if (!plan.data_ || plan.data_->domain != domain_)
      throw std::invalid_argument("foreign binary submission plan");
    return *plan.data_;
  }
  inline static std::atomic<std::uint64_t> next_domain_{1};
  const std::uint64_t domain_;
  std::map<std::uint64_t, Counters> states_;
};
} // namespace venture::vulkan
