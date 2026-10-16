import { NextRequest } from 'next/server';
import { createUser, findUserByEmail, createSession } from '@/lib/auth';
import { isValidEmail } from '@/lib/engine/checker';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

export async function POST(request: NextRequest) {
  const body = (await request.json().catch(() => null)) as {
    email?: string;
    password?: string;
  } | null;

  const email = body?.email?.trim().toLowerCase() ?? '';
  const password = body?.password ?? '';

  if (!isValidEmail(email)) {
    return Response.json({ code: 'invalid_email', message: 'Enter a valid email address.' }, { status: 400 });
  }
  if (password.length < 8) {
    return Response.json({ code: 'weak_password', message: 'Password must be at least 8 characters.' }, { status: 400 });
  }
  if (findUserByEmail(email)) {
    return Response.json({ code: 'email_taken', message: 'An account with that email already exists.' }, { status: 409 });
  }

  const user = createUser(email, password);
  await createSession(user.id);
  return Response.json({ ok: true, user: { id: user.id, email: user.email, plan: user.plan } });
}
