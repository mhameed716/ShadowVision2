import type { Category } from './sites';
import type { Profile } from './checker';

export interface RankedHit {
  siteName: string;
  siteUrlMain: string;
  resultUrl: string;
  category: Category;
  tier: number;
  score: number;
  profile: Profile;
  via: string | null;
}

const TIER_WEIGHT: Record<number, number> = { 1: 100, 2: 60, 3: 25 };

function audienceScore(profile: Profile): number {
  const counts = Object.values(profile.counts ?? {});
  if (counts.length === 0) return 0;
  const peak = Math.max(...counts);
  // Log-scaled so a 10M-follower account does not swamp everything else.
  return Math.min(40, Math.round(Math.log10(peak + 1) * 6));
}

function completenessScore(profile: Profile): number {
  let score = 0;
  if (profile.displayName) score += 6;
  if (profile.bio) score += 6;
  if (profile.avatar) score += 4;
  if (profile.location) score += 4;
  score += Math.min(10, (profile.links?.length ?? 0) * 2);
  score += Math.min(6, Object.keys(profile.counts ?? {}).length * 2);
  return score;
}

/**
 * Major services and verified accounts come first, then profiles with the
 * largest audience and the most public data. Placeholder and brand echoes are
 * filtered upstream, so this only orders genuine hits.
 */
export function scoreHit(
  tier: number,
  profile: Profile,
  opts: { verified?: boolean } = {}
): number {
  let score = TIER_WEIGHT[tier] ?? 25;
  score += audienceScore(profile);
  score += completenessScore(profile);
  if (opts.verified) score += 30;
  return Math.round(score * 10) / 10;
}

export function rankHits(hits: RankedHit[]): RankedHit[] {
  return [...hits].sort((a, b) => {
    if (b.score !== a.score) return b.score - a.score;
    if (a.tier !== b.tier) return a.tier - b.tier;
    return a.siteName.localeCompare(b.siteName);
  });
}

export function summarize(hits: RankedHit[]): {
  total: number;
  byCategory: Record<string, number>;
  byTier: Record<number, number>;
} {
  const byCategory: Record<string, number> = {};
  const byTier: Record<number, number> = { 1: 0, 2: 0, 3: 0 };
  for (const hit of hits) {
    byCategory[hit.category] = (byCategory[hit.category] ?? 0) + 1;
    byTier[hit.tier] = (byTier[hit.tier] ?? 0) + 1;
  }
  return { total: hits.length, byCategory, byTier };
}
