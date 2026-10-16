import { cookies, headers } from 'next/headers';
import { resolveLocale, dict, type Locale } from '@/lib/i18n';
import { corpusStats, loadSites, CATEGORY_LABELS, type Category } from '@/lib/engine/sites';
import { currentUser, anonKey, clientIp } from '@/lib/auth';
import { allowanceFor, anonBudgetFor, nextReset } from '@/lib/credits';
import { PLANS } from '@/lib/credits';
import { SearchPanel, type Budget } from '@/components/SearchPanel';
import Link from 'next/link';

export const dynamic = 'force-dynamic';

export default async function HomePage() {
  const jar = await cookies();
  const head = await headers();
  const locale = resolveLocale(jar.get('athar_locale')?.value, head.get('accept-language'));
  const sites = loadSites();
  const stats = corpusStats(sites);
  const user = await currentUser();

  const budget: Budget = user
    ? { authenticated: true, allowance: allowanceFor(user.id, user.plan) }
    : {
        authenticated: false,
        anonymous: anonBudgetFor(anonKey(clientIp(head)))
      };

  const canDeepSearch = Boolean(user && PLANS[user.plan].deepSearch);
  const synthetic = process.env.ATHAR_OFFLINE === '1';

  return (
    <>
      <SearchPanel
        endpointCount={stats.total}
        budget={budget}
        canDeepSearch={canDeepSearch}
        synthetic={synthetic}
      />

      <section className="athar-shell border-t border-ink-800 py-12">
        <h2 className="text-lg font-semibold tracking-tight">
          {locale === 'ar' ? 'ما يغطّيه الفحص.' : 'What a scan covers.'}
        </h2>
        <div className="mt-4 grid gap-3 sm:grid-cols-2 lg:grid-cols-3">
          {(Object.keys(stats.byCategory) as Category[]).map((category) => (
            <div key={category} className="athar-card">
              <p className="text-xs uppercase tracking-wide text-slate-500">
                {CATEGORY_LABELS[category][locale]}
              </p>
              <p className="mt-1 text-2xl font-bold text-slate-100">
                {stats.byCategory[category]}
                <span className="ms-1 text-xs font-normal text-slate-500">
                  {locale === 'ar' ? 'مصدرًا' : 'endpoints'}
                </span>
              </p>
            </div>
          ))}
        </div>
      </section>

      <section className="athar-shell border-t border-ink-800 py-12">
        <h2 className="text-lg font-semibold tracking-tight">
          {dict.pricing.title[locale]}
        </h2>
        <p className="mt-2 max-w-2xl text-sm text-slate-400">{dict.pricing.subtitle[locale]}</p>
        <div className="mt-5 grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
          {(['free', 'starter', 'pro', 'enterprise'] as const).map((id) => {
            const plan = PLANS[id];
            return (
              <div key={id} className="athar-card flex flex-col">
                <p className="text-xs uppercase tracking-wide text-slate-500">{id}</p>
                <p className="mt-1 text-2xl font-bold">
                  {plan.priceMonthlyUsd === null
                    ? locale === 'ar'
                      ? 'حسب العقد'
                      : 'Custom'
                    : `$${plan.priceMonthlyUsd}`}
                  {plan.priceMonthlyUsd !== null && (
                    <span className="ms-1 text-xs font-normal text-slate-500">/mo</span>
                  )}
                </p>
                <p className="mt-1 text-xs text-slate-400">
                  {plan.weeklyUnits === null
                    ? dict.pricing.unlimited[locale]
                    : `${plan.weeklyUnits} ${dict.pricing.perWeek[locale]}`}
                </p>
                <Link
                  href={id === 'enterprise' ? '/pricing' : '/pricing'}
                  className="athar-btn-ghost mt-4 !py-1.5 !text-xs"
                >
                  {id === 'free' ? dict.pricing.startFree[locale] : dict.pricing.choose[locale]}
                </Link>
              </div>
            );
          })}
        </div>
        <p className="mt-4 text-xs text-slate-600">
          {dict.pricing.oneSearch[locale]} · {dict.pricing.reveal[locale]} ·{' '}
          {dict.pricing.deep[locale]}
        </p>
      </section>

      <section className="athar-shell border-t border-ink-800 py-12">
        <h2 className="text-lg font-semibold tracking-tight">
          {locale === 'ar' ? 'كيف يعمل البحث.' : 'How a search works.'}
        </h2>
        <ol className="mt-4 grid gap-3 sm:grid-cols-3">
          {[
            {
              en: ['You type a handle', 'Handles, @mentions and pasted profile URLs are normalized.'],
              ar: ['تكتب معرّفًا', 'تُوحَّد المعرّفات وإشارات @ وروابط الملفات الشخصية.']
            },
            {
              en: ['Sources are checked', `The query fans out across ${stats.total} endpoints concurrently.`],
              ar: ['تُفحص المصادر', `يُوزَّع الطلب على ${stats.total} مصدرًا بالتوازي.`]
            },
            {
              en: ['Hits are filtered and ranked', 'Brand echoes and placeholders are dropped, then ranked by tier, audience and public data.'],
              ar: ['تُصفّى النتائج وتُرتَّب', 'تُستبعد الحسابات الوهمية ثم تُرتَّب حسب الأهمية والجمهور والبيانات.']
            }
          ].map((step, index) => (
            <li key={index} className="athar-card">
              <p className="text-xs text-slate-600">0{index + 1}</p>
              <p className="mt-1 text-sm font-semibold text-slate-100">{step[locale][0]}</p>
              <p className="mt-1 text-xs text-slate-400">{step[locale][1]}</p>
            </li>
          ))}
        </ol>
      </section>
    </>
  );
}
