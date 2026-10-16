/**
 * Pure corpus metadata — no Node builtins, safe to import from client components.
 *
 * Categories, tiers and the ranking model are Athar's own. The endpoint list
 * itself lives in data/sites.json and is loaded by ./sites.ts.
 */
export interface SiteDef {
  name: string;
  url: string;
  urlMain: string;
  errorType: 'message' | 'status_code' | 'response_url';
  errorMsg?: string[];
  errorCode?: number[];
  errorUrl?: string;
  regexCheck?: string;
  isNSFW?: boolean;
  request_method?: 'GET' | 'POST';
  request_payload?: unknown;
  urlProbe?: string;
  headers?: Record<string, string>;
  username_claimed?: string;
}

export type Category =
  | 'social'
  | 'creator'
  | 'developer'
  | 'gaming'
  | 'commerce'
  | 'long_tail';

export const CATEGORY_LABELS: Record<Category, { en: string; ar: string }> = {
  social: { en: 'Social and messaging', ar: 'التواصل والمراسلة' },
  creator: { en: 'Creator and media', ar: 'المنشئون والوسائط' },
  developer: { en: 'Developer and tech', ar: 'المطورون والتقنية' },
  gaming: { en: 'Gaming', ar: 'الألعاب' },
  commerce: { en: 'Commerce and links', ar: 'التجارة والروابط' },
  long_tail: { en: 'Long tail', ar: 'المنصات الأخرى' }
};

const CATEGORY_RULES: Array<[Category, RegExp]> = [
  ['developer', /github|gitlab|bitbucket|gitbook|npm|pypi|stackexchange|stackoverflow|codepen|codesandbox|replit|hackernews|hackerone|dev\.to|hashnode|kaggle|docker|package|dev|coder|programming|sourceforge|launchpad|archive\.org/i],
  ['gaming', /steam|xbox|playstation|nintendo|twitch|roblox|minecraft|epic|origin|ea\.|game|gaming|chess|lichess|speedrun|osu|riot|blizzard|battle/i],
  ['creator', /youtube|vimeo|tiktok|instagram|soundcloud|bandcamp|spotify|mixcloud|deviantart|behance|dribbble|artstation|flickr|500px|medium|substack|patreon|onlyfans|fanhouse|kick|rumble|odysee|tumblr|pinterest|podcast|music|video|photo|art|film|stream/i],
  ['commerce', /amazon|ebay|etsy|shopify|gumroad|aliexpress|wish|mercari|poshmark|producthunt|linktr|beacons|cashapp|paypal|venmo|buyfl|marketplace|shop|store|sell|deal|link/i],
  ['social', /twitter|^x$|facebook|linkedin|telegram|discord|whatsapp|signal|snapchat|reddit|quora|mastodon|bluesky|threads|vk\.com|ok\.ru|tinder|bumble|weibo|line\b|kik|clubhouse|nextdoor|gab|minds|social|forum|community|board|chat/i],
  ['long_tail', /.*/]
];

/** Platforms whose hits carry the most investigative weight. Tier 1 is highest. */
const TIER_ONE = new Set([
  'twitter', 'x', 'instagram', 'facebook', 'tiktok', 'youtube', 'linkedin',
  'github', 'reddit', 'telegram', 'discord', 'twitch', 'pinterest', 'snapchat',
  'spotify', 'medium', 'mastodon', 'bluesky', 'threads'
]);
const TIER_TWO = new Set([
  'vimeo', 'soundcloud', 'behance', 'dribbble', 'dev.to', 'gitlab', 'bitbucket',
  'npm', 'stackoverflow', 'quora', 'tumblr', 'flickr', 'steam', 'patreon',
  'producthunt', 'kaggle', 'archive.org', 'codepen', 'replit', 'hackernews'
]);

export function categoryOf(name: string, urlMain: string): Category {
  const haystack = `${name} ${urlMain}`;
  for (const [category, pattern] of CATEGORY_RULES) {
    if (pattern.test(haystack)) return category;
  }
  return 'long_tail';
}

export function tierOf(name: string): number {
  const n = name.toLowerCase();
  if (TIER_ONE.has(n)) return 1;
  if (TIER_TWO.has(n)) return 2;
  return 3;
}

export interface CorpusStats {
  total: number;
  byCategory: Record<Category, number>;
  byTier: Record<number, number>;
}

export function corpusStats(sites: SiteDef[]): CorpusStats {
  const byCategory: Record<Category, number> = {
    social: 0, creator: 0, developer: 0, gaming: 0, commerce: 0, long_tail: 0
  };
  const byTier: Record<number, number> = { 1: 0, 2: 0, 3: 0 };
  for (const site of sites) {
    byCategory[categoryOf(site.name, site.urlMain)] += 1;
    byTier[tierOf(site.name)] += 1;
  }
  return { total: sites.length, byCategory, byTier };
}
