import { cookies, headers } from 'next/headers';
import { resolveLocale, dict, type Locale, PLAN_LABELS } from '@/lib/i18n';
import { PLANS } from '@/lib/credits';
import { currentUser } from '@/lib/auth';
import { UpgradeButtons } from '@/components/UpgradeButtons';
import type { Plan } from '@/lib/db';

export const dynamic = 'force-dynamic';

export default async function PricingPage() {
  const jar = await cookies();
  const head = await headers();
  const locale = resolveLocale(jar.get('athar_locale')?.value, head.get('accept-language'));
  const user = await currentUser();
  const currentPlan: Plan = user?.plan ?? 'free';
  const p = dict.pricing;

  const rows: Array<{ en: string; ar: string; value: (plan: Plan) => string }> = [
    {
      en: 'Weekly search units',
      ar: 'وحدات البحث الأسبوعية',
      value: (id) => {
        const units = PLANS[id].weeklyUnits;
        return units === null ? dict.pricing.unlimited[locale] : String(units);
      }
    },
    {
      en: 'Breach records',
      ar: 'سجلات الاختراق',
      value: (id) => {
        const mode = PLANS[id].breachRecords;
        const map = { counts: locale === 'ar' ? 'العدد فقط' : 'Counts only', partial: locale === 'ar' ? 'جزئية' : 'Partial', full: locale === 'ar' ? 'كاملة' : 'Full' };
        return map[mode];
      }
    },
    {
      en: 'Deep search, depths 1–4',
      ar: 'البحث العميق ١–٤',
      value: (id) => (PLANS[id].deepSearch ? '1–4' : '—')
    },
    {
      en: 'JSON / CSV / PDF export',
      ar: 'تصدير JSON / CSV / PDF',
      value: () => (locale === 'ar' ? 'مضمّن' : 'Included')
    },
    {
      en: 'AI summary in PDF',
      ar: 'ملخص ذكي في PDF',
      value: (id) => (PLANS[id].aiSummary ? (locale === 'ar' ? 'مضمّن' : 'Included') : '—')
    },
    {
      en: 'REST API',
      ar: 'واجهة REST',
      value: (id) => (PLANS[id].restApi ? (locale === 'ar' ? 'مضمّن' : 'Included') : '—')
    }
  ];

  return (
    <div className="athar-shell py-12">
      <h1 className="text-3xl font-bold tracking-tight">{p.title[locale]}</h1>
      <p className="mt-2 max-w-2xl text-sm text-slate-400">{p.subtitle[locale]}</p>

      <div className="mt-8 grid gap-4 lg:grid-cols-4">
        {(['free', 'starter', 'pro', 'enterprise'] as Plan[]).map((id) => {
          const plan = PLANS[id];
          const isCurrent = id === currentPlan;
          return (
            <div
              key={id}
              className={`athar-card flex flex-col ${isCurrent ? '!border-athar-500/60' : ''} ${id === 'pro' ? 'ring-1 ring-athar-500/30' : ''}`}
            >
              <div className="flex items-center justify-between">
                <p className="text-sm font-semibold text-slate-200">{PLAN_LABELS[id][locale]}</p>
                {id === 'pro' && (
                  <span className="rounded bg-athar-500/15 px-2 py-0.5 text-[10px] text-athar-400">
                    {locale === 'ar' ? 'موصى به' : 'Recommended'}
                  </span>
                )}
              </div>
              <p className="mt-3 text-3xl font-bold">
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
                  : `${plan.weeklyUnits} ${p.perWeek[locale]}`}
              </p>
              {isCurrent && (
                <p className="mt-3 rounded bg-ink-800 px-2 py-1 text-[11px] text-slate-400">
                  {locale === 'ar' ? 'باقتك الحالية' : 'Your current plan'}
                </p>
              )}
            </div>
          );
        })}
      </div>

      <div className="athar-card mt-8 overflow-x-auto">
        <table className="w-full text-sm">
          <thead>
            <tr className="border-b border-ink-700 text-start text-xs uppercase tracking-wide text-slate-500">
              <th className="py-2 text-start font-medium">{locale === 'ar' ? 'الميزة' : 'Capability'}</th>
              {(['free', 'starter', 'pro', 'enterprise'] as Plan[]).map((id) => (
                <th key={id} className="py-2 text-start font-medium">
                  {PLAN_LABELS[id][locale]}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((row) => (
              <tr key={row.en} className="border-b border-ink-800/60 last:border-0">
                <td className="py-2 text-slate-400">{row[locale]}</td>
                {(['free', 'starter', 'pro', 'enterprise'] as Plan[]).map((id) => (
                  <td key={id} className="py-2 text-slate-200">
                    {row.value(id)}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      <div className="athar-card mt-8">
        <h2 className="text-sm font-semibold uppercase tracking-wide text-slate-400">
          {p.unitCosts[locale]}
        </h2>
        <ul className="mt-2 space-y-1 text-sm text-slate-300">
          <li>{p.oneSearch[locale]}</li>
          <li>{p.reveal[locale]}</li>
          <li>{p.deep[locale]}</li>
        </ul>
      </div>

      <div className="mt-8 max-w-md">
        <h2 className="mb-3 text-sm font-semibold uppercase tracking-wide text-slate-400">
          {user ? dict.dashboard.manage[locale] : dict.auth.signUp[locale]}
        </h2>
        <UpgradeButtons currentPlan={currentPlan} authenticated={Boolean(user)} />
      </div>

      <p className="mt-8 text-xs text-slate-600">{dict.common.disclaimer[locale]}</p>
    </div>
  );
}
