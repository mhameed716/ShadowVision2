import type { Profile } from './checker';
import { normalizeHandle } from './checker';

/**
 * Deep search. Bios and link pages frequently hand you the next identifier, so
 * handles found there are extracted, de-duplicated and re-checked at the next
 * depth. Depths 1..4 map to the published 3/6/12/24 unit costs.
 */

const HANDLE_TOKEN = /@([A-Za-z0-9_][A-Za-z0-9_.-]{2,38})/g;

/** Platforms whose profile URLs are strong evidence of a linked handle. */
const PROFILE_URL_PATTERNS: Array<{ pattern: RegExp; group: number }> = [
  { pattern: /https?:\/\/(?:www\.)?twitter\.com\/([A-Za-z0-9_]{1,15})/gi, group: 1 },
  { pattern: /https?:\/\/(?:www\.)?x\.com\/([A-Za-z0-9_]{1,15})/gi, group: 1 },
  { pattern: /https?:\/\/(?:www\.)?instagram\.com\/([A-Za-z0-9_.]{1,30})/gi, group: 1 },
  { pattern: /https?:\/\/(?:www\.)?t\.me\/([A-Za-z0-9_]{4,32})/gi, group: 1 },
  { pattern: /https?:\/\/(?:www\.)?github\.com\/([A-Za-z0-9-]{1,39})/gi, group: 1 },
  { pattern: /https?:\/\/(?:www\.)?tiktok\.com\/@([A-Za-z0-9_.]{1,30})/gi, group: 1 },
  { pattern: /https?:\/\/(?:www\.)?youtube\.com\/@([A-Za-z0-9_.-]{1,30})/gi, group: 1 },
  { pattern: /https?:\/\/(?:www\.)?twitch\.tv\/([A-Za-z0-9_]{3,25})/gi, group: 1 },
  { pattern: /https?:\/\/(?:www\.)?linkedin\.com\/in\/([A-Za-z0-9-_%]{3,100})/gi, group: 1 }
];

/** Handles that are never a real next lead. */
const RESERVED = new Set([
  'home', 'about', 'help', 'support', 'login', 'signup', 'settings', 'search',
  'explore', 'profile', 'user', 'users', 'account', 'terms', 'privacy', 'blog',
  'contact', 'status', 'share', 'link', 'links', 'bio', 'me', 'you', 'web',
  'app', 'official', 'verified', 'example', 'test', 'email', 'mail'
]);

/**
 * A token that is really a domain, not a handle. The @mention pattern happily
 * matches the domain half of an email address (`ada@example.org` -> `example.org`),
 * and bios often mention bare hosts, so both must be rejected.
 */
const DOMAIN_LIKE =
  /\.(com|org|net|io|dev|co|me|info|xyz|app|gov|edu|biz|tech|online|site|store|blog|uk|de|fr|ru|jp|es|it|nl|se|no|br|in|au|ca|us|tv|gg|sh|to|ly|be|ai)(\b|$)/i;

const EMAIL_IN_BIO = /[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g;

export interface ExtractedLeads {
  handles: string[];
  emails: string[];
}

function addHandle(bucket: Set<string>, candidate: string): void {
  const handle = normalizeHandle(candidate);
  if (handle.length < 3 || handle.length > 40) return;
  if (!/^[A-Za-z0-9][A-Za-z0-9_.-]*$/.test(handle)) return;
  if (RESERVED.has(handle.toLowerCase())) return;
  // A bare filename or asset path is not a handle.
  if (/\.(png|jpe?g|gif|webp|svg|css|js|json|pdf|mp4)$/i.test(handle)) return;
  // Neither is the domain half of an email address or a host mentioned in a bio.
  if (DOMAIN_LIKE.test(handle)) return;
  bucket.add(handle);
}

export function extractLeads(
  profiles: Profile[],
  seedHandle: string
): ExtractedLeads {
  const handles = new Set<string>();
  const emails = new Set<string>();

  for (const profile of profiles) {
    const raw = [profile.displayName, profile.bio, profile.title].filter(Boolean).join(' ');
    // Collect emails first, then blank them out: the @mention pattern would
    // otherwise match the domain half of every address in the bio.
    for (const match of raw.matchAll(EMAIL_IN_BIO)) {
      if (emails.size < 20) emails.add(match[0].toLowerCase());
    }
    const text = raw.replace(EMAIL_IN_BIO, ' ');
    for (const match of text.matchAll(HANDLE_TOKEN)) addHandle(handles, match[1]);
    for (const link of profile.links ?? []) {
      for (const { pattern, group } of PROFILE_URL_PATTERNS) {
        pattern.lastIndex = 0;
        for (const match of link.matchAll(pattern)) addHandle(handles, match[group]);
      }
    }
  }

  handles.delete(normalizeHandle(seedHandle));
  return { handles: [...handles].slice(0, 25), emails: [...emails] };
}

/** Handles that appeared on more than one source are stronger leads. */
export function rankLeads(
  leads: ExtractedLeads,
  occurrences: Map<string, number>
): string[] {
  return [...leads.handles].sort((a, b) => {
    const oa = occurrences.get(a.toLowerCase()) ?? 1;
    const ob = occurrences.get(b.toLowerCase()) ?? 1;
    if (ob !== oa) return ob - oa;
    return a.length - b.length;
  });
}
