import { NextRequest } from 'next/server';
import { currentUser } from '@/lib/auth';
import { db, type FindingRow, type SearchRow } from '@/lib/db';
import type { Profile } from '@/lib/engine/checker';
import type { RankedHit } from '@/lib/engine/rank';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

export async function GET(
  _request: NextRequest,
  { params }: { params: Promise<{ id: string }> }
) {
  const { id } = await params;
  const searchId = Number(id);
  if (!Number.isInteger(searchId)) {
    return Response.json({ code: 'invalid_id', message: 'Invalid search id.' }, { status: 400 });
  }

  const user = await currentUser();
  const row = db()
    .prepare('SELECT * FROM searches WHERE id = ?')
    .get(searchId) as SearchRow | undefined;

  if (!row) {
    return Response.json({ code: 'not_found', message: 'Search not found.' }, { status: 404 });
  }
  // A search belongs either to an account or to the anonymous caller that made it.
  if (row.user_id !== null && row.user_id !== user?.id) {
    return Response.json({ code: 'forbidden', message: 'Not your search.' }, { status: 403 });
  }

  const findings = db()
    .prepare('SELECT * FROM findings WHERE search_id = ? ORDER BY score DESC, site_name ASC')
    .all(searchId) as FindingRow[];

  const hits: RankedHit[] = findings.map((finding) => ({
    siteName: finding.site_name,
    siteUrlMain: finding.site_url_main,
    resultUrl: finding.result_url,
    category: finding.category as RankedHit['category'],
    tier: finding.tier,
    score: finding.score,
    via: finding.via,
    profile: JSON.parse(finding.profile_json) as Profile
  }));

  const byCategory: Record<string, number> = {};
  for (const hit of hits) byCategory[hit.category] = (byCategory[hit.category] ?? 0) + 1;

  return Response.json({
    search: {
      id: row.id,
      query: row.query,
      kind: row.kind,
      depth: row.depth,
      createdAt: row.created_at,
      status: row.status,
      checked: row.checked_count,
      found: row.found_count,
      units: row.units,
      locale: row.locale
    },
    summary: {
      handle: row.query,
      checked: row.checked_count,
      total: row.checked_count,
      found: row.found_count,
      skipped: 0,
      failed: 0,
      elapsedMs: 0,
      depthReached: row.depth,
      byCategory,
      leads: { handles: [], emails: [] }
    },
    hits
  });
}
