// The one message this extension ever sends, and to the one native messaging host it ever talks
// to. There is no reply, no handshake and no persistent connection: each push opens a fresh
// connectNative() port, sends one "Cookies" message, and disconnects. The host-manifest's
// allowed_origins (pinned to this extension's id - see wxt.config.ts) is what gates who can reach
// it; nothing here needs its own port or token on top of that.

/** The name the browser (and the OS-level native host manifest) knows the host by. */
export const NATIVE_HOST = "com.venture.meta_session";

export enum NativeMessageType {
  /** extension -> host: the current Meta/Oculus cookie set. */
  Cookies = "Cookies"
}

/** One cookie exactly as chrome.cookies reports it. */
export interface MetaCookie {
  name: string;
  value: string;
  domain: string;
  path: string;
  secure: boolean;
  httpOnly: boolean;
  sameSite: string;
  expirationDate: number | null;
}

/** The single envelope shape ever written to the native host's stdin. */
export interface CookiesMessage {
  type: NativeMessageType.Cookies;
  cookies: MetaCookie[];
}
