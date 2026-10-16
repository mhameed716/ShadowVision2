'use client';

import { useState } from 'react';
import { useRouter } from 'next/navigation';
import { useLocale } from './LocaleProvider';
import { dict } from '@/lib/i18n';
import type { Plan } from '@/lib/db';

export function UpgradeButtons({
  currentPlan,
  authenticated
}: {
  currentPlan: Plan;
  authenticated: boolean;
}) {
  const { locale } = useLocale();
  const router = useRouter();
  const [busy, setBusy] = useState<Plan | null>(null);
  const [notice, setNotice] = useState<string | null>(null);

  const upgrade = async (plan: Plan) => {
    if (!authenticated) {
      router.push('/signup');
      return;
    }
    setBusy(plan);
    setNotice(null);
    const response = await fetch('/api/checkout', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ plan })
    });
    const body = (await response.json().catch(() => null)) as {
      checkoutUrl?: string;
      message?: string;
      mode?: string;
    } | null;
    setBusy(null);
    if (!response.ok) {
      setNotice(body?.message ?? dict.common.error[locale]);
      return;
    }
    if (body?.checkoutUrl) {
      window.location.href = body.checkoutUrl;
      return;
    }
    setNotice(body?.message ?? null);
    router.refresh();
  };

  return (
    <div>
      {notice && (
        <p className="athar-card mb-3 !border-amber-500/40 !bg-amber-500/5 text-xs text-amber-200">
          {notice}
        </p>
      )}
      <div className="grid gap-3 sm:grid-cols-2">
        {(['starter', 'pro'] as Plan[]).map((plan) => (
          <button
            key={plan}
            type="button"
            disabled={busy !== null || plan === currentPlan}
            onClick={() => upgrade(plan)}
            className="athar-btn w-full disabled:opacity-40"
          >
            {busy === plan
              ? dict.common.loading[locale]
              : plan === currentPlan
                ? `${dict.dashboard.plan[locale]}: ${plan}`
                : `${dict.pricing.choose[locale]} ${plan}`}
          </button>
        ))}
      </div>
    </div>
  );
}
