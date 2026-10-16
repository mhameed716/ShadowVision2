import { NextRequest } from 'next/server';
import { currentUser, anonKey, clientIp } from '@/lib/auth';
import { PLANS, allowanceFor, anonBudgetFor, nextReset } from '@/lib/credits';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

export async function GET(request: NextRequest) {
  const user = await currentUser();
  const resetsAt = nextReset().toISOString();

  if (!user) {
    const budget = anonBudgetFor(anonKey(clientIp(request.headers)));
    return Response.json({
      authenticated: false,
      plan: 'free',
      planLabel: PLANS.free,
      anonymous: budget,
      resetsAt
    });
  }

  const allowance = allowanceFor(user.id, user.plan);
  return Response.json({
    authenticated: true,
    user: { id: user.id, email: user.email, plan: user.plan },
    plan: user.plan,
    planLabel: PLANS[user.plan],
    allowance,
    resetsAt
  });
}
