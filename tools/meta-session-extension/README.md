# Venture - Meta session

Hands the Venture launcher your own browser's Meta/Oculus session, so Venture can use your account's sign-in without asking you to sign in separately inside the app.

## What it is

A background-only Chrome/Edge extension (no content script, no popup) that reads your own cookies for the Meta/Oculus auth domains Venture's own sign-in flow already talks to (`meta.com`, `facebook.com`, `fbsbx.com`, `oculus.com`, `instagram.com` - see `src/common/meta/auth.cpp`, `auth_navigation_allowed`) and delivers them to the Venture app.

**Delivery is native messaging only.** There is no WebSocket, no loopback socket and no local server anywhere in this extension. Each push is a single `runtime.connectNative("com.venture.meta_session")`, one `"Cookies"` message, then disconnect - a fresh, short-lived connection every time, never a persistent link. Chrome's own native-messaging gate (the host manifest's `allowed_origins`, pinned to this extension's id) is what decides whether this extension may talk to the host at all, so there's no port or token for this extension to negotiate on top of that.

This only ever touches cookies already sitting in your own browser profile, for your own account, on your own machine. Nothing is sent anywhere except to the local native host process.

## Install

### The extension (sideload)

1. Build it (see below), or use an existing `.output/chrome-mv3` build.
2. Open `chrome://extensions` (`edge://extensions` in Edge) and turn on **Developer mode** (top right).
3. Click **Load unpacked** and pick `.output/chrome-mv3`.

The manifest pins a fixed extension id (`key` in `wxt.config.ts`), so **Load unpacked** gets the same id every time you reload it, instead of a new one each load.

### The native host

This extension only *sends* cookies; a native messaging host named `com.venture.meta_session` (installed by the Venture app, under `src/`, not by this tool) is what receives them. Chrome only lets a native host talk to an extension listed in that host's own manifest, under `allowed_origins`:

```json
{
  "name": "com.venture.meta_session",
  "description": "Venture - receives the user's Meta/Oculus browser session",
  "path": "<path to the Venture native-host executable>",
  "type": "stdio",
  "allowed_origins": [
    "chrome-extension://cdaemjfgmclhmheafddnmmgdgjlkjhnh/"
  ]
}
```

`cdaemjfgmclhmheafddnmmgdgjlkjhnh` is this extension's id, generated for this tool and pinned in `wxt.config.ts`'s `key`. **If that key ever changes, the id changes too** - regenerate both together (see `scripts/gen-id.mjs` below) and update every native host manifest's `allowed_origins` to match, or Chrome will silently refuse to launch the host for this extension.

### Updates

There's no update channel here: rebuild (`wxt build`) and reload the unpacked extension (the refresh icon on its card in `chrome://extensions`, or remove and **Load unpacked** again) whenever the source changes. Because the id is pinned by `key`, a reload never needs a new `allowed_origins` entry.

## How it works

- **Your browser's Meta/Oculus session.** The background worker pushes the session's cookies to the Venture app's native host whenever they change, and on a keepalive timer so a push missed while the app wasn't running (or the worker was asleep) catches up quickly. The browser - not Venture - stays the only thing that ever signs in or rotates these cookies.
- **One push, one connection.** Each push is `runtime.connectNative` -> `postMessage({ type: "Cookies", cookies })` -> `disconnect()`. Nothing is kept open between pushes, and nothing but the OS process launched by that connection ever receives the message.
- **No handshake needed.** Unlike a socket-based bridge, there's no port to discover and no token to exchange: the native-messaging host manifest's `allowed_origins` already restricts the host to this one pinned extension id, so the message itself needs nothing extra to prove where it came from.

## Build from source

Needs Node and a package manager (`npm`/`pnpm`).

```powershell
npm install
npm run build    # wxt build -> .output/chrome-mv3
```

Then **Load unpacked** that folder, as above.

| Path | What |
| --- | --- |
| `src/entrypoints/background.ts` | the whole extension: wires alarms, `cookies.onChanged`, and the native-messaging push |
| `src/lib/meta-cookies.ts` | which cookies belong to the Meta/Oculus domains, and reading them |
| `src/lib/native-host.ts` | the one-shot `connectNative` -> send -> disconnect push, debounced |
| `src/lib/native-protocol.ts` | the single `"Cookies"` message shape sent to the host |
| `keys/chrome.pem` | this extension's private signing key (git-ignored; only needed if you ever sign a `.crx` - **not** needed for the Load-unpacked workflow above) |
| `scripts/gen-id.mjs` | derives the extension id from a public key's DER bytes, the same way Chrome does |

### Regenerating the key/id pair

```powershell
openssl genrsa -out keys/chrome.pem 2048
openssl rsa -in keys/chrome.pem -pubout -outform DER -out keys/pub.der
# keys/public_key.b64 -> paste into wxt.config.ts's "key"
openssl base64 -in keys/pub.der -A
node scripts/gen-id.mjs keys/pub.der   # prints the new extension id
```

Update both `wxt.config.ts`'s `key` and this README's example `allowed_origins` id together, and update every installed native host manifest to match.

## License

Apache License 2.0, matching the rest of this repository.
