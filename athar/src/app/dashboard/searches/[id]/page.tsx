import Link from 'next/link';
import { notFound } from 'next/navigation';
import { cookies, headers } from 'next/headers';
import { resolveLocale, dict, type Locale } from '@/lib/i18n';
import { currentUser } from '@/lib/auth';
import { db, type FindingRow, type SearchRow } from '@/lib/db';
import { ResultCard } from '@/components/ResultCard';
import { CATEGORY_LABELS, type Category } from '@/lib/engine/sites';
import type { Profile } from '@/lib/engine/checker';
import type { RankedHit } from '@/lib/engine/rank';

export const dynamic = 'force-dynamic';

export default async function SearchDetailPage({
  params
}: {
  params: Promise<{ id: string }>;
}) {
  const { id } = await params;
  const searchId = Number(id);
  if (!Number.isInteger(searchId)) notFound();

  const jar = await cookies();
  const head = await headers();
  const locale = resolveLocale(jar.get('athar_locale')?.value, head.get('accept-language'));

  const user = await currentUser();
  const row = db().prepare('SELECT * FROM searches WHERE id = ?').get(searchId) as
    | SearchRow
    | undefined;
  if (!row) notFound();
  if (row.user_id !== null && row.user_id !== user?.id) notFound();

  const findings = db()
    .prepare('SELECT * FROM findings WHERE search_id = ? ORDER BY score DESC, site_name ASC')
    .all(searchId) as FindingRow[];

  const hits: RankedHit[] = findings.map((finding) => ({
    siteName: finding.site_name,
    siteUrlMain: finding.site_url_main,
    resultUrl: finding.result_url,
    category: finding.category as RankedHit['category'],
    tier: finding.tier,
    score: finding.score,
    via: finding.via,
    profile: JSON.parse(finding.profile_json) as Profile
  }));

  const byCategory: Record<string, number> = {};
  for (const hit of hits) byCategory[hit.category] = (byCategory[hit.category] ?? 0) + 1;

  const r = dict.results;
  const d = dict.dashboard;

  return (
    <div className="athar-shell py-10">
      <Link href="/dashboard" className="text-xs text-slate-500 hover:text-slate-300">
        ← {d.title[locale]}
      </Link>

      <div className="mt-3 flex flex-wrap items-end justify-between gap-4">
        <div>
          <h1 className="font-mono text-2xl font-bold" dir="ltr">
            {row.query}
          </h1>
          <p className="mt-1 text-xs text-slate-500">
            {row.kind} · {row.checked_count} {locale === 'ar' ? 'مصدرًا' : 'endpoints'} ·{' '}
            {row.found_count} {dict.search.found[locale]} · {row.units} units
            {row.depth > 0 && ` · depth ${row.depth}`}
          </p>
        </div>
        <div className="flex gap-2">
          {(['json', 'csv', 'pdf'] as const).map((format) => (
            <a
              key={format}
              href={`/api/export/${row.id}/${format}`}
              className="athar-btn-ghost !px-3 !py-1 !text-xs uppercase"
            >
              {format}
            </a>
          ))}
        </div>
      </div>

      {Object.keys(byCategory).length > 0 && (
        <div className="athar-card mt-5 flex flex-wrap gap-3 text-xs text-slate-400">
          {Object.entries(byCategory).map(([category, count]) => (
            <span key={category}>
              {CATEGORY_LABELS[category as Category]?.[locale] ?? category}:{' '}
              <strong className="text-slate-200">{count}</strong>
            </span>
          ))}
        </div>
      )}

      <h2 className="mt-6 text-sm font-semibold uppercase tracking-wide text-slate-400">
        {r.title[locale]} · {hits.length}
      </h2>

      {hits.length === 0 ? (
        <p className="athar-card mt-3 text-sm text-slate-500">{dict.search.noResults[locale]}</p>
      ) : (
        <ul className="mt-3 space-y-2">
          {hits.map((hit) => (
            <ResultCard
              key={`${hit.siteName}-${hit.via ?? ''}`}
              hit={hit}
              locale={locale}
              synthetic={process.env.ATHAR_OFFLINE === '1'}
            />
          ))}
        </ul>
      )}
    </div>
  );
}
