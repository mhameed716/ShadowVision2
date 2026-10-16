/**
 * Offline fixture transport.
 *
 * The sandbox running this build has outbound network restricted to package
 * registries, so live probes to third-party platforms cannot complete here.
 * Setting ATHAR_OFFLINE=1 routes every probe through this fake transport so the
 * whole pipeline — corpus iteration, evaluation, extraction, ranking, budget,
 * persistence and export — is genuinely exercised end to end.
 *
 * It is a test double, not a data source. Results produced under it are labelled
 * as synthetic in the UI.
 */
import type { FetchLike } from './checker';

interface FixtureProfile {
  displayName: string;
  bio?: string;
  avatar?: string;
  followers?: number;
  following?: number;
  posts?: number;
  subscribers?: number;
  links?: string[];
}

const FIXTURES: Record<string, FixtureProfile> = {
  github: {
    displayName: 'Ada L.',
    bio: 'Systems engineer. Notes at https://ada.example/ · contact: ada@example.org',
    followers: 12400,
    following: 310,
    links: ['https://ada.example/']
  },
  twitter: {
    displayName: 'Ada L.',
    bio: 'Building things. Previously @northwindnow',
    followers: 88200,
    following: 402
  },
  instagram: {
    displayName: 'Ada L.',
    bio: 'Photography and long walks',
    followers: 5100,
    posts: 412
  },
  reddit: { displayName: 'ada_l', bio: 'karma and opinions', followers: 940 },
  medium: { displayName: 'Ada L.', bio: 'Essays on systems design', followers: 2200 },
  devto: { displayName: 'Ada L.', bio: 'Writing about compilers', followers: 1500 },
  youtube: { displayName: 'Ada L.', bio: 'Occasional talks', subscribers: 33000 },
  linkedin: { displayName: 'Ada Lovelace', bio: 'Staff Engineer at Northwind', followers: 1800 },
  keybase: { displayName: 'ada' },
  'hacker-news': { displayName: 'ada' },
  gitlab: { displayName: 'Ada L.', bio: 'Mirrors', followers: 88 },
  pinterest: { displayName: 'Ada L.', followers: 210 },
  spotify: { displayName: 'Ada L.' },
  twitch: { displayName: 'ada_l', followers: 44 },
  steam: { displayName: 'ada' }
};

/** Sites that must come back negative so the not-found path is exercised too. */
const NEGATIVE = new Set(['facebook', 'tiktok', 'telegram', 'discord', 'snapchat']);

function siteNameFromUrl(url: string): string | null {
  const match = /https?:\/\/([^/]+)/.exec(url);
  if (!match) return null;
  return match[1].replace(/^www\./, '').split('.')[0].toLowerCase();
}

function renderProfile(name: string, profile: FixtureProfile): string {
  const counts: string[] = [];
  if (profile.followers) counts.push(`${profile.followers.toLocaleString('en-US')} followers`);
  if (profile.following) counts.push(`${profile.following.toLocaleString('en-US')} following`);
  if (profile.posts) counts.push(`${profile.posts.toLocaleString('en-US')} posts`);
  if (profile.subscribers) {
    counts.push(`${profile.subscribers.toLocaleString('en-US')} subscribers`);
  }
  const description = [profile.bio, counts.join(' · ')].filter(Boolean).join(' — ');
  return `<!doctype html><html><head>
<meta property="og:title" content="${profile.displayName} (@${name})" />
<meta property="og:description" content="${description}" />
<meta property="og:image" content="https://cdn.example/avatar-${name}.jpg" />
<title>${profile.displayName} | ${name}</title>
</head><body><h1>${profile.displayName}</h1></body></html>`;
}

export const offlineFetch: FetchLike = async (url: string, init?: RequestInit) => {
  const signal = init?.signal;
  if (signal?.aborted) throw new DOMException('aborted', 'AbortError');

  const host = siteNameFromUrl(url) ?? '';
  const handle = /\/([A-Za-z0-9_.-]+)\/?$/.exec(new URL(url).pathname)?.[1] ?? '';

  // Deterministic: only a fixed set of platforms return a profile.
  const profile = FIXTURES[host];
  const claimed = Boolean(profile) && !NEGATIVE.has(host);

  if (!claimed) {
    const body = '<html><head><title>404 Not Found</title></head><body>Not found</body></html>';
    return new Response(body, { status: 404, headers: { 'content-type': 'text/html' } });
  }

  return new Response(renderProfile(handle || host, profile as FixtureProfile), {
    status: 200,
    headers: { 'content-type': 'text/html' },
    // Response.url is read-only; emulate it for response_url sites.
    ...({ url } as Partial<Response>)
  });
};

export function isOffline(): boolean {
  return process.env.ATHAR_OFFLINE === '1';
}

export function offlineTransport(): FetchLike | undefined {
  return isOffline() ? offlineFetch : undefined;
}
