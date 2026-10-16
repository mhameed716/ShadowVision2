import { NextRequest } from 'next/server';
import { currentUser } from '@/lib/auth';
import { PLANS, consumeUnits, allowanceFor, BREACH_REVEAL_COST } from '@/lib/credits';
import { lookupBreaches, hibpConfigured } from '@/lib/hibp';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * Breach reveal. Costs 5 units and is gated on the caller's plan tier, which
 * decides whether identifiers are returned at all. A missing provider key is
 * reported as `not_configured` rather than as an empty result.
 */
export async function POST(request: NextRequest) {
  const user = await currentUser();
  if (!user) {
    return Response.json({ code: 'unauthenticated', message: 'Sign in first.' }, { status: 401 });
  }

  const body = (await request.json().catch(() => null)) as { account?: string } | null;
  const account = body?.account?.trim().toLowerCase() ?? '';
  if (!account) {
    return Response.json({ code: 'invalid_account', message: 'An email or handle is required.' }, { status: 400 });
  }

  const mode = PLANS[user.plan].breachRecords;
  const allowance = allowanceFor(user.id, user.plan);
  if (!allowance.unlimited && (allowance.remaining ?? 0) < BREACH_REVEAL_COST) {
    return Response.json(
      { code: 'no_units', message: `A reveal costs ${BREACH_REVEAL_COST} units.` },
      { status: 429 }
    );
  }

  if (!hibpConfigured()) {
    return Response.json({
      configured: false,
      mode,
      reason: 'no_api_key',
      remediation:
        'Set HIBP_API_KEY to a Have I Been Pwned v3 key purchased from haveibeenpwned.com. Breach corpora are commercially licensed and cannot be sourced from open data.'
    });
  }

  try {
    const result = await lookupBreaches(account, { mode });
    if (result.configured) consumeUnits(user.id, BREACH_REVEAL_COST);
    return Response.json(result);
  } catch (error) {
    const message = error instanceof Error ? error.message : 'lookup_failed';
    const status = message === 'rate_limited' ? 429 : 502;
    return Response.json({ code: message, message: 'Breach lookup failed.' }, { status });
  }
}
