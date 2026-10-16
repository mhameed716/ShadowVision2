import { loadSites, categoryOf, tierOf, type SiteDef } from './sites';
import {
  checkSite,
  extractProfile,
  looksLikePlaceholder,
  normalizeHandle,
  type FetchLike
} from './checker';
import { scoreHit, rankHits, type RankedHit } from './rank';
import { extractLeads, rankLeads } from './extract';
import { providersForSite, standaloneProviders } from './providers';
import type { Profile } from './checker';

/**
 * Overlay structured provider data on top of scraped HTML.
 *
 * The provider wins for every field it actually returned, because a platform's
 * own JSON endpoint is more reliable than a regex over its markup. HTML values
 * survive wherever the provider had nothing.
 */
export function mergeProfiles(scraped: Profile, structured: Profile): Profile {
  const links = [...new Set([...(structured.links ?? []), ...(scraped.links ?? [])])].slice(0, 12);
  return {
    ...scraped,
    ...structured,
    displayName: structured.displayName || scraped.displayName,
    bio: structured.bio || scraped.bio,
    avatar: structured.avatar || scraped.avatar,
    location: structured.location || scraped.location,
    website: structured.website || scraped.website,
    links,
    counts: { ...scraped.counts, ...structured.counts }
  };
}

export interface ScanParams {
  query: string;
  depth?: number; // 0..4
  concurrency?: number;
  timeoutMs?: number;
  includeNsfw?: boolean;
  fetchImpl?: FetchLike;
}

export type ScanEvent =
  | { type: 'start'; handle: string; total: number; depth: number }
  | { type: 'progress'; checked: number; total: number; found: number; elapsedMs: number }
  | { type: 'hit'; hit: RankedHit }
  | { type: 'depth'; level: number; seeds: string[] }
  | { type: 'done'; summary: ScanSummary }
  | { type: 'error'; message: string };

export interface ScanSummary {
  handle: string;
  checked: number;
  total: number;
  found: number;
  skipped: number;
  failed: number;
  /** Probes that answered 200 but were dropped as soft-404 or anti-bot pages. */
  filtered: number;
  /** Probes that answered cleanly with "no such profile" — the normal case. */
  notFound: number;
  elapsedMs: number;
  depthReached: number;
  byCategory: Record<string, number>;
  leads: { handles: string[]; emails: string[] };
}

/** Run `limit` tasks at a time, yielding each result as soon as it settles. */
async function* mapPool<T, R>(
  items: T[],
  limit: number,
  fn: (item: T) => Promise<R>
): AsyncGenerator<R> {
  const queue = [...items];
  const pending = new Set<Promise<void>>();
  const results: R[] = [];
  let wake: (() => void) | null = null;
  let active = 0;
  let drained = false;

  const pump = () => {
    while (active < limit && queue.length > 0) {
      const item = queue.shift() as T;
      active += 1;
      const task = fn(item)
        .then((value) => {
          results.push(value);
        })
        .catch(() => {
          /* a single failed probe never aborts the scan */
        })
        .finally(() => {
          active -= 1;
          wake?.();
          pump();
        });
      pending.add(task);
      task.finally(() => pending.delete(task));
    }
    if (active === 0 && queue.length === 0) {
      drained = true;
      wake?.();
    }
  };

  pump();
  while (!drained || results.length > 0) {
    if (results.length > 0) {
      yield results.shift() as R;
      continue;
    }
    await new Promise<void>((resolve) => {
      wake = resolve;
      // Safety net so the generator can never wedge on a missed wake-up.
      setTimeout(resolve, 50);
    });
    wake = null;
  }
  await Promise.allSettled([...pending]);
}

interface ProbeResult {
  site: SiteDef;
  outcome: Awaited<ReturnType<typeof checkSite>>;
  profile: ReturnType<typeof extractProfile> | null;
}

export async function* runScan(params: ScanParams): AsyncGenerator<ScanEvent> {
  const started = Date.now();
  const handle = normalizeHandle(params.query);
  if (!handle) {
    yield { type: 'error', message: 'empty_query' };
    return;
  }

  const maxDepth = Math.max(0, Math.min(params.depth ?? 0, 4));
  const sites = loadSites({ includeNsfw: params.includeNsfw ?? false });
  const concurrency = params.concurrency ?? 24;
  const timeoutMs = params.timeoutMs ?? 12000;

  yield { type: 'start', handle, total: sites.length, depth: maxDepth };

  const hits: RankedHit[] = [];
  const occurrences = new Map<string, number>();
  let checked = 0;
  let skipped = 0;
  let failed = 0;
  let filtered = 0;
  let notFound = 0;
  let depthReached = 0;
  let lastProgress = 0;
  // Probes actually scheduled. Deep search fans out per seed, so the total
  // grows as levels run — reporting a fixed corpus size here would let the
  // progress bar pass 100%.
  let scheduled = 0;

  const seeds: string[] = [handle];
  const seenSeeds = new Set<string>([handle.toLowerCase()]);

  // Standalone providers are platforms with a keyless JSON endpoint but no
  // corpus entry, so they are queried directly. Each one is a real platform
  // hit, not an HTML guess.
  const standalones = standaloneProviders().filter((p) => {
    try {
      return p.accepts(handle);
    } catch {
      return false;
    }
  });
  for (const provider of standalones) {
    try {
      const result = await provider.fetch(handle, params.fetchImpl ?? fetch);
      if (!result) continue;
      const profile = result.profile;
      const tier = tierOf(provider.id);
      const hit: RankedHit = {
        siteName: provider.label,
        siteUrlMain: new URL(result.url).origin + '/',
        resultUrl: result.url,
        category: categoryOf(provider.label, result.url),
        tier,
        // A structured answer from the platform itself outranks a scraped page.
        score: scoreHit(tier, profile, { verified: profile.verified === true }) + 20,
        profile: { ...profile, source: result.source, raw: result.raw },
        via: null
      };
      hits.push(hit);
      checked += 1;
      yield { type: 'hit', hit };
    } catch {
      /* one unreachable provider never blocks the rest */
    }
  }

  for (let level = 0; level <= maxDepth; level += 1) {
    const currentSeeds = level === 0 ? seeds : seeds.slice();
    if (level > 0 && currentSeeds.length === 0) break;
    if (level > 0) {
      depthReached = level;
      yield { type: 'depth', level, seeds: currentSeeds };
    }

    const levelProfiles: ReturnType<typeof extractProfile>[] = [];

    for (const seed of currentSeeds) {
      scheduled += sites.length;
      const probe = async (site: SiteDef): Promise<ProbeResult> => {
        const outcome = await checkSite(site, seed, {
          timeoutMs,
          fetchImpl: params.fetchImpl
        });
        const profile = outcome.kind === 'found' ? extractProfile(outcome.body) : null;
        return { site, outcome, profile };
      };

      for await (const result of mapPool(sites, concurrency, probe)) {
        checked += 1;
        if (result.outcome.kind === 'skipped') {
          skipped += 1;
        } else if (result.outcome.kind === 'error') {
          failed += 1;
        } else if (result.outcome.kind === 'found') {
          let profile = result.profile ?? { links: [], counts: {} };
          if (!looksLikePlaceholder(result.site, profile, seed)) {
            // Structured enrichment: where the platform publishes a keyless JSON
            // endpoint, its data replaces the scraped HTML, because it carries
            // identifiers, join dates and counts the page never exposes.
            let canonicalUrl: string | null = null;
            const provider = providersForSite(result.site.name)[0];
            if (provider && provider.accepts(seed)) {
              try {
                const enriched = await provider.fetch(seed, params.fetchImpl ?? fetch);
                if (enriched) {
                  profile = mergeProfiles(profile, enriched.profile);
                  canonicalUrl = enriched.url;
                }
              } catch {
                /* a failed provider must never discard the HTML hit */
              }
            }

            levelProfiles.push(profile);
            const tier = tierOf(result.site.name);
            const hit: RankedHit = {
              siteName: result.site.name,
              siteUrlMain: result.site.urlMain,
              resultUrl: canonicalUrl ?? result.outcome.finalUrl,
              category: categoryOf(result.site.name, result.site.urlMain),
              tier,
              score: scoreHit(tier, profile),
              profile,
              via: seed.toLowerCase() === handle.toLowerCase() ? null : seed
            };
            hits.push(hit);
            occurrences.set(
              seed.toLowerCase(),
              (occurrences.get(seed.toLowerCase()) ?? 0) + 1
            );
            yield { type: 'hit', hit };
          } else {
            // A 200 that is really a soft-404 or an anti-bot interstitial. It
            // consumed a probe, so it must be accounted for or `checked`
            // silently drifts past found + failed + skipped.
            filtered += 1;
          }
        } else {
          // 'not_found': a clean 404-style answer, which is the expected
          // outcome for almost every site. It still consumed a probe, so it
          // must be counted or the summary under-reports by hundreds.
          notFound += 1;
        }

        const now = Date.now();
        if (now - lastProgress > 250 || checked >= scheduled) {
          lastProgress = now;
          yield {
            type: 'progress',
            checked,
            total: scheduled,
            found: hits.length,
            elapsedMs: now - started
          };
        }
      }
    }

    if (level >= maxDepth) break;

    const leads = extractLeads(levelProfiles, handle);
    const nextSeeds = rankLeads(leads, occurrences)
      .filter((h) => !seenSeeds.has(h.toLowerCase()))
      .slice(0, 8);
    for (const seed of nextSeeds) seenSeeds.add(seed.toLowerCase());
    seeds.length = 0;
    seeds.push(...nextSeeds);
  }

  const ranked = rankHits(hits);
  const byCategory: Record<string, number> = {};
  for (const hit of ranked) {
    byCategory[hit.category] = (byCategory[hit.category] ?? 0) + 1;
  }
  const finalLeads = extractLeads(
    ranked.map((h) => h.profile),
    handle
  );

  // `checked` counts one probe per seed per site, so `total` must be the number
  // of probes actually scheduled, plus the standalone provider calls. Deriving
  // it from depth levels instead would under-report whenever a level fans out
  // across several seeds, and the progress bar would pass 100%.
  const totalChecks = scheduled + standalones.length;

  yield {
    type: 'done',
    summary: {
      handle,
      checked,
      total: totalChecks,
      found: ranked.length,
      skipped,
      failed,
      filtered,
      notFound,
      elapsedMs: Date.now() - started,
      depthReached,
      byCategory,
      leads: { handles: finalLeads.handles.slice(0, 15), emails: finalLeads.emails }
    }
  };
}
