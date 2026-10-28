#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>

namespace venture::vulkan {
// A short admission mutex, NEVER held during guest execution or bookkeeping.
// Every allocator callback's ticket spans its entire begin/call/complete path.
class CallbackAdmission {
 public:
  enum class Phase { normal, closed, terminal };
  class Ticket {
    friend class CallbackAdmission;
    CallbackAdmission* owner_;
    Phase phase_;
    Ticket(CallbackAdmission* owner, Phase phase):owner_(owner),phase_(phase){}
   public:
    Ticket(const Ticket&)=delete;
    Ticket& operator=(const Ticket&)=delete;
    Ticket(Ticket&& t) noexcept:owner_(std::exchange(t.owner_,nullptr)),phase_(t.phase_){}
    ~Ticket(){if(owner_){std::lock_guard lock(owner_->mutex_);--owner_->active_;owner_->cv_.notify_all();}}
    Phase phase() const {return phase_;}
  };
 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  Phase phase_=Phase::normal;
  std::size_t active_=0;
 public:
  Ticket enter(){
    std::lock_guard lock(mutex_);
    if(phase_==Phase::closed)return Ticket(nullptr,phase_);
    ++active_;return Ticket(this,phase_);
  }
  bool close_and_drain(std::chrono::milliseconds bound){
    std::unique_lock lock(mutex_);
    if(phase_!=Phase::normal)return false;
    phase_=Phase::closed; // Irreversible, even on timeout. No guest resume.
    return cv_.wait_for(lock,bound,[&]{return active_==0;});
  }
  bool activate_terminal(){
    std::lock_guard lock(mutex_);
    if(phase_!=Phase::closed || active_)return false;
    phase_=Phase::terminal;return true;
  }
};

} // namespace venture::vulkan
