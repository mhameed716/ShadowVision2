import Link from 'next/link';
import { cookies, headers } from 'next/headers';
import { redirect } from 'next/navigation';
import { resolveLocale, dict, type Locale, PLAN_LABELS } from '@/lib/i18n';
import { currentUser } from '@/lib/auth';
import { allowanceFor, nextReset, PLANS } from '@/lib/credits';
import { db, type SearchRow } from '@/lib/db';
import { hibpConfigured } from '@/lib/hibp';

export const dynamic = 'force-dynamic';

export default async function DashboardPage() {
  const jar = await cookies();
  const head = await headers();
  const locale = resolveLocale(jar.get('athar_locale')?.value, head.get('accept-language'));
  const user = await currentUser();
  if (!user) redirect('/login');

  const allowance = allowanceFor(user.id, user.plan);
  const searches = db()
    .prepare(
      `SELECT id, query, kind, depth, created_at, status, checked_count, found_count, units
         FROM searches WHERE user_id = ? ORDER BY id DESC LIMIT 50`
    )
    .all(user.id) as SearchRow[];

  const d = dict.dashboard;
  const breachReady = hibpConfigured();
  const breachMode = PLANS[user.plan].breachRecords;

  return (
    <div className="athar-shell py-10">
      <div className="flex flex-wrap items-end justify-between gap-4">
        <div>
          <h1 className="text-2xl font-bold tracking-tight">{d.title[locale]}</h1>
          <p className="mt-1 text-sm text-slate-400" dir="ltr">
            {user.email}
          </p>
        </div>
        <div className="flex gap-2">
          <Link href="/" className="athar-btn-ghost !text-xs">
            {dict.nav.search[locale]}
          </Link>
          <Link href="/pricing" className="athar-btn !text-xs">
            {PLANS[user.plan].weeklyUnits === 20 ? dict.dashboard.upgrade[locale] : dict.dashboard.manage[locale]}
          </Link>
        </div>
      </div>

      <div className="mt-6 grid gap-3 sm:grid-cols-3">
        <div className="athar-card">
          <p className="text-xs uppercase tracking-wide text-slate-500">{d.plan[locale]}</p>
          <p className="mt-1 text-xl font-bold text-slate-100">{PLAN_LABELS[user.plan][locale]}</p>
        </div>
        <div className="athar-card">
          <p className="text-xs uppercase tracking-wide text-slate-500">{d.usage[locale]}</p>
          <p className="mt-1 text-xl font-bold text-slate-100">
            {allowance.unlimited
              ? dict.pricing.unlimited[locale]
              : `${allowance.used} / ${allowance.weeklyLimit}`}
          </p>
          {!allowance.unlimited && (
            <p className="mt-1 text-xs text-athar-400">
              {dict.search.unitsLeft[locale]}: {allowance.remaining}
            </p>
          )}
        </div>
        <div className="athar-card">
          <p className="text-xs uppercase tracking-wide text-slate-500">{d.resets[locale]}</p>
          <p className="mt-1 text-sm text-slate-200" dir="ltr">
            {nextReset().toISOString().slice(0, 10)}
          </p>
          <p className="mt-1 text-xs text-slate-500">00:00 UTC</p>
        </div>
      </div>

      <div className="athar-card mt-4">
        <p className="text-xs uppercase tracking-wide text-slate-500">{d.breach[locale]}</p>
        {breachReady ? (
          <p className="mt-1 text-sm text-athar-400">
            {locale === 'ar' ? 'مُعدّ' : 'Configured'} · {breachMode}
          </p>
        ) : (
          <>
            <p className="mt-1 text-sm text-amber-300">{d.notConfigured[locale]}</p>
            <p className="mt-1 text-xs text-slate-500">{d.configure[locale]}</p>
          </>
        )}
      </div>

      <h2 className="mt-8 text-sm font-semibold uppercase tracking-wide text-slate-400">
        {d.history[locale]}
      </h2>
      {searches.length === 0 ? (
        <p className="athar-card mt-3 text-sm text-slate-500">{d.empty[locale]}</p>
      ) : (
        <ul className="mt-3 space-y-2">
          {searches.map((row) => (
            <li key={row.id} className="athar-card flex flex-wrap items-center justify-between gap-3">
              <div className="min-w-0">
                <p className="truncate font-mono text-sm text-slate-200" dir="ltr">
                  {row.query}
                </p>
                <p className="mt-0.5 text-xs text-slate-500">
                  {row.kind} · {row.found_count} {dict.search.found[locale]} ·{' '}
                  <span dir="ltr">{row.created_at.slice(0, 16).replace('T', ' ')}</span>
                  {row.depth > 0 && ` · depth ${row.depth}`}
                </p>
              </div>
              <div className="flex items-center gap-2">
                <span
                  className={`rounded px-2 py-0.5 text-[10px] uppercase ${
                    row.status === 'done'
                      ? 'bg-athar-500/15 text-athar-400'
                      : row.status === 'error'
                        ? 'bg-red-500/15 text-red-300'
                        : 'bg-ink-700 text-slate-400'
                  }`}
                >
                  {row.status}
                </span>
                <Link
                  href={`/dashboard/searches/${row.id}`}
                  className="athar-btn-ghost !px-3 !py-1 !text-xs"
                >
                  {d.open[locale]}
                </Link>
              </div>
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}
