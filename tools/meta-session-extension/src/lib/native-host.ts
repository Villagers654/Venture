// One-shot pushes to the app's native messaging host. No WebSocket, no local server, no port or
// token: each push is its own connectNative() -> postMessage() -> disconnect(), gated purely by
// the OS-level native host manifest's allowed_origins (this extension's pinned id).

import { collectMetaCookies } from "@/lib/meta-cookies";
import { NATIVE_HOST, NativeMessageType, type CookiesMessage } from "@/lib/native-protocol";

// A sign-in or token rotation changes several cookies at once; push them as one message instead
// of one per cookie.
const PUSH_DEBOUNCE_MS = 1500;

let pendingPush: ReturnType<typeof setTimeout> | undefined;

/** Pushes the current cookie set once a burst of cookie changes has settled. */
export function pushCookiesSoon(): void {
  clearTimeout(pendingPush);
  pendingPush = setTimeout(() => void pushCookies(), PUSH_DEBOUNCE_MS);
}

/**
 * Opens one native-messaging connection, sends the current Meta/Oculus cookies, and closes it.
 * Safe to call even when the app (and its native host) isn't running: connectNative() either
 * throws synchronously or the port disconnects almost immediately, and either way this just
 * returns - the next alarm or cookie change tries again.
 */
export async function pushCookies(): Promise<void> {
  const cookies = await collectMetaCookies();

  let port: Browser.runtime.Port;
  try {
    port = browser.runtime.connectNative(NATIVE_HOST);
  } catch {
    return;
  }

  // A missing or crashed host surfaces as a disconnect rather than an exception from
  // postMessage(); reading lastError here just marks it handled so the browser doesn't log it.
  port.onDisconnect.addListener(() => void browser.runtime.lastError);

  const message: CookiesMessage = { type: NativeMessageType.Cookies, cookies };
  try {
    port.postMessage(message);
  } finally {
    port.disconnect();
  }
}
