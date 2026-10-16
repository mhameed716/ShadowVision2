import fs from 'node:fs';
import path from 'node:path';
import type { SiteDef } from './catalog';

/**
 * Site corpus loader (server-side only — reads from disk).
 *
 * The endpoint list is derived from the Sherlock Project's `data.json`, which is
 * MIT licensed (see data/SITES-LICENSE). Pure metadata used by both server and
 * client code lives in ./catalog.ts.
 */
export type { SiteDef } from './catalog';
export {
  CATEGORY_LABELS,
  categoryOf,
  tierOf,
  corpusStats
} from './catalog';
export type { Category, CorpusStats } from './catalog';

let cachedSites: SiteDef[] | null = null;

export function loadSites(opts: { includeNsfw?: boolean } = {}): SiteDef[] {
  if (!cachedSites) {
    const dataDir = path.join(process.cwd(), 'data');
    const raw = JSON.parse(
      fs.readFileSync(path.join(dataDir, 'sites.json'), 'utf8')
    ) as Record<string, unknown>;

    // Athar's own supplementary corpus is merged over the base, so an entry
    // here overrides a base entry of the same name (e.g. a corrected probe URL).
    const supplementPath = path.join(dataDir, 'sites.athar.json');
    if (fs.existsSync(supplementPath)) {
      const supplement = JSON.parse(fs.readFileSync(supplementPath, 'utf8')) as Record<
        string,
        unknown
      >;
      for (const [name, value] of Object.entries(supplement)) {
        if (name.startsWith('$')) continue;
        if (!value || typeof value !== 'object') continue;
        raw[name] = value;
      }
    }

    const sites: SiteDef[] = [];
    for (const [name, value] of Object.entries(raw)) {
      if (name.startsWith('$') || !value || typeof value !== 'object') continue;
      const def = value as Partial<SiteDef>;
      if (!def.url || !def.errorType) continue;
      // `name` is the corpus key, so it wins over any field inside the entry.
      sites.push({ ...(def as SiteDef), name });
    }
    cachedSites = sites;
  }
  return opts.includeNsfw ? cachedSites : cachedSites.filter((s) => !s.isNSFW);
}
