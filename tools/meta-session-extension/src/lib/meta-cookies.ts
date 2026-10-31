// Which cookies belong to the Meta/Oculus session Venture mirrors, and how to read them. Kept to
// exactly the domains Venture's own sign-in flow already talks to (src/common/meta/auth.cpp,
// auth_navigation_allowed), so this extension never gains cookie access beyond what Venture's own
// native auth flow uses.

import type { MetaCookie } from "@/lib/native-protocol";

/** Every base domain Venture's native auth flow allows (see auth.cpp). */
export const META_COOKIE_DOMAINS = ["meta.com", "facebook.com", "fbsbx.com", "oculus.com", "instagram.com"] as const;

/** Whether a cookie belongs to one of those domains or a subdomain of one. */
export function isMetaCookie(cookie: Browser.cookies.Cookie): boolean {
  // Chrome reports a domain-scoped cookie with a leading dot (".meta.com"); strip it before
  // comparing, so "meta.com" itself and "auth.meta.com" both match, but "notmeta.com" never does.
  const domain = cookie.domain.replace(/^\./, "");
  return META_COOKIE_DOMAINS.some(base => domain === base || domain.endsWith(`.${base}`));
}

/** Every current cookie across the Meta/Oculus domains, shaped for the native message. */
export async function collectMetaCookies(): Promise<MetaCookie[]> {
  const perDomain = await Promise.all(META_COOKIE_DOMAINS.map(domain => browser.cookies.getAll({ domain })));
  const seen = new Set<string>();
  const cookies: MetaCookie[] = [];

  for (const cookie of perDomain.flat()) {
    // getAll({domain}) also returns sibling subdomains, so the same cookie can come back once per
    // domain queried; de-dupe on its natural key.
    const key = `${cookie.domain}\u0000${cookie.path}\u0000${cookie.name}`;
    if (seen.has(key)) {
      continue;
    }

    seen.add(key);
    cookies.push({
      name: cookie.name,
      value: cookie.value,
      domain: cookie.domain,
      path: cookie.path,
      secure: cookie.secure,
      httpOnly: cookie.httpOnly,
      sameSite: cookie.sameSite,
      expirationDate: cookie.expirationDate ?? null
    });
  }

  return cookies;
}
