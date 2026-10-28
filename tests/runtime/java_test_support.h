#pragma once
#include <functional>
#include "core/thread.h"
namespace venture::test {
extern thread_local std::function<GuestCallResult(GuestAddr, const GuestCallArgs&)> java_guest_call;
GuestAddr java_thunk(GuestAddr logical_id, unsigned gp, unsigned fp = 0, unsigned stack = 0);
}
