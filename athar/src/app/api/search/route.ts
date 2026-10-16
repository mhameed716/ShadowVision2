import { NextRequest } from 'next/server';
import { db } from '@/lib/db';
import { currentUser, anonKey, clientIp } from '@/lib/auth';
import {
  allowanceFor,
  anonBudgetFor,
  consumeAnonScan,
  consumeUnits,
  costOf
} from '@/lib/credits';
import { runScan, type ScanEvent, type ScanSummary } from '@/lib/engine/scan';
import { rankHits, type RankedHit } from '@/lib/engine/rank';
import { offlineTransport } from '@/lib/engine/fixtures';
import { isValidEmail } from '@/lib/engine/checker';
import { scanEmail, handlesFromEmailResult } from '@/lib/engine/email';
import { resolveLocale } from '@/lib/i18n';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

interface BudgetDecision {
  ok: true;
  userId: number | null;
  anon: string | null;
  units: number;
}
interface BudgetRefusal {
  ok: false;
  status: number;
  code: 'invalid_query' | 'rate_limited' | 'no_units' | 'needs_account' | 'needs_upgrade';
  message: string;
}

/**
 * Deep search is an entitlement, not just a price: Free and Starter cannot run
 * it at all. Checked for accounts as well as anonymous callers.
 */
async function decideBudget(
  request: NextRequest,
  depth: number
): Promise<BudgetDecision | BudgetRefusal> {
  const units = costOf(depth);
  const user = await currentUser();

  // Athar is free. Deep search, every field and full records are available to
  // everyone, signed in or not, so there is no entitlement gate here.
  if (user) {
    const allowance = allowanceFor(user.id, user.plan);
    if (!allowance.unlimited && (allowance.remaining ?? 0) < units) {
      return {
        ok: false,
        status: 429,
        code: 'no_units',
        message: `Weekly units exhausted on the ${user.plan} plan. Resets Monday 00:00 UTC.`
      };
    }
    return { ok: true, userId: user.id, anon: null, units };
  }

  // The only remaining brake is a coarse anti-abuse limit, so one client cannot
  // hammer hundreds of upstream sites in a loop and get the host IP blocked.
  const key = anonKey(clientIp(request.headers));
  const budget = anonBudgetFor(key);
  if (budget.remaining < 1) {
    return {
      ok: false,
      status: 429,
      code: 'rate_limited',
      message: 'Daily scan limit reached. Create a free account for unlimited searching.'
    };
  }
  return { ok: true, userId: null, anon: key, units: 0 };
}

function ssePayload(event: ScanEvent | Record<string, unknown>, name: string): string {
  return `event: ${name}\ndata: ${JSON.stringify(event)}\n\n`;
}

export async function GET(request: NextRequest) {
  const query = request.nextUrl.searchParams.get('q')?.trim() ?? '';
  const kind = request.nextUrl.searchParams.get('kind') === 'email' ? 'email' : 'username';
  const depthRaw = Number(request.nextUrl.searchParams.get('depth') ?? '0');
  const depth = Number.isFinite(depthRaw) ? Math.max(0, Math.min(depthRaw, 4)) : 0;
  const locale = resolveLocale(
    request.cookies.get('athar_locale')?.value,
    request.headers.get('accept-language')
  );

  if (!query || (kind === 'email' && !isValidEmail(query))) {
    return Response.json({ code: 'invalid_query', message: 'A valid handle or email is required.' }, { status: 400 });
  }

  const budget = await decideBudget(request, kind === 'email' ? 0 : depth);
  if (!budget.ok) {
    return Response.json({ code: budget.code, message: budget.message }, { status: budget.status });
  }

  const handle = db();
  const nowIso = new Date().toISOString();
  const searchRow = handle
    .prepare(
      `INSERT INTO searches (user_id, anon_key, query, kind, depth, created_at, status, units, locale)
       VALUES (?, ?, ?, ?, ?, ?, 'running', ?, ?)`
    )
    .run(
      budget.userId,
      budget.anon,
      query,
      kind,
      kind === 'email' ? 0 : depth,
      nowIso,
      budget.units,
      locale
    );
  const searchId = Number(searchRow.lastInsertRowid);

  if (budget.userId) consumeUnits(budget.userId, budget.units);
  if (budget.anon) consumeAnonScan(budget.anon);

  const encoder = new TextEncoder();
  const stream = new ReadableStream<Uint8Array>({
    async start(controller) {
      const send = (event: ScanEvent | Record<string, unknown>, name = 'message') => {
        controller.enqueue(encoder.encode(ssePayload(event, name)));
      };

      try {
        send({ searchId }, 'open');

        if (kind === 'email') {
          const result = await scanEmail(query, { fetchImpl: offlineTransport() });
          const leads = handlesFromEmailResult(result);
          const payload = {
            type: 'done',
            summary: {
              handle: query,
              checked: result.checked.length,
              total: result.checked.length + result.failed.length,
              found: result.hits.length,
              skipped: 0,
              failed: result.failed.length,
              filtered: 0,
              notFound: 0,
              elapsedMs: 0,
              depthReached: 0,
              byCategory: {},
              leads: { handles: leads, emails: [query] }
            } satisfies ScanSummary,
            email: result
          };
          handle
            .prepare(
              `UPDATE searches SET status='done', checked_count=?, found_count=? WHERE id=?`
            )
            .run(payload.summary.checked, payload.summary.found, searchId);
          send(payload, 'done');
          controller.close();
          return;
        }

        const hits: RankedHit[] = [];
        let finalSummary: ScanSummary | null = null;
        const maxRows = 400;

        for await (const event of runScan({
          query,
          depth,
          fetchImpl: offlineTransport()
        })) {
          if (event.type === 'hit') hits.push(event.hit);
          if (event.type === 'done') finalSummary = event.summary;
          send(event, event.type);
        }

        const ranked = rankHits(hits);
        const insert = handle.prepare(
          `INSERT INTO findings
             (search_id, site_name, site_url_main, result_url, category, score, tier, via, profile_json)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
        );
        for (const hit of ranked.slice(0, maxRows)) {
          insert.run(
            searchId,
            hit.siteName,
            hit.siteUrlMain,
            hit.resultUrl,
            hit.category,
            hit.score,
            hit.tier,
            hit.via,
            JSON.stringify(hit.profile)
          );
        }

        const checked = finalSummary?.checked ?? 0;
        handle
          .prepare(
            `UPDATE searches SET status='done', checked_count=?, found_count=? WHERE id=?`
          )
          .run(checked, ranked.length, searchId);

        controller.close();
      } catch (error) {
        const message = error instanceof Error ? error.message : 'scan_failed';
        handle.prepare(`UPDATE searches SET status='error' WHERE id=?`).run(searchId);
        send({ type: 'error', message }, 'error');
        controller.close();
      }
    }
  });

  return new Response(stream, {
    headers: {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache, no-transform',
      Connection: 'keep-alive',
      'X-Accel-Buffering': 'no',
      'X-Athar-Plan': budget.userId ? 'account' : 'anonymous'
    }
  });
}
