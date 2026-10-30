// Venture - Meta session link.
//
// This browser stays the only owner of the user's own Meta/Oculus session. This worker reads that
// session's cookies and pushes them to the Venture launcher app purely over native messaging - one
// connectNative() -> postMessage("Cookies") -> disconnect() per push, never a WebSocket or a local
// server. There is no content script and no popup; this background worker is the whole extension.

import { isMetaCookie } from "@/lib/meta-cookies";
import { pushCookies, pushCookiesSoon } from "@/lib/native-host";

// Wakes the suspendable MV3 worker so a push missed while it slept (e.g. the app launched later,
// or the host wasn't installed yet) retries within 30 seconds, Chrome's floor for a periodic alarm
// (since Chrome 120).
const PUSH_KEEPALIVE_MINUTES = 0.5;

/** Every alarm this worker creates; onAlarm dispatches on the name. */
enum AlarmName {
  /** Periodic keepalive that re-sends the current cookies. */
  Push = "push"
}

// WXT discovers entrypoints by their default export, so this one file is exempt from the
// named-exports rule.
export default defineBackground(() => {
  // Every worker start (install, browser launch, dev reload, idle wake) pushes once right away.
  void pushCookies();
  browser.runtime.onInstalled.addListener(() => void pushCookies());
  browser.runtime.onStartup.addListener(() => void pushCookies());

  void browser.alarms.create(AlarmName.Push, { periodInMinutes: PUSH_KEEPALIVE_MINUTES });
  browser.alarms.onAlarm.addListener(alarm => {
    if (alarm.name !== AlarmName.Push) {
      return;
    }

    void pushCookies();
  });

  browser.cookies.onChanged.addListener(({ cookie }) => {
    if (!isMetaCookie(cookie)) {
      return;
    }

    pushCookiesSoon();
  });
});
