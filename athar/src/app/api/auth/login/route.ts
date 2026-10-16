import { NextRequest } from 'next/server';
import { findUserByEmail, verifyPassword, createSession } from '@/lib/auth';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

export async function POST(request: NextRequest) {
  const body = (await request.json().catch(() => null)) as {
    email?: string;
    password?: string;
  } | null;

  const email = body?.email?.trim().toLowerCase() ?? '';
  const password = body?.password ?? '';
  const user = findUserByEmail(email);

  // Same response shape whether the account exists or not.
  if (!user || !verifyPassword(password, user.password_hash)) {
    return Response.json({ code: 'invalid_credentials', message: 'Invalid email or password.' }, { status: 401 });
  }

  await createSession(user.id);
  return Response.json({ ok: true, user: { id: user.id, email: user.email, plan: user.plan } });
}
