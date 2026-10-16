import type { RankedHit } from '@/lib/engine/rank';
import { CATEGORY_LABELS, type Category } from '@/lib/engine/catalog';
import { dict, type Locale } from '@/lib/i18n';

const TIER_STYLE: Record<number, string> = {
  1: 'bg-athar-500/15 text-athar-400 border-athar-500/30',
  2: 'bg-sky-500/10 text-sky-300 border-sky-500/25',
  3: 'bg-ink-700/60 text-slate-400 border-ink-600'
};

export function ResultCard({
  hit,
  locale,
  synthetic = false
}: {
  hit: RankedHit;
  locale: Locale;
  synthetic?: boolean;
}) {
  const r = dict.results;
  const category = CATEGORY_LABELS[hit.category as Category]?.[locale] ?? hit.category;
  const counts = Object.entries(hit.profile.counts ?? {});

  return (
    <li className="athar-row athar-card flex flex-col gap-2 sm:flex-row sm:items-start sm:justify-between">
      <div className="min-w-0 flex-1">
        <div className="flex flex-wrap items-center gap-2">
          <span className="font-semibold text-slate-100">{hit.siteName}</span>
          <span
            className={`rounded border px-1.5 py-0.5 text-[10px] uppercase tracking-wide ${TIER_STYLE[hit.tier] ?? TIER_STYLE[3]}`}
          >
            tier {hit.tier}
          </span>
          <span className="rounded border border-ink-700 px-1.5 py-0.5 text-[10px] text-slate-400">
            {category}
          </span>
          {synthetic && (
            <span className="rounded border border-amber-500/40 bg-amber-500/10 px-1.5 py-0.5 text-[10px] uppercase tracking-wide text-amber-300">
              {dict.common.syntheticTag[locale]}
            </span>
          )}
          {hit.via && (
            <span className="text-[10px] text-slate-500">
              {dict.search.via[locale]}{' '}
              <span className="font-mono text-athar-400">@{hit.via}</span>
            </span>
          )}
        </div>

        {hit.profile.displayName && (
          <p className="mt-1 truncate text-sm text-slate-300">{hit.profile.displayName}</p>
        )}
        {hit.profile.bio && (
          <p className="mt-0.5 line-clamp-2 text-xs text-slate-400">{hit.profile.bio}</p>
        )}
        {counts.length > 0 && (
          <p className="mt-1 flex flex-wrap gap-3 text-[11px] text-slate-500">
            {counts.map(([key, value]) => (
              <span key={key}>
                <strong className="text-slate-300">{value.toLocaleString('en-US')}</strong>{' '}
                {r[key as 'followers']?.[locale] ?? key}
              </span>
            ))}
          </p>
        )}
      </div>

      <div className="flex shrink-0 items-center gap-2">
        <span className="text-[11px] tabular-nums text-slate-500">
          {r.rank[locale]} {hit.score}
        </span>
        <a
          href={hit.resultUrl}
          target="_blank"
          rel="noopener noreferrer nofollow"
          className="athar-btn-ghost !px-3 !py-1 !text-xs"
        >
          {r.open[locale]}
        </a>
      </div>
    </li>
  );
}
