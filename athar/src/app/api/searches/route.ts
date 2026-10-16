import { currentUser } from '@/lib/auth';
import { db, type SearchRow } from '@/lib/db';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

export async function GET() {
  const user = await currentUser();
  if (!user) {
    return Response.json({ code: 'unauthenticated', message: 'Sign in to see history.' }, { status: 401 });
  }
  const rows = db()
    .prepare(
      `SELECT id, query, kind, depth, created_at, status, checked_count, found_count, units, locale
         FROM searches WHERE user_id = ? ORDER BY id DESC LIMIT 100`
    )
    .all(user.id) as SearchRow[];
  return Response.json({ searches: rows });
}
