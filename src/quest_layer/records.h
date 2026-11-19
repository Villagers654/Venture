#pragma once
// The Quest layer's records for the debug report (docs/design/native-headsets.md,
// section 9): what it changed for the game (instance version, extensions it
// answers, each shim's first use) and every error result a game gets from an
// OpenXR call, the first time for each function and result and then at 10, 100,
// 1000... occurrences, with a count summary when the instance ends.
//
// The file is <the game's OBB folder>/venture/questlayer.log on Android, found
// through the XrInstanceCreateInfoAndroidKHR the game passes, beside the other
// Venture libraries' logs; VENTURE_QUEST_LAYER_LOG names the file instead (tests
// and other hosts). The run before is kept as questlayer.prev.log. On Android
// each line also goes to logcat.
#include <openxr/openxr.h>

#include <atomic>
#include <string>
#include <string_view>

namespace venture::quest_layer::records {
// Opens the file for this process, from the game's create info. Later calls
// do nothing once a file is open.
void start(const XrInstanceCreateInfo& info);
void write(std::string_view line);

// "XR_ERROR_..." for a result, or its number.
std::string result_name(XrResult result);

// The function the game gets for `name`: `target` wrapped so that its error
// results are recorded, or `target` itself for functions not wrapped.
PFN_xrVoidFunction checked(const char* name, PFN_xrVoidFunction target);
// The counts of the errors recorded so far. With `last`, no instance is left
// and the wrapped targets are forgotten.
void summary(bool last);
}  // namespace venture::quest_layer::records

// Writes `text` (a std::string expression, evaluated only then) to the records
// the first time this line runs.
#define VENTURE_QUEST_RECORD_ONCE(text)                                \
  do {                                                                 \
    static std::atomic_flag venture_recorded_;                         \
    if (!venture_recorded_.test_and_set(std::memory_order_relaxed))    \
      ::venture::quest_layer::records::write(text);                    \
  } while (0)
