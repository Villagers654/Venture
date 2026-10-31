import { defineConfig } from "wxt";

// WXT loads its config from the default export.
export default defineConfig({
  srcDir: "src",
  webExt: {
    disabled: true
  },
  manifest: {
    name: "Venture - Meta session",
    description:
      "Hands your own Meta/Oculus browser session to the Venture launcher over native messaging, so Venture can sign in as you without storing a separate password.",
    // Pins the extension ID to cdaemjfgmclhmheafddnmmgdgjlkjhnh, generated for this tool with
    // scripts/gen-id.mjs from keys/chrome.pem (git-ignored; see README). Load-unpacked sideloads
    // use this "key" field to get the same stable ID every reload, which must be added to the
    // native host manifest's allowed_origins as "chrome-extension://<id>/" - see README.
    // TODO(venture): if this key is ever rotated, regenerate both the key and the ID together
    // (scripts/gen-id.mjs) and update every native host manifest's allowed_origins to match.
    key: "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAxlmp2Gz9rU2edPc4u0h4COzVPEsPGCWzl4I/sr7MdE3IXsmfO3SanfyUuVyrW5MSklnO2XgDZT+IORLapr5v/j5mNl7umR/hjUFOHP6QZHYdEdlw7WLfdjm5JOVibz1H+0puabydBKJNqZJY5DIVzT5xDb1C32XNxDIaVd61nn7Q1CxpyDYQn30qTmnHxFz6QFt5Yos0/Yd/F5vINi5ggUyzOBAU8QP4GKAN9PLQSpImUY1U1ZP2uKtEP0q7rT813HxrjcrmfDTGcDznl096/tAwmvyUqbIL6V+jdQJPskEVtP3lsoBSYUjjrEoz1jcxu/X0XLqzEWFXF9/pk3J1EQIDAQAB",
    // nativeMessaging only: no content script, no popup, no WebSocket, no local server. The
    // background worker pushes cookies to the app's native host (com.venture.meta_session) and
    // nothing else talks to this extension.
    permissions: ["cookies", "nativeMessaging", "alarms"],
    // The Meta/Oculus auth domains Venture's own sign-in flow already talks to
    // (see src/common/meta/auth.cpp: auth_navigation_allowed). Both schemes: a domain's
    // non-Secure cookies need an http:// grant too, or cookies.getAll silently drops them.
    host_permissions: [
      "*://*.meta.com/*",
      "*://*.facebook.com/*",
      "*://*.fbsbx.com/*",
      "*://*.oculus.com/*",
      "*://*.instagram.com/*"
    ]
  }
});
