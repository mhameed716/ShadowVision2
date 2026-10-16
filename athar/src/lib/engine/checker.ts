import type { SiteDef } from './sites';

/**
 * Handle normalization. Accepts a bare handle, an @mention, or a pasted
 * profile URL, and returns the bare handle.
 */
export function normalizeHandle(input: string): string {
  let value = input.trim();
  if (!value) return '';
  // Pasted profile URL -> trailing path segment.
  if (/^https?:\/\//i.test(value)) {
    try {
      const url = new URL(value);
      const segments = url.pathname.split('/').filter(Boolean);
      if (segments.length > 0) value = decodeURIComponent(segments[segments.length - 1]);
    } catch {
      /* fall through with the raw string */
    }
  }
  value = value.replace(/^@+/, '').trim();
  return value;
}

export function isValidEmail(input: string): boolean {
  return /^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/.test(input.trim());
}

export type CheckOutcome =
  | { kind: 'skipped'; reason: 'invalid_format' }
  | { kind: 'error'; reason: string }
  | { kind: 'found'; finalUrl: string; status: number; body: string }
  | { kind: 'not_found'; finalUrl: string; status: number };

export interface Profile {
  displayName?: string;
  bio?: string;
  avatar?: string;
  location?: string;
  title?: string;
  /** Personal or project website, distinct from links found in the bio. */
  website?: string;
  /** Organisation or employer where a source publishes it. */
  company?: string;
  /** Account creation date, ISO yyyy-mm-dd where the source gives one. */
  joinedAt?: string;
  /** Platform-native numeric or opaque identifier. */
  userId?: string;
  /** Verification / proof state where the source exposes it. */
  verified?: boolean;
  links: string[];
  counts: Record<string, number>;
  /** Which source produced this profile — the platform or the API provider. */
  source?: string;
  /** Raw provider payload, retained for evidence and export. */
  raw?: Record<string, unknown>;
}

const UA =
  'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36';

const MAX_BODY_BYTES = 512 * 1024;

function substitute(value: unknown, username: string): unknown {
  if (typeof value === 'string') return value.replaceAll('{}', username);
  if (Array.isArray(value)) return value.map((v) => substitute(v, username));
  if (value && typeof value === 'object') {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(value as Record<string, unknown>)) {
      out[k] = substitute(v, username);
    }
    return out;
  }
  return value;
}

export function requestTarget(site: SiteDef, username: string): string {
  const template = site.urlProbe || site.url;
  return template.replaceAll('{}', username);
}

/** Decide whether a probe response indicates the handle is claimed. */
export function evaluate(
  site: SiteDef,
  status: number,
  finalUrl: string,
  body: string
): boolean {
  switch (site.errorType) {
    case 'status_code': {
      const codes = site.errorCode ?? [404];
      return !codes.includes(status) && status < 400;
    }
    case 'message': {
      const messages = site.errorMsg ?? [];
      if (messages.length === 0) return status < 400;
      return messages.every((needle) => !body.includes(needle));
    }
    case 'response_url': {
      const errorUrl = site.errorUrl ?? site.urlMain;
      return !finalUrl.startsWith(errorUrl.replace(/\/$/, '')) || finalUrl.length > errorUrl.length + 1;
    }
    default:
      return false;
  }
}

export interface FetchLike {
  (url: string, init?: RequestInit): Promise<Response>;
}

export interface CheckOptions {
  timeoutMs?: number;
  fetchImpl?: FetchLike;
}

export async function checkSite(
  site: SiteDef,
  username: string,
  opts: CheckOptions = {}
): Promise<CheckOutcome> {
  if (site.regexCheck) {
    try {
      if (!new RegExp(site.regexCheck).test(username)) {
        return { kind: 'skipped', reason: 'invalid_format' };
      }
    } catch {
      /* a malformed pattern must not disable the site */
    }
  }

  const doFetch = opts.fetchImpl ?? fetch;
  const timeoutMs = opts.timeoutMs ?? 12000;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const target = requestTarget(site, username);
    const method = site.request_method === 'POST' ? 'POST' : 'GET';
    const headers: Record<string, string> = {
      'User-Agent': UA,
      Accept: 'text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.8',
      'Accept-Language': 'en-US,en;q=0.8'
    };
    let body: string | undefined;
    if (method === 'POST' && site.request_payload !== undefined) {
      headers['Content-Type'] = 'application/json';
      body = JSON.stringify(substitute(site.request_payload, username));
    }
    for (const [k, v] of Object.entries(site.headers ?? {})) headers[k] = v;

    const response = await doFetch(target, {
      method,
      headers,
      body,
      signal: controller.signal,
      redirect: 'follow'
    });

    const text = (await response.text()).slice(0, MAX_BODY_BYTES);
    const finalUrl = response.url || target;
    const claimed = evaluate(site, response.status, finalUrl, text);
    return claimed
      ? { kind: 'found', finalUrl, status: response.status, body: text }
      : { kind: 'not_found', finalUrl, status: response.status };
  } catch (error) {
    const reason = error instanceof Error ? error.name || error.message : 'unknown';
    return { kind: 'error', reason };
  } finally {
    clearTimeout(timer);
  }
}

const META_TAGS: Array<[keyof Profile | 'title', RegExp]> = [
  ['displayName', /<meta[^>]+property=["']og:title["'][^>]+content=["']([^"']*)/i],
  ['bio', /<meta[^>]+(?:property=["']og:description["']|name=["']description["'])[^>]+content=["']([^"']*)/i],
  ['avatar', /<meta[^>]+property=["']og:image["'][^>]+content=["']([^"']*)/i],
  ['title', /<title[^>]*>([^<]*)<\/title>/i]
];

const LINK_IN_BIO = /https?:\/\/[^\s"'<>)\]]+/g;
const COUNT_PATTERNS: Array<[string, RegExp]> = [
  ['followers', /([\d.,]+[KMB]?)\s*(?:followers|متابع)/i],
  ['following', /([\d.,]+[KMB]?)\s*(?:following|يتابع)/i],
  ['posts', /([\d.,]+[KMB]?)\s*(?:posts|منشور)/i],
  ['subscribers', /([\d.,]+[KMB]?)\s*(?:subscribers|مشترك)/i]
];

function decodeEntities(value: string): string {
  return value
    .replace(/&amp;/g, '&')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#x27;|&#39;/g, "'")
    .replace(/&nbsp;/g, ' ')
    .trim();
}

function toNumber(value: string): number {
  const cleaned = value.replace(/,/g, '');
  const match = /^([\d.]+)([KMB])?$/i.exec(cleaned);
  if (!match) return Number.NaN;
  const base = Number.parseFloat(match[1]);
  const mult = { k: 1e3, m: 1e6, b: 1e9 }[(match[2] || '').toLowerCase()] ?? 1;
  return Math.round(base * mult);
}

/** Pull the publicly advertised profile fields out of a hit's HTML. */
/** Profile fields JSON-LD can fill with a plain string. */
type ProfileStringField =
  | 'displayName'
  | 'bio'
  | 'avatar'
  | 'location'
  | 'company'
  | 'website'
  | 'joinedAt'
  | 'userId';

/** Fields JSON-LD Person/ProfilePage objects commonly carry. */
const JSONLD_TEXT_FIELDS: Array<[ProfileStringField, string[]]> = [
  ['displayName', ['name', 'alternateName']],
  ['bio', ['description']],
  ['avatar', ['image']],
  ['location', ['homeLocation', 'address', 'workLocation']],
  ['company', ['worksFor', 'affiliation', 'organization']],
  ['website', ['url', 'sameAs']]
];

/** Pull a scalar or a nested name/url out of a JSON-LD value. */
function jsonldScalar(value: unknown): string | undefined {
  if (typeof value === 'string' && value.trim()) return value.trim();
  if (Array.isArray(value) && value.length > 0) return jsonldScalar(value[0]);
  if (value && typeof value === 'object') {
    const obj = value as Record<string, unknown>;
    for (const key of ['name', 'url', 'value', '@id', 'text']) {
      const nested = jsonldScalar(obj[key]);
      if (nested) return nested;
    }
  }
  return undefined;
}

/**
 * Parse embedded JSON-LD. Many platforms publish a schema.org Person or
 * ProfilePage block that carries identifiers, join dates, audience counts and
 * cross-platform links that never appear in the og: tags.
 */
export function extractJsonLd(html: string): Partial<Profile> {
  const out: Partial<Profile> = {};
  const blocks = html.matchAll(
    /<script[^>]+type=["']application\/ld\+json["'][^>]*>([\s\S]*?)<\/script>/gi
  );
  for (const block of blocks) {
    let data: unknown;
    try {
      data = JSON.parse(block[1].trim());
    } catch {
      continue; // a malformed block must not discard the rest of the page
    }
    const nodes: Record<string, unknown>[] = [];
    const walk = (node: unknown) => {
      if (Array.isArray(node)) {
        for (const item of node) walk(item);
      } else if (node && typeof node === 'object') {
        const obj = node as Record<string, unknown>;
        const type = String(obj['@type'] ?? '');
        if (/person|profilepage|organization|user/i.test(type)) nodes.push(obj);
        if (obj['@graph']) walk(obj['@graph']);
      }
    };
    walk(data);

    for (const node of nodes) {
      for (const [field, keys] of JSONLD_TEXT_FIELDS) {
        if (out[field]) continue;
        for (const key of keys) {
          if (node[key] === undefined) continue;
          const value = jsonldScalar(node[key]);
          if (!value) continue;
          // `sameAs`/`url` are link lists, not a display website.
          if (field === 'website' && key === 'sameAs') {
            out.links = [...(out.links ?? []), value];
          } else {
            out[field] = value;
          }
          break;
        }
      }
      if (!out.joinedAt) {
        const created = jsonldScalar(node['dateCreated'] ?? node['startDate']);
        if (created) out.joinedAt = created.slice(0, 10);
      }
      if (!out.userId) {
        const id = jsonldScalar(node['identifier'] ?? node['@id']);
        if (id && !/^https?:/.test(id)) out.userId = id;
      }
      const counts: Record<string, number> = {};
      const interactions = node['interactionStatistic'];
      if (Array.isArray(interactions)) {
        for (const stat of interactions) {
          const name = jsonldScalar(stat?.interactionType ?? stat?.name)?.toLowerCase() ?? '';
          const raw = stat?.userInteractionCount;
          const value = typeof raw === 'number' ? raw : toNumber(String(raw ?? ''));
          if (name && Number.isFinite(value)) {
            if (/follow/i.test(name)) counts.followers = value;
            else if (/like/i.test(name)) counts.likes = value;
            else if (/comment/i.test(name)) counts.comments = value;
            else counts[name] = value;
          }
        }
      }
      for (const key of ['followersCount', 'followsCount', 'postsCount']) {
        const value = node[key];
        if (typeof value === 'number' && Number.isFinite(value)) {
          counts[key.replace('Count', '').toLowerCase()] = value;
        }
      }
      if (Object.keys(counts).length > 0) {
        out.counts = { ...(out.counts ?? {}), ...counts };
      }
    }
  }
  return out;
}

export function extractProfile(html: string): Profile {
  const profile: Profile = { links: [], counts: {} };
  for (const [field, pattern] of META_TAGS) {
    const match = pattern.exec(html);
    if (!match) continue;
    const value = decodeEntities(match[1]);
    if (!value) continue;
    if (field === 'title') {
      if (!profile.displayName) profile.displayName = value;
      else profile.title = value;
    } else if (field === 'displayName') {
      profile.displayName = value;
    } else if (field === 'bio') {
      profile.bio = value;
    } else if (field === 'avatar') {
      profile.avatar = value;
    }
  }
  if (profile.bio) {
    for (const link of profile.bio.match(LINK_IN_BIO) ?? []) {
      if (profile.links.length < 12) profile.links.push(link);
    }
  }
  for (const [key, pattern] of COUNT_PATTERNS) {
    const match = pattern.exec(profile.bio ?? '');
    if (!match) continue;
    const value = toNumber(match[1]);
    if (Number.isFinite(value)) profile.counts[key] = value;
  }

  // Structured data fills whatever the og: tags did not carry. og: wins for
  // displayName because it is the platform's own headline.
  const jsonld = extractJsonLd(html);
  profile.displayName = profile.displayName ?? jsonld.displayName;
  profile.bio = profile.bio ?? jsonld.bio;
  profile.avatar = profile.avatar ?? jsonld.avatar;
  profile.location = profile.location ?? jsonld.location;
  profile.company = jsonld.company;
  profile.website = jsonld.website;
  profile.joinedAt = jsonld.joinedAt;
  profile.userId = jsonld.userId;
  for (const link of jsonld.links ?? []) {
    if (profile.links.length < 12 && !profile.links.includes(link)) profile.links.push(link);
  }
  profile.counts = { ...profile.counts, ...jsonld.counts };

  return profile;
}

/**
 * Negative-page titles. A source that renders a soft 404 still answers 200, so
 * the title is the reliable signal. Matched as a phrase rather than as the whole
 * string, because sites wrap it ("404 Not Found", "Page not found | Example").
 */
const NEGATIVE_TITLE = [
  /\b404\b/,
  /\bnot\s+found\b/,
  /\bno\s+such\s+user\b/,
  /\bdoesn'?t\s+exist\b/,
  /\bdoes\s+not\s+exist\b/,
  /\bprofile\s+unavailable\b/,
  /\baccount\s+(?:not\s+found|doesn'?t\s+exist)\b/,
  /\buser\s+(?:not\s+found|doesn'?t\s+exist)\b/,
  /\bpage\s+removed\b/,
  /\bсодержимое\s+недоступно\b/
];

/**
 * Anti-bot interstitials. A CDN challenge page answers 200 with its own
 * branding, so a status-code probe reads it as a live profile — PyPI's
 * "Client Challenge" page scored as a real hit until this list existed.
 * These are titles no human would choose for themselves, and they only ever
 * appear when the site refused to serve the actual profile.
 */
const CHALLENGE_TITLE = [
  /\bclient\s+challenge\b/,
  /\bjust\s+a\s+moment\b/,
  /\battention\s+required\b/,
  /\baccess\s+denied\b/,
  /\brequest\s+blocked\b/,
  /\bsecurity\s+check\b/,
  /\bchecking\s+your\s+browser\b/,
  /\bverify\s+you\s+are\s+human\b/,
  /\bare\s+you\s+a\s+(?:human|robot)\b/,
  /\benable\s+javascript\s+and\s+cookies\b/,
  /\bddos\s+protection\b/,
  /\bcaptcha\b/,
  /\bratelimited\b/,
  /\btoo\s+many\s+requests\b/,
  /\bservice\s+unavailable\b/,
  /\bforbidden\b/
];

/**
 * Obvious false positives are dropped before anything reaches the user:
 * a soft-404 page, a source echoing its own brand account, or an empty shell
 * that just reflects the searched handle back.
 *
 * Only the title fields are tested against NEGATIVE_TITLE — a real person may
 * legitimately write "404" in a bio.
 */
export function looksLikePlaceholder(site: SiteDef, profile: Profile, username: string): boolean {
  const titleText = `${profile.displayName ?? ''} ${profile.title ?? ''}`.toLowerCase();
  if (titleText.length <= 120 && NEGATIVE_TITLE.some((p) => p.test(titleText))) return true;
  // Challenge pages arrive with a 200 and no username in the title, so they
  // survive the NEGATIVE_TITLE check above.
  if (titleText.length <= 120 && CHALLENGE_TITLE.some((p) => p.test(titleText))) return true;

  const brand = site.urlMain.replace(/^https?:\/\//, '').replace(/^www\./, '').split('.')[0];
  if (brand.length > 3 && profile.displayName?.toLowerCase() === brand) return true;
  return profile.displayName?.toLowerCase() === username.toLowerCase() && !profile.bio;
}
