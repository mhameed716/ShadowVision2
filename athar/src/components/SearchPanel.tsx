'use client';

import { useCallback, useEffect, useRef, useState } from 'react';
import { dict, fill, type Locale } from '@/lib/i18n';
import { CATEGORY_LABELS, type Category } from '@/lib/engine/catalog';
import { useLocale } from './LocaleProvider';
import { ResultCard } from './ResultCard';
import type { RankedHit } from '@/lib/engine/rank';
import type { ScanSummary } from '@/lib/engine/scan';

type Phase = 'idle' | 'running' | 'done' | 'error' | 'refused';

export interface Budget {
  authenticated: boolean;
  anonymous?: { limit: number; used: number; remaining: number };
  allowance?: { weeklyLimit: number | null; used: number; remaining: number | null; unlimited: boolean };
}

export function SearchPanel({
  endpointCount,
  budget,
  synthetic
}: {
  endpointCount: number;
  budget: Budget | null;
  synthetic: boolean;
}) {
  const { locale } = useLocale();
  const s = dict.search;
  const r = dict.results;

  const [query, setQuery] = useState('');
  const [depth, setDepth] = useState(0);
  const [phase, setPhase] = useState<Phase>('idle');
  const [checked, setChecked] = useState(0);
  const [elapsed, setElapsed] = useState(0);
  const [hits, setHits] = useState<RankedHit[]>([]);
  const [summary, setSummary] = useState<ScanSummary | null>(null);
  const [searchId, setSearchId] = useState<number | null>(null);
  const [notice, setNotice] = useState<string | null>(null);

  const abortRef = useRef<AbortController | null>(null);

  useEffect(() => () => abortRef.current?.abort(), []);

  const stop = useCallback(() => {
    abortRef.current?.abort();
    abortRef.current = null;
    setPhase((p) => (p === 'running' ? 'done' : p));
  }, []);

  const scan = useCallback(
    async (event: React.FormEvent) => {
      event.preventDefault();
      const handle = query.trim();
      if (!handle) return;

      abortRef.current?.abort();
      const controller = new AbortController();
      abortRef.current = controller;

      setPhase('running');
      setHits([]);
      setSummary(null);
      setChecked(0);
      setElapsed(0);
      setNotice(null);
      setSearchId(null);

      const params = new URLSearchParams({ q: handle, depth: String(depth) });
      let response: Response;
      try {
        response = await fetch(`/api/search?${params.toString()}`, {
          signal: controller.signal,
          headers: { Accept: 'text/event-stream' }
        });
      } catch {
        setPhase('idle');
        return;
      }

      if (!response.ok || !response.body) {
        const body = (await response.json().catch(() => null)) as { message?: string } | null;
        setNotice(body?.message ?? s.rateLimited[locale]);
        setPhase('refused');
        return;
      }

      const startedAt = performance.now();
      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let buffer = '';

      const handleEvent = (name: string, raw: string) => {
        let payload: Record<string, unknown>;
        try {
          payload = JSON.parse(raw) as Record<string, unknown>;
        } catch {
          return;
        }
        if (name === 'open') {
          setSearchId(typeof payload.searchId === 'number' ? payload.searchId : null);
        } else if (name === 'progress') {
          setChecked(Number(payload.checked) || 0);
          setElapsed(Math.round((Number(payload.elapsedMs) || 0) / 100) / 10);
        } else if (name === 'hit') {
          const hit = payload.hit as RankedHit;
          setHits((prev) => [...prev, hit].sort((a, b) => b.score - a.score));
        } else if (name === 'done') {
          setSummary(payload.summary as ScanSummary);
          setPhase('done');
        } else if (name === 'error') {
          setNotice(String(payload.message ?? 'scan_failed'));
          setPhase('error');
        }
      };

      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });
        const frames = buffer.split('\n\n');
        buffer = frames.pop() ?? '';
        for (const frame of frames) {
          const lines = frame.split('\n');
          const name = lines.find((l) => l.startsWith('event: '))?.slice(7) ?? 'message';
          const data = lines
            .filter((l) => l.startsWith('data: '))
            .map((l) => l.slice(6))
            .join('\n');
          if (data) handleEvent(name, data);
        }
      }

      setElapsed(Math.round((performance.now() - startedAt) / 100) / 10);
      setPhase((p) => (p === 'running' ? 'done' : p));
    },
    [query, depth, locale, s]
  );

  const running = phase === 'running';
  const percent = endpointCount > 0 ? Math.min(100, Math.round((checked / endpointCount) * 100)) : 0;

  const budgetLine = budget?.authenticated
    ? budget.allowance?.unlimited
      ? dict.pricing.unlimited[locale]
      : `${dict.search.unitsLeft[locale]}: ${budget?.allowance?.remaining ?? 0}`
    : fill(dict.search.budget[locale], {
        n: String(Math.max(0, (budget?.anonymous?.remaining ?? 5) - 0))
      });

  return (
    <section className="athar-shell py-10 sm:py-14">
      {synthetic && (
        <div
          role="alert"
          className="mb-6 rounded-xl border border-amber-500/50 bg-amber-500/10 p-4"
        >
          <p className="text-sm font-bold uppercase tracking-wide text-amber-300">
            {locale === 'ar' ? 'وضع العرض التجريبي' : 'Demo mode'}
          </p>
          <p className="mt-1 text-xs leading-relaxed text-amber-100/90">
            {dict.common.syntheticBanner[locale]}
          </p>
        </div>
      )}

      <h1 className="max-w-2xl text-3xl font-bold tracking-tight sm:text-4xl">
        {s.title[locale]}
      </h1>
      <p className="mt-3 max-w-2xl text-sm text-slate-400">{s.subtitle[locale]}</p>

      <form onSubmit={scan} className="mt-6 flex flex-col gap-3 sm:flex-row">
        <input
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder={s.placeholder[locale]}
          className="athar-input flex-1 !py-3 !text-base"
          autoComplete="off"
          spellCheck={false}
          aria-label={s.placeholder[locale]}
        />
        <div className="flex gap-2">
          <select
            value={depth}
            onChange={(e) => setDepth(Number(e.target.value))}
            className="athar-input !w-auto"
            aria-label={s.depth[locale]}
            title={s.depth[locale]}
          >
            <option value={0}>
              {s.depth[locale]}: {s.depthOff[locale]}
            </option>
            {[1, 2, 3, 4].map((d) => (
              <option key={d} value={d}>
                {s.depth[locale]} {d}
              </option>
            ))}
          </select>
          {running ? (
            <button type="button" onClick={stop} className="athar-btn-ghost !px-5">
              {s.stop[locale]}
            </button>
          ) : (
            <button type="submit" className="athar-btn !px-6" disabled={!query.trim()}>
              {s.scan[locale]}
            </button>
          )}
        </div>
      </form>

      <div className="mt-4 flex flex-wrap items-center gap-x-4 gap-y-1 text-xs text-slate-500">
        <span>
          {s.endpoints[locale]}: <strong className="text-slate-300">{endpointCount}</strong>
        </span>
        <span>
          {s.streaming[locale]}: <strong className="text-athar-400">{s.live[locale]}</strong>
        </span>
        <span>{budgetLine}</span>
      </div>

      {notice && (
        <p className="athar-card mt-4 !border-amber-500/40 !bg-amber-500/5 text-sm text-amber-200">
          {notice}
        </p>
      )}

      <div className="athar-card mt-4">
        <div className="flex items-center justify-between text-xs text-slate-400">
          <span>
            {phase === 'idle' ? s.ready[locale] : running ? s.progress[locale] : s.found[locale]}
            {phase !== 'idle' && (
              <>
                {' '}
                <strong className="text-slate-200">
                  {String(checked).padStart(3, '0')} / {endpointCount}
                </strong>
              </>
            )}
          </span>
          <span className="tabular-nums">
            {elapsed.toFixed(1)}s {s.elapsed[locale]} ·{' '}
            <strong className="text-athar-400">
              {String(hits.length).padStart(3, '0')}
            </strong>{' '}
            {s.found[locale]}
          </span>
        </div>
        <div className="mt-2 h-1.5 overflow-hidden rounded-full bg-ink-800">
          <div
            className="h-full rounded-full bg-athar-500 transition-[width] duration-200"
            style={{ width: `${phase === 'done' ? 100 : percent}%` }}
          />
        </div>
      </div>

      {hits.length > 0 && (
        <div className="mt-6 flex flex-wrap items-center justify-between gap-3">
          <h2 className="text-sm font-semibold uppercase tracking-wide text-slate-400">
            {r.title[locale]} · {hits.length}
          </h2>
          {searchId && phase === 'done' && (
            <div className="flex gap-2">
              <span className="text-xs text-slate-500">{r.export[locale]}:</span>
              {(['json', 'csv', 'pdf'] as const).map((format) => (
                <a
                  key={format}
                  href={`/api/export/${searchId}/${format}`}
                  className="athar-btn-ghost !px-2 !py-1 !text-xs uppercase"
                >
                  {format}
                </a>
              ))}
            </div>
          )}
        </div>
      )}

      <ul className="mt-3 space-y-2">
        {hits.map((hit) => (
          <ResultCard
            key={`${hit.siteName}-${hit.via ?? ''}`}
            hit={hit}
            locale={locale}
            synthetic={synthetic}
          />
        ))}
      </ul>

      {hits.length === 0 && phase === 'running' && (
        <p className="mt-6 text-sm text-slate-500">{s.noResults[locale]}</p>
      )}

      {summary && (
        <div className="athar-card mt-6 text-sm">
          <h3 className="mb-2 text-xs font-semibold uppercase tracking-wide text-slate-400">
            {r.leads[locale]}
          </h3>
          <div className="flex flex-wrap gap-3 text-xs text-slate-400">
            {Object.entries(summary.byCategory).map(([category, count]) => (
              <span key={category}>
                {CATEGORY_LABELS[category as Category]?.[locale] ?? category}:{' '}
                <strong className="text-slate-200">{count}</strong>
              </span>
            ))}
          </div>
          {summary.leads.handles.length > 0 && (
            <p className="mt-3 text-xs">
              <span className="text-slate-500">{r.handles[locale]}: </span>
              <span className="font-mono text-athar-400">
                {summary.leads.handles.join(', ')}
              </span>
            </p>
          )}
          {summary.leads.emails.length > 0 && (
            <p className="mt-1 text-xs">
              <span className="text-slate-500">{r.emails[locale]}: </span>
              <span className="font-mono text-athar-400">
                {summary.leads.emails.join(', ')}
              </span>
            </p>
          )}
          {summary.depthReached > 0 && (
            <p className="mt-3 text-xs text-slate-500">{r.deepSearchOn[locale]}</p>
          )}
        </div>
      )}
    </section>
  );
}
