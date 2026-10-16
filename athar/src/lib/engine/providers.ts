import crypto from 'node:crypto';
import type { Profile } from './checker';

/**
 * Structured profile providers.
 *
 * Scraping HTML yields a display name and maybe a bio. Several major platforms
 * publish a real JSON endpoint that needs no authentication, which returns far
 * more: identifiers, join dates, audience counts, websites, verification state.
 *
 * Every provider here is a documented public endpoint. None requires an API key
 * and none uses an unofficial mirror, so a result is attributable to the
 * platform itself rather than to a third party that may vanish.
 */

export interface ProviderResult {
  /** The platform that actually answered. */
  source: string;
  /** Canonical profile URL on that platform. */
  url: string;
  profile: Profile;
  /** Raw fields the provider returned, for evidence and export. */
  raw: Record<string, unknown>;
}

export interface Provider {
  id: string;
  label: string;
  /** Sites in the corpus this provider enriches, matched by lowercase name. */
  matches: string[];
  /** Extra standalone sources with no corpus entry. */
  standalone?: boolean;
  /** Whether the handle shape is plausible for this platform. */
  accepts: (handle: string) => boolean;
  fetch: (handle: string, fetchImpl: FetchImpl) => Promise<ProviderResult | null>;
}

export type FetchImpl = (url: string, init?: RequestInit) => Promise<Response>;

const UA = 'Athar/0.2 (+https://github.com/mhameed716/ShadowVision2; osint research)';

async function getJson(
  url: string,
  fetchImpl: FetchImpl,
  timeoutMs = 10000
): Promise<unknown | null> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetchImpl(url, {
      headers: { 'User-Agent': UA, Accept: 'application/json' },
      signal: controller.signal
    });
    if (response.status === 404) return null;
    if (!response.ok) return null;
    return await response.json();
  } catch {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

function numbersOnly(input: Record<string, unknown>): Record<string, number> {
  const out: Record<string, number> = {};
  for (const [key, value] of Object.entries(input)) {
    if (typeof value === 'number' && Number.isFinite(value)) out[key] = value;
  }
  return out;
}

/* ----------------------------------------------------------------- GitHub */

const github: Provider = {
  id: 'github',
  label: 'GitHub',
  matches: ['github'],
  accepts: (h) => /^[a-zA-Z0-9](?:[a-zA-Z0-9]|-(?=[a-zA-Z0-9])){0,38}$/.test(h),
  async fetch(handle, fetchImpl) {
    const data = (await getJson(
      `https://api.github.com/users/${encodeURIComponent(handle)}`,
      fetchImpl
    )) as Record<string, unknown> | null;
    if (!data || data.message) return null;

    const links: string[] = [];
    if (typeof data.blog === 'string' && data.blog) {
      links.push(/^https?:\/\//.test(data.blog) ? data.blog : `https://${data.blog}`);
    }
    if (typeof data.twitter_username === 'string' && data.twitter_username) {
      links.push(`https://twitter.com/${data.twitter_username}`);
    }
    if (typeof data.html_url === 'string') links.push(data.html_url);

    return {
      source: 'GitHub',
      url: String(data.html_url ?? `https://github.com/${handle}`),
      profile: {
        displayName: typeof data.name === 'string' && data.name ? data.name : undefined,
        bio: typeof data.bio === 'string' && data.bio ? data.bio : undefined,
        avatar: typeof data.avatar_url === 'string' ? data.avatar_url : undefined,
        location: typeof data.location === 'string' && data.location ? data.location : undefined,
        website: typeof data.blog === 'string' && data.blog ? data.blog : undefined,
        company: typeof data.company === 'string' && data.company ? data.company : undefined,
        joinedAt: typeof data.created_at === 'string' ? data.created_at.slice(0, 10) : undefined,
        userId: typeof data.id === 'number' ? String(data.id) : undefined,
        verified: data.type === 'Organization',
        links,
        counts: numbersOnly({
          followers: data.followers,
          following: data.following,
          public_repos: data.public_repos,
          public_gists: data.public_gists
        })
      },
      raw: data
    };
  }
};

/* ----------------------------------------------------------------- GitLab */

const gitlab: Provider = {
  id: 'gitlab',
  label: 'GitLab',
  matches: ['gitlab'],
  accepts: (h) => /^[a-zA-Z0-9._-]{2,255}$/.test(h),
  async fetch(handle, fetchImpl) {
    const data = (await getJson(
      `https://gitlab.com/api/v4/users?username=${encodeURIComponent(handle)}`,
      fetchImpl
    )) as Array<Record<string, unknown>> | null;
    const user = Array.isArray(data) ? data[0] : null;
    if (!user) return null;
    return {
      source: 'GitLab',
      url: String(user.web_url ?? `https://gitlab.com/${handle}`),
      profile: {
        displayName: typeof user.name === 'string' ? user.name : undefined,
        bio: typeof user.bio === 'string' && user.bio ? user.bio : undefined,
        avatar: typeof user.avatar_url === 'string' ? user.avatar_url : undefined,
        location: typeof user.location === 'string' && user.location ? user.location : undefined,
        website: typeof user.public_email === 'string' ? user.public_email : undefined,
        joinedAt: typeof user.created_at === 'string' ? user.created_at.slice(0, 10) : undefined,
        userId: typeof user.id === 'number' ? String(user.id) : undefined,
        links: typeof user.web_url === 'string' ? [user.web_url] : [],
        counts: {}
      },
      raw: user
    };
  }
};

/* ----------------------------------------------------------------- Reddit */

const reddit: Provider = {
  id: 'reddit',
  label: 'Reddit',
  matches: ['reddit'],
  accepts: (h) => /^[A-Za-z0-9_-]{3,20}$/.test(h),
  async fetch(handle, fetchImpl) {
    const data = (await getJson(
      `https://www.reddit.com/user/${encodeURIComponent(handle)}/about.json`,
      fetchImpl
    )) as { kind?: string; data?: Record<string, unknown> } | null;
    const user = data?.kind === 't2' ? data.data : null;
    if (!user) return null;
    const createdUtc = typeof user.created_utc === 'number' ? user.created_utc : undefined;
    return {
      source: 'Reddit',
      url: `https://www.reddit.com/user/${handle}`,
      profile: {
        displayName: typeof user.name === 'string' ? user.name : undefined,
        avatar:
          typeof user.icon_img === 'string' && user.icon_img
            ? user.icon_img.split('?')[0]
            : typeof user.snoovatar_img === 'string' && user.snoovatar_img
              ? user.snoovatar_img
              : undefined,
        joinedAt: createdUtc ? new Date(createdUtc * 1000).toISOString().slice(0, 10) : undefined,
        userId: typeof user.id === 'string' ? user.id : undefined,
        verified: user.has_verified_email === true,
        links: [`https://www.reddit.com/user/${handle}`],
        counts: numbersOnly({
          total_karma: user.total_karma,
          link_karma: user.link_karma,
          comment_karma: user.comment_karma
        })
      },
      raw: user
    };
  }
};

/* ---------------------------------------------------------------- Bluesky */

const bluesky: Provider = {
  id: 'bluesky',
  label: 'Bluesky',
  matches: ['bluesky'],
  accepts: (h) => /^[a-zA-Z0-9._-]{2,128}$/.test(h),
  async fetch(handle, fetchImpl) {
    // Accept either a bare handle or a full DID / domain handle.
    const actor = handle.includes('.') ? handle : `${handle}.bsky.social`;
    const data = (await getJson(
      `https://public.api.bsky.app/xrpc/app.bsky.actor.getProfile?actor=${encodeURIComponent(actor)}`,
      fetchImpl
    )) as Record<string, unknown> | null;
    if (!data || data.error) return null;
    const did = typeof data.did === 'string' ? data.did : undefined;
    return {
      source: 'Bluesky',
      url: `https://bsky.app/profile/${did ?? actor}`,
      profile: {
        displayName: typeof data.displayName === 'string' ? data.displayName : undefined,
        bio: typeof data.description === 'string' && data.description ? data.description : undefined,
        avatar: typeof data.avatar === 'string' ? data.avatar : undefined,
        joinedAt: typeof data.indexedAt === 'string' ? data.indexedAt.slice(0, 10) : undefined,
        userId: did,
        links: [`https://bsky.app/profile/${did ?? actor}`],
        counts: numbersOnly({
          followers: data.followersCount,
          following: data.followsCount,
          posts: data.postsCount
        })
      },
      raw: data
    };
  }
};

/* --------------------------------------------------------------- Mastodon */

const mastodon: Provider = {
  id: 'mastodon',
  label: 'Mastodon',
  matches: ['mastodon'],
  standalone: true,
  accepts: (h) => /^[a-zA-Z0-9_]{2,128}$/.test(h),
  async fetch(handle, fetchImpl) {
    const data = (await getJson(
      `https://mastodon.social/api/v1/accounts/lookup/${encodeURIComponent(handle)}`,
      fetchImpl
    )) as Record<string, unknown> | null;
    if (!data || data.error) return null;
    const acct = typeof data.acct === 'string' ? data.acct : handle;
    const fields = Array.isArray(data.fields)
      ? (data.fields as Array<{ name?: string; value?: string }>)
      : [];
    const links = fields
      .map((f) => /href="([^"]+)"/.exec(f.value ?? '')?.[1])
      .filter((v): v is string => Boolean(v));
    return {
      source: 'Mastodon',
      url: String(data.url ?? `https://mastodon.social/@${acct}`),
      profile: {
        displayName: typeof data.display_name === 'string' ? data.display_name : undefined,
        bio:
          typeof data.note === 'string' && data.note
            ? data.note.replace(/<[^>]+>/g, '').trim()
            : undefined,
        avatar: typeof data.avatar === 'string' ? data.avatar : undefined,
        joinedAt: typeof data.created_at === 'string' ? data.created_at.slice(0, 10) : undefined,
        userId: typeof data.id === 'string' ? data.id : undefined,
        verified: fields.some((f) => /verified|proof/i.test(f.name ?? '')),
        links: links.length > 0 ? links : [String(data.url ?? '')],
        counts: numbersOnly({
          followers: data.followers_count,
          following: data.following_count,
          posts: data.statuses_count
        })
      },
      raw: data
    };
  }
};

/* ----------------------------------------------------------- Hacker News */

const hackernews: Provider = {
  id: 'hackernews',
  label: 'Hacker News',
  matches: ['hacker-news', 'hackernews'],
  accepts: (h) => /^[A-Za-z0-9_-]{2,64}$/.test(h),
  async fetch(handle, fetchImpl) {
    const data = (await getJson(
      `https://hn.algolia.com/api/v1/users/${encodeURIComponent(handle)}`,
      fetchImpl
    )) as Record<string, unknown> | null;
    if (!data || !data.username) return null;
    const createdAt =
      typeof data.created_at === 'string' ? data.created_at.slice(0, 10) : undefined;
    const about =
      typeof data.about === 'string' ? data.about.replace(/<[^>]+>/g, '').trim() : undefined;
    const links =
      typeof data.about === 'string'
        ? (data.about.match(/https?:\/\/[^\s"'<)]+/g) ?? [])
        : [];
    return {
      source: 'Hacker News',
      url: `https://news.ycombinator.com/user?id=${handle}`,
      profile: {
        displayName: String(data.username),
        bio: about || undefined,
        joinedAt: createdAt,
        links: [`https://news.ycombinator.com/user?id=${handle}`, ...links],
        counts: numbersOnly({ karma: data.karma })
      },
      raw: data
    };
  }
};

/* ----------------------------------------------------------------- Dev.to */

const devto: Provider = {
  id: 'devto',
  label: 'DEV Community',
  matches: ['dev.to', 'devto'],
  accepts: (h) => /^[a-zA-Z0-9_-]{2,60}$/.test(h),
  async fetch(handle, fetchImpl) {
    const data = (await getJson(
      `https://dev.to/api/users/by_username?url=${encodeURIComponent(handle)}`,
      fetchImpl
    )) as Record<string, unknown> | null;
    if (!data || typeof data.username !== 'string') return null;
    const links: string[] = [];
    if (typeof data.website_url === 'string' && data.website_url) links.push(data.website_url);
    links.push(`https://dev.to/${handle}`);
    return {
      source: 'DEV Community',
      url: `https://dev.to/${handle}`,
      profile: {
        displayName: typeof data.name === 'string' ? data.name : undefined,
        bio: typeof data.summary === 'string' && data.summary ? data.summary : undefined,
        avatar: typeof data.profile_image === 'string' ? data.profile_image : undefined,
        location: typeof data.location === 'string' && data.location ? data.location : undefined,
        website: typeof data.website_url === 'string' ? data.website_url : undefined,
        joinedAt: typeof data.joined_at === 'string' ? data.joined_at : undefined,
        userId: typeof data.id === 'number' ? String(data.id) : undefined,
        links,
        counts: {}
      },
      raw: data
    };
  }
};

/* -------------------------------------------------------------------- npm */

const npm: Provider = {
  id: 'npm',
  label: 'npm',
  matches: ['npm'],
  accepts: (h) => /^[a-zA-Z0-9._-]{1,214}$/.test(h),
  async fetch(handle, fetchImpl) {
    const data = (await getJson(
      `https://registry.npmjs.org/-/user/org.couchdb.user:${encodeURIComponent(handle)}`,
      fetchImpl
    )) as Record<string, unknown> | null;
    if (!data || data.error) return null;
    const links: string[] = [`https://www.npmjs.com/~${handle}`];
    const github = typeof data.github === 'string' ? data.github : undefined;
    if (github) links.push(/^https?:/.test(github) ? github : `https://github.com/${github}`);
    return {
      source: 'npm',
      url: `https://www.npmjs.com/~${handle}`,
      profile: {
        displayName: typeof data.name === 'string' ? data.name : undefined,
        avatar:
          typeof data.avatarMedium === 'string'
            ? data.avatarMedium
            : typeof data.avatar === 'string'
              ? data.avatar
              : undefined,
        website: typeof data.twitter === 'string' ? data.twitter : undefined,
        joinedAt:
          typeof data.date === 'string'
            ? data.date.slice(0, 10)
            : typeof data.created === 'string'
              ? data.created.slice(0, 10)
              : undefined,
        links,
        counts: {}
      },
      raw: data
    };
  }
};

/* ---------------------------------------------------------------- Keybase */

const keybase: Provider = {
  id: 'keybase',
  label: 'Keybase',
  matches: ['keybase'],
  accepts: (h) => /^[a-zA-Z0-9_.]{2,66}$/.test(h),
  async fetch(handle, fetchImpl) {
    const data = (await getJson(
      `https://keybase.io/_/api/1.0/user/lookup.json?usernames=${encodeURIComponent(handle)}`,
      fetchImpl
    )) as { status?: { code?: number }; them?: Array<Record<string, unknown>> } | null;
    const them = data?.them?.[0];
    if (!them || data?.status?.code !== 0) return null;
    const profile = them.profile as Record<string, unknown> | undefined;
    const pictures = them.pictures as
      | { primary?: { url?: string } }
      | undefined;
    const links: string[] = [`https://keybase.io/${handle}`];
    const proofsSummary = them.proofs_summary as { all?: unknown } | undefined;
    const proofs = Array.isArray(proofsSummary?.all)
      ? (proofsSummary!.all as Array<{ profile_url?: string }>)
      : [];
    for (const proof of proofs) if (proof.profile_url) links.push(proof.profile_url);
    return {
      source: 'Keybase',
      url: `https://keybase.io/${handle}`,
      profile: {
        displayName: typeof profile?.full_name === 'string' ? profile.full_name : undefined,
        bio: typeof profile?.bio === 'string' && profile.bio ? profile.bio : undefined,
        avatar: typeof pictures?.primary?.url === 'string' ? pictures.primary.url : undefined,
        location:
          typeof profile?.location === 'string' && profile.location ? profile.location : undefined,
        userId: typeof them.id === 'string' ? them.id : undefined,
        links,
        counts: {}
      },
      raw: them
    };
  }
};

/* ------------------------------------------------------------------ Steam */

const steam: Provider = {
  id: 'steam',
  label: 'Steam',
  matches: ['steam', 'steamcommunity'],
  standalone: true,
  accepts: (h) => /^[a-zA-Z0-9_-]{2,64}$/.test(h),
  async fetch(handle, fetchImpl) {
    // Steam exposes a keyless XML profile endpoint.
    const url = `https://steamcommunity.com/id/${encodeURIComponent(handle)}/?xml=1`;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 10000);
    let text = '';
    try {
      const response = await fetchImpl(url, {
        headers: { 'User-Agent': UA },
        signal: controller.signal
      });
      if (!response.ok) return null;
      text = await response.text();
    } catch {
      return null;
    } finally {
      clearTimeout(timer);
    }
    if (text.includes('<error>')) return null;
    const tag = (name: string): string | undefined => {
      const match = new RegExp(`<${name}>(?:<!\\[CDATA\\[)?([\\s\\S]*?)(?:\\]\\]>)?</${name}>`).exec(
        text
      );
      return match ? match[1].trim() : undefined;
    };
    const steamId = tag('steamID64');
    if (!steamId) return null;
    const profileUrl = tag('profileURL') ?? `https://steamcommunity.com/id/${handle}`;
    return {
      source: 'Steam',
      url: profileUrl,
      profile: {
        displayName: tag('steamID') || tag('realname') || handle,
        bio: tag('summary')?.replace(/<[^>]+>/g, '').trim() || undefined,
        avatar: tag('avatarFull') || undefined,
        location:
          [tag('location'), tag('state'), tag('country')].filter(Boolean).join(', ') || undefined,
        joinedAt: tag('memberSince') || undefined,
        userId: steamId,
        links: [profileUrl],
        counts: {}
      },
      raw: { steamID64: steamId, profileURL: profileUrl }
    };
  }
};

/* -------------------------------------------------------------- Wikipedia */

const wikipedia: Provider = {
  id: 'wikipedia',
  label: 'Wikipedia',
  matches: ['wikipedia'],
  standalone: true,
  accepts: (h) => /^[A-Za-z0-9_.\-]{1,255}$/.test(h),
  async fetch(handle, fetchImpl) {
    const data = (await getJson(
      `https://en.wikipedia.org/w/api.php?action=query&list=users&ususers=${encodeURIComponent(handle)}&usprop=editcount|registration|gender&format=json`,
      fetchImpl
    )) as { query?: { users?: Array<Record<string, unknown>> } } | null;
    const user = data?.query?.users?.[0];
    if (!user || user.missing !== undefined || typeof user.userid !== 'number') return null;
    return {
      source: 'Wikipedia',
      url: `https://en.wikipedia.org/wiki/User:${encodeURIComponent(handle)}`,
      profile: {
        displayName: typeof user.name === 'string' ? user.name : handle,
        joinedAt:
          typeof user.registration === 'string' ? user.registration.slice(0, 10) : undefined,
        userId: String(user.userid),
        links: [`https://en.wikipedia.org/wiki/User:${encodeURIComponent(handle)}`],
        counts: numbersOnly({ edit_count: user.editcount })
      },
      raw: user
    };
  }
};

/* --------------------------------------------------------------- Gravatar */

const gravatar: Provider = {
  id: 'gravatar',
  label: 'Gravatar',
  matches: [],
  standalone: true,
  accepts: (h) => /^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/.test(h),
  async fetch(address, fetchImpl) {
    const hash = crypto.createHash('md5').update(address.trim().toLowerCase()).digest('hex');
    const data = (await getJson(`https://www.gravatar.com/${hash}.json`, fetchImpl)) as {
      entry?: Array<Record<string, unknown>>;
    } | null;
    const entry = data?.entry?.[0];
    if (!entry) return null;
    const accounts = Array.isArray(entry.accounts)
      ? (entry.accounts as Array<{ url?: string; username?: string }>)
      : [];
    const links = accounts.map((a) => a.url).filter((v): v is string => Boolean(v));
    return {
      source: 'Gravatar',
      url: String(entry.profileUrl ?? `https://gravatar.com/${hash}`),
      profile: {
        displayName: typeof entry.displayName === 'string' ? entry.displayName : undefined,
        bio: typeof entry.aboutMe === 'string' ? entry.aboutMe.replace(/<[^>]+>/g, '').trim() || undefined : undefined,
        avatar: typeof entry.thumbnailUrl === 'string' ? entry.thumbnailUrl : undefined,
        location: typeof entry.currentLocation === 'string' ? entry.currentLocation : undefined,
        website:
          typeof entry.preferredUsername === 'string' ? entry.preferredUsername : undefined,
        userId: hash,
        links: links.length > 0 ? links : [String(entry.profileUrl ?? '')],
        counts: {}
      },
      raw: entry
    };
  }
};

export const PROVIDERS: Provider[] = [
  github,
  gitlab,
  reddit,
  bluesky,
  mastodon,
  hackernews,
  devto,
  npm,
  keybase,
  steam,
  wikipedia,
  gravatar
];

export function providersForSite(siteName: string): Provider[] {
  const name = siteName.toLowerCase();
  return PROVIDERS.filter((p) => p.matches.some((m) => m === name || name.includes(m)));
}

export function standaloneProviders(): Provider[] {
  return PROVIDERS.filter((p) => p.standalone === true);
}

/**
 * Ask every provider that plausibly covers this handle.
 *
 * Providers run concurrently and each failure is isolated, so one unreachable
 * platform never blocks the rest.
 */
export async function collectProfiles(
  handle: string,
  opts: { fetchImpl?: FetchImpl; providers?: Provider[]; limit?: number } = {}
): Promise<ProviderResult[]> {
  const fetchImpl = opts.fetchImpl ?? fetch;
  const candidates = (opts.providers ?? PROVIDERS).filter((p) => {
    try {
      return p.accepts(handle);
    } catch {
      return false;
    }
  });
  const results = await Promise.all(
    candidates.map(async (provider) => {
      try {
        return await provider.fetch(handle, fetchImpl);
      } catch {
        return null;
      }
    })
  );
  const found = results.filter((r): r is ProviderResult => r !== null);
  return found.slice(0, opts.limit ?? found.length);
}
