import { NextRequest } from 'next/server';
import { currentUser } from '@/lib/auth';
import { db, type Plan } from '@/lib/db';
import { PLANS } from '@/lib/credits';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * Checkout.
 *
 * With STRIPE_SECRET_KEY set this creates a real Checkout Session against the
 * Stripe REST API (no SDK, so no extra dependency). Without a key it falls back
 * to an explicit, clearly-labelled development top-up so the upgrade flow is
 * testable locally. Dev mode never pretends a payment happened: the response
 * carries `mode: "dev"` and the ledger records the reason.
 */

const UPGRADEABLE: Plan[] = ['starter', 'pro'];

export async function POST(request: NextRequest) {
  const body = (await request.json().catch(() => null)) as { plan?: string } | null;
  const plan = (body?.plan ?? '') as Plan;

  if (!UPGRADEABLE.includes(plan)) {
    return Response.json(
      { code: 'invalid_plan', message: 'Choose starter or pro.' },
      { status: 400 }
    );
  }

  const user = await currentUser();
  if (!user) {
    return Response.json({ code: 'unauthenticated', message: 'Sign in first.' }, { status: 401 });
  }

  const def = PLANS[plan];
  const secretKey = process.env.STRIPE_SECRET_KEY;

  if (secretKey) {
    const priceId =
      plan === 'starter' ? process.env.STRIPE_PRICE_STARTER : process.env.STRIPE_PRICE_PRO;
    if (!priceId) {
      return Response.json(
        {
          code: 'misconfigured',
          message: `STRIPE_PRICE_${plan.toUpperCase()} is not set.`
        },
        { status: 500 }
      );
    }
    const origin = request.headers.get('origin') ?? request.nextUrl.origin;
    const form = new URLSearchParams({
      mode: 'subscription',
      'line_items[0][price]': priceId,
      'line_items[0][quantity]': '1',
      success_url: `${origin}/dashboard?upgraded=${plan}`,
      cancel_url: `${origin}/pricing`,
      client_reference_id: String(user.id)
    });
    const response = await fetch('https://api.stripe.com/v1/checkout/sessions', {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${secretKey}`,
        'Content-Type': 'application/x-www-form-urlencoded'
      },
      body: form
    });
    if (!response.ok) {
      const detail = await response.text();
      return Response.json(
        { code: 'stripe_error', message: `Stripe rejected the request (${response.status}).`, detail },
        { status: 502 }
      );
    }
    const session = (await response.json()) as { id: string; url: string };
    // The plan itself flips in the webhook, so a cancelled checkout changes nothing.
    return Response.json({ mode: 'live', checkoutUrl: session.url, sessionId: session.id });
  }

  // Development top-up: apply the plan immediately and label it honestly.
  //
  // The plan change is the whole entitlement — `weeklyLimit` is derived from
  // PLANS, so a Pro account already has 500 units this week. Granting units on
  // top as well would double the published allowance.
  db().prepare('UPDATE users SET plan = ? WHERE id = ?').run(plan, user.id);
  return Response.json({
    mode: 'dev',
    plan,
    weeklyLimit: def.weeklyUnits,
    message:
      'No STRIPE_SECRET_KEY configured — this was a development plan change, not a payment.'
  });
}
