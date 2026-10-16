import { NextRequest } from 'next/server';
import { currentUser } from '@/lib/auth';
import { db, type FindingRow, type SearchRow } from '@/lib/db';
import { exportJson, exportCsv, exportPdf } from '@/lib/export';
import type { Profile } from '@/lib/engine/checker';
import type { RankedHit } from '@/lib/engine/rank';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

const CONTENT_TYPES: Record<string, string> = {
  json: 'application/json; charset=utf-8',
  csv: 'text/csv; charset=utf-8',
  pdf: 'application/pdf'
};

export async function GET(
  _request: NextRequest,
  { params }: { params: Promise<{ id: string; format: string }> }
) {
  const { id, format } = await params;
  const searchId = Number(id);
  if (!Number.isInteger(searchId) || !CONTENT_TYPES[format]) {
    return Response.json({ code: 'invalid_request', message: 'Bad id or format.' }, { status: 400 });
  }

  const user = await currentUser();
  const row = db().prepare('SELECT * FROM searches WHERE id = ?').get(searchId) as
    | SearchRow
    | undefined;
  if (!row) {
    return Response.json({ code: 'not_found', message: 'Search not found.' }, { status: 404 });
  }
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

  const summary = {
    handle: row.query,
    checked: row.checked_count,
    total: row.checked_count,
    found: row.found_count,
    skipped: 0,
    failed: 0,
    filtered: 0,
    notFound: 0,
    elapsedMs: 0,
    depthReached: row.depth,
    byCategory,
    leads: { handles: [], emails: [] }
  };

  const filename = `athar-${row.query.replace(/[^\w.-]/g, '_')}-${searchId}.${format}`;
  const headers = {
    'Content-Type': CONTENT_TYPES[format],
    'Content-Disposition': `attachment; filename="${filename}"`
  };

  if (format === 'json') return new Response(exportJson(summary, hits), { headers });
  if (format === 'csv') return new Response(exportCsv(hits), { headers });
  return new Response(exportPdf(summary, hits), { headers });
}
