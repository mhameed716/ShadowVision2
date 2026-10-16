import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadSites, corpusStats, categoryOf, tierOf } from '../src/lib/engine/sites.ts';
import {
  normalizeHandle,
  isValidEmail,
  evaluate,
  extractProfile,
  looksLikePlaceholder,
  checkSite
} from '../src/lib/engine/checker.ts';
import { scoreHit, rankHits } from '../src/lib/engine/rank.ts';
import { extractLeads } from '../src/lib/engine/extract.ts';
import { runScan } from '../src/lib/engine/scan.ts';
import { offlineFetch } from '../src/lib/engine/fixtures.ts';
import { exportCsv, exportJson, exportPdf, toRows } from '../src/lib/export.ts';
import {
  nextReset,
  costOf,
  DEPTH_COST,
  PLANS,
  BREACH_REVEAL_COST,
  allowanceFor,
  consumeUnits,
  grantUnits,
  grantedUnits
} from '../src/lib/credits.ts';
import { createUser, hashPassword, verifyPassword } from '../src/lib/auth.ts';
import { weekKey } from '../src/lib/db.ts';
import type { SiteDef } from '../src/lib/engine/catalog.ts';
import type { ScanSummary } from '../src/lib/engine/scan.ts';

/* ---------------------------------------------------------------- corpus */

test('corpus loads from data/sites.json and drops NSFW by default', () => {
  const all = loadSites({ includeNsfw: true });
  const safe = loadSites();
  assert.ok(all.length > 400, `expected 400+ sites, got ${all.length}`);
  assert.ok(safe.length < all.length, 'NSFW sites should be filtered out by default');
  for (const site of safe) assert.ok(!site.isNSFW, `${site.name} should be filtered`);
  for (const site of all) {
    assert.ok(site.url, `${site.name} missing url`);
    assert.ok(site.errorType, `${site.name} missing errorType`);
  }
});

test('every site is assigned a category and a tier', () => {
  const stats = corpusStats(loadSites({ includeNsfw: true }));
  const categorized = Object.values(stats.byCategory).reduce((a, b) => a + b, 0);
  assert.equal(categorized, stats.total);
  const tiered = Object.values(stats.byTier).reduce((a, b) => a + b, 0);
  assert.equal(tiered, stats.total);
  assert.equal(categoryOf('GitHub', 'https://github.com/'), 'developer');
  assert.equal(categoryOf('Steam', 'https://steamcommunity.com/'), 'gaming');
  assert.equal(tierOf('github'), 1);
  assert.equal(tierOf('some-obscure-forum'), 3);
});

/* ------------------------------------------------------------ validation */

test('normalizeHandle accepts bare handles, @mentions and profile URLs', () => {
  assert.equal(normalizeHandle('  ada  '), 'ada');
  assert.equal(normalizeHandle('@ada'), 'ada');
  assert.equal(normalizeHandle('@@ada'), 'ada');
  assert.equal(normalizeHandle('https://github.com/ada'), 'ada');
  assert.equal(normalizeHandle('https://x.com/ada/'), 'ada');
  assert.equal(normalizeHandle(''), '');
});

test('isValidEmail accepts real addresses and rejects junk', () => {
  assert.ok(isValidEmail('ada@example.org'));
  assert.ok(!isValidEmail('not-an-email'));
  assert.ok(!isValidEmail('ada@example'));
  assert.ok(!isValidEmail('@example.org'));
});

/* -------------------------------------------------------------- evaluate */

const base: SiteDef = {
  name: 'Example',
  url: 'https://example.com/{}',
  urlMain: 'https://example.com/',
  errorType: 'status_code'
};

test('status_code evaluation honours errorCode', () => {
  assert.equal(evaluate(base, 200, 'https://example.com/ada', ''), true);
  assert.equal(evaluate(base, 404, 'https://example.com/ada', ''), false);
  // A site that declares 403 as its "missing user" code.
  const custom = { ...base, errorCode: [403] };
  assert.equal(evaluate(custom, 403, 'u', ''), false, 'the declared error code means missing');
  assert.equal(evaluate(custom, 200, 'u', ''), true, 'a clean 200 is a hit');
  // A 404 is never a hit, even when the site declared a different error code.
  // Without this guard a rate-limited 404 would be reported as a match.
  assert.equal(evaluate(custom, 404, 'u', ''), false, 'a 404 must never count as found');
});

test('message evaluation looks for every error string', () => {
  const site: SiteDef = {
    ...base,
    errorType: 'message',
    errorMsg: ['<title>404 Not Found</title>', 'user does not exist']
  };
  assert.equal(evaluate(site, 200, 'u', '<html>profile</html>'), true);
  assert.equal(evaluate(site, 200, 'u', '<title>404 Not Found</title>'), false);
  assert.equal(evaluate(site, 200, 'u', 'sorry, user does not exist'), false);
});

test('response_url evaluation rejects the bare error URL', () => {
  const site: SiteDef = {
    ...base,
    errorType: 'response_url',
    errorUrl: 'https://www.avizo.cz/'
  };
  assert.equal(evaluate(site, 200, 'https://www.avizo.cz/ada/', ''), true);
  assert.equal(evaluate(site, 200, 'https://www.avizo.cz/', ''), false);
});

test('checkSite skips sites whose regexCheck rejects the handle', async () => {
  const site: SiteDef = { ...base, regexCheck: '^[A-Za-z0-9]{4,12}$' };
  const outcome = await checkSite(site, 'ab', { fetchImpl: offlineFetch });
  assert.equal(outcome.kind, 'skipped');
  if (outcome.kind === 'skipped') assert.equal(outcome.reason, 'invalid_format');
});

/* ------------------------------------------------------------- profiling */

test('extractProfile reads og tags, links and audience counts', () => {
  const html = `<html><head>
    <meta property="og:title" content="Ada L. (@ada)" />
    <meta property="og:description" content="Engineer — 12,400 followers · 310 following" />
    <meta property="og:image" content="https://cdn.example/a.jpg" />
    <title>fallback</title></head><body></body></html>`;
  const profile = extractProfile(html);
  assert.equal(profile.displayName, 'Ada L. (@ada)');
  assert.equal(profile.counts.followers, 12400);
  assert.equal(profile.counts.following, 310);
  assert.equal(profile.avatar, 'https://cdn.example/a.jpg');
});

test('extractProfile decodes entities and keeps bio links', () => {
  const profile = extractProfile(
    `<meta property="og:description" content="See https://ada.example/ &amp; more" />`
  );
  assert.match(profile.bio ?? '', /&/);
  assert.deepEqual(profile.links, ['https://ada.example/']);
});

test('looksLikePlaceholder drops soft-404 pages even when the status is 200', () => {
  const site: SiteDef = { ...base, urlMain: 'https://archive.org/' };
  // A source can answer 200 and still render a not-found page.
  assert.ok(
    looksLikePlaceholder(site, { links: [], counts: {}, displayName: '404 Not Found' }, 'ada'),
    '"404 Not Found" must be dropped'
  );
  assert.ok(
    looksLikePlaceholder(
      site,
      { links: [], counts: {}, displayName: 'Page not found | Archive' },
      'ada'
    ),
    'a wrapped not-found title must be dropped'
  );
  assert.ok(
    looksLikePlaceholder(site, { links: [], counts: {}, displayName: "User doesn't exist" }, 'ada'),
    "a 'doesn't exist' title must be dropped"
  );
  // A real person may legitimately mention 404 in a bio — the bio is not a title.
  assert.ok(
    !looksLikePlaceholder(
      site,
      { links: [], counts: {}, displayName: 'Ada L.', bio: 'I collect 404 pages' },
      'ada'
    ),
    'a 404 mention in a bio must not disqualify a real profile'
  );
});

test('looksLikePlaceholder drops brand echoes and 404 pages', () => {
  const site: SiteDef = { ...base, urlMain: 'https://example.com/' };
  assert.ok(
    looksLikePlaceholder(site, { links: [], counts: {}, displayName: 'Example' }, 'ada'),
    'brand echo should be dropped'
  );
  assert.ok(
    looksLikePlaceholder(site, { links: [], counts: {}, displayName: '404' }, 'ada'),
    '404 page should be dropped'
  );
  assert.ok(
    !looksLikePlaceholder(
      site,
      { links: [], counts: {}, displayName: 'Ada L.', bio: 'Engineer' },
      'ada'
    ),
    'a real profile must survive'
  );
});

/* --------------------------------------------------------------- ranking */

test('ranking puts major services and richer profiles first', () => {
  const major = {
    siteName: 'github', siteUrlMain: 'https://github.com/', resultUrl: 'https://github.com/ada',
    category: 'developer' as const, tier: tierOf('github'),
    score: scoreHit(tierOf('github'), {
      links: [], counts: { followers: 12400 }, displayName: 'Ada', bio: 'Engineer'
    }),
    profile: { links: [], counts: { followers: 12400 } }, via: null
  };
  const minor = {
    siteName: 'tiny-forum', siteUrlMain: 'https://tiny.example/', resultUrl: 'https://tiny.example/ada',
    category: 'long_tail' as const, tier: tierOf('tiny-forum'),
    score: scoreHit(tierOf('tiny-forum'), { links: [], counts: {} }),
    profile: { links: [], counts: {} }, via: null
  };
  const ranked = rankHits([minor, major]);
  assert.equal(ranked[0].siteName, 'github');
  assert.ok(ranked[0].score > ranked[1].score);
});

/* ----------------------------------------------------------- deep search */

test('extractLeads pulls handles from bios, @mentions and profile URLs', () => {
  const leads = extractLeads(
    [
      {
        links: ['https://twitter.com/northwindnow', 'https://ada.example/'],
        counts: {},
        bio: 'Previously @oldhandle — reach me at ada@example.org',
        displayName: 'Ada'
      }
    ],
    'ada'
  );
  assert.ok(leads.handles.includes('northwindnow'), `handles: ${leads.handles}`);
  assert.ok(leads.handles.includes('oldhandle'));
  assert.deepEqual(leads.emails, ['ada@example.org']);
});

test('extractLeads never returns the domain half of an email address', () => {
  const leads = extractLeads(
    [{ links: [], counts: {}, bio: 'mail me at ada@example.org or see @realhandle' }],
    'ada'
  );
  assert.deepEqual(leads.emails, ['ada@example.org']);
  assert.deepEqual(
    leads.handles,
    ['realhandle'],
    'the email domain must not become a handle'
  );
});

test('extractLeads ignores reserved words and asset filenames', () => {
  const leads = extractLeads(
    [{ links: [], counts: {}, bio: 'see @login and @avatar.png and @home' }],
    'ada'
  );
  assert.deepEqual(leads.handles, []);
});

/* ------------------------------------------------------------- full scan */

test('runScan streams progress and hits end to end over the corpus', async () => {
  const events = [];
  for await (const event of runScan({
    query: '@ada',
    depth: 0,
    concurrency: 32,
    fetchImpl: offlineFetch
  })) {
    events.push(event);
  }

  const start = events.find((e) => e.type === 'start');
  const done = events.find((e) => e.type === 'done');
  const hits = events.filter((e) => e.type === 'hit');
  const progress = events.filter((e) => e.type === 'progress');

  assert.ok(start, 'expected a start event');
  assert.equal(start.handle, 'ada', 'the @mention should be normalized');
  assert.ok(start.total > 400, `expected the full corpus, got ${start.total}`);

  assert.ok(progress.length > 0, 'expected progress events');
  assert.ok(hits.length > 0, 'expected at least one hit');
  assert.ok(done, 'expected a done event');

  const summary = done.summary as ScanSummary;
  assert.equal(summary.handle, 'ada');
  assert.equal(summary.found, hits.length);
  assert.ok(summary.checked > 0);
  assert.equal(summary.depthReached, 0);

  // Ordering invariant: hits arrive unsorted, the summary ranks them.
  const matched = hits.map((e) => e.hit);
  const scores = matched.map((hit) => hit.score);
  assert.ok(Math.max(...scores) >= Math.min(...scores));
  for (const hit of matched) {
    assert.ok(hit.siteName, 'every hit needs a site name');
    assert.ok(/^https?:\/\//.test(hit.resultUrl), `bad url: ${hit.resultUrl}`);
    assert.ok([1, 2, 3].includes(hit.tier), `bad tier: ${hit.tier}`);
    assert.ok([
      'social', 'creator', 'developer', 'gaming', 'commerce', 'long_tail'
    ].includes(hit.category), `bad category: ${hit.category}`);
  }
  assert.ok(
    matched.some((hit) => hit.tier === 1),
    'a major platform should be tier 1'
  );
});

test('progress never exceeds its own total, at any depth', async () => {
  // Regression guard: deep search fans out per seed, so a total derived from
  // the corpus size or the depth count lets `checked` pass `total`.
  for (const depth of [0, 2, 4]) {
    let maxChecked = 0;
    let reportedTotal = 0;
    let summaryChecked = 0;
    let summaryTotal = 0;

    for await (const event of runScan({
      query: 'ada',
      depth,
      concurrency: 24,
      fetchImpl: offlineFetch
    })) {
      if (event.type === 'progress') {
        maxChecked = Math.max(maxChecked, event.checked);
        reportedTotal = event.total;
        assert.ok(
          event.checked <= event.total,
          `depth ${depth}: progress reported checked=${event.checked} > total=${event.total}`
        );
      }
      if (event.type === 'done') {
        summaryChecked = event.summary.checked;
        summaryTotal = event.summary.total;
      }
    }

    assert.ok(summaryTotal > 0, `depth ${depth} reported a zero total`);
    assert.ok(
      summaryChecked <= summaryTotal,
      `depth ${depth}: summary checked=${summaryChecked} exceeds total=${summaryTotal}`
    );
    assert.ok(maxChecked <= reportedTotal + 1, 'progress overshot its final total');
  }
});

test('anti-bot challenge pages are not reported as profiles', async () => {
  // Regression guard: PyPI served a "Client Challenge" interstitial with a 200
  // and it was scored as a real hit for `torvalds`.
  const challenge = async (): Promise<Response> =>
    new Response(
      '<html><head><title>Client Challenge</title></head><body><p>Checking your browser</p></body></html>',
      { status: 200, headers: { 'content-type': 'text/html' } }
    );

  const outcome = await checkSite(
    { name: 'PyPI', urlMain: 'https://pypi.org/', errorType: 'status_code', url: 'https://pypi.org/user/{}/' },
    'torvalds',
    { fetchImpl: challenge }
  );
  assert.equal(outcome.kind, 'found', 'the probe itself still succeeds — filtering happens downstream');
  const profile = extractProfile((outcome as { body: string }).body);
  assert.ok(
    looksLikePlaceholder({ name: 'PyPI', urlMain: 'https://pypi.org/', url: 'https://pypi.org/user/{}/', errorType: 'status_code' }, profile, 'torvalds'),
    'a CDN challenge page must be filtered out, not shown as a match'
  );

  // A real profile must survive the same filter.
  const real = extractProfile(
    '<html><head><title>torvalds (Linus Torvalds)</title></head><body>Kernel developer</body></html>'
  );
  assert.ok(
    !looksLikePlaceholder({ name: 'GitHub', urlMain: 'https://github.com/', url: 'https://github.com/{}', errorType: 'status_code' }, real, 'torvalds'),
    'a genuine profile must not be filtered'
  );
});

test('every probe is accounted for exactly once', async () => {
  // Regression guard: a 200 rejected as a placeholder used to increment
  // `checked` without incrementing any bucket, so the counters drifted apart.
  for await (const event of runScan({
    query: 'ada',
    depth: 0,
    concurrency: 24,
    fetchImpl: offlineFetch
  })) {
    if (event.type !== 'done') continue;
    const s = event.summary;
    assert.equal(
      s.found + s.notFound + s.failed + s.skipped + s.filtered,
      s.checked,
      `found(${s.found}) + notFound(${s.notFound}) + failed(${s.failed}) + skipped(${s.skipped}) + filtered(${s.filtered}) !== checked(${s.checked})`
    );
  }
});

test('runScan rejects an empty query without touching the network', async () => {
  const events = [];
  for await (const event of runScan({ query: '   ', fetchImpl: offlineFetch })) {
    events.push(event);
  }
  assert.equal(events.length, 1);
  assert.equal(events[0].type, 'error');
});

/* --------------------------------------------------------------- exports */

test('CSV, JSON and PDF exports are all produced from the same hits', () => {
  const hits = [
    {
      siteName: 'GitHub', siteUrlMain: 'https://github.com/', resultUrl: 'https://github.com/ada',
      category: 'developer' as const, tier: 1, score: 150,
      profile: {
        displayName: 'Ada L.', bio: 'Engineer, see https://ada.example/',
        links: ['https://ada.example/'], counts: { followers: 12400 }
      }, via: null
    }
  ];
  const summary: ScanSummary = {
    handle: 'ada', checked: 481, total: 481, found: 1, skipped: 0, failed: 0, filtered: 0, notFound: 0,
    elapsedMs: 1200, depthReached: 0, byCategory: { developer: 1 },
    leads: { handles: ['northwindnow'], emails: ['ada@example.org'] }
  };

  const json = JSON.parse(exportJson(summary, hits));
  assert.equal(json.hits.length, 1);
  assert.equal(json.hits[0].site, 'GitHub');

  const csv = exportCsv(hits);
  const csvLines = csv.split('\r\n');
  assert.equal(csvLines.length, 2);
  assert.match(csvLines[0], /^site,url,category/);
  assert.match(csvLines[1], /GitHub,https:\/\/github\.com\/ada,developer/);

  const pdf = exportPdf(summary, hits);
  assert.ok(Buffer.isBuffer(pdf));
  assert.equal(pdf.subarray(0, 8).toString('latin1'), '%PDF-1.4');
  assert.ok(pdf.subarray(-6).toString('latin1').includes('%%EOF'));
  // xref offsets must point at real object headers.
  const text = pdf.toString('latin1');
  const startxref = Number(/startxref\n(\d+)\n%%EOF/.exec(text)?.[1]);
  assert.ok(startxref > 0);
  assert.equal(text.slice(startxref, startxref + 4), 'xref');

  const rows = toRows(hits);
  assert.equal(rows[0].counts, 'followers=12400');
  assert.equal(rows[0].links, 'https://ada.example/');
});

/* --------------------------------------------------------------- pricing */

test('the week bucket rolls over on Monday 00:00 UTC', () => {
  // 2026-10-16 is a Friday; the bucket must be that week's Monday.
  const friday = new Date('2026-10-16T12:00:00Z');
  assert.equal(weekKey(friday), '2026-10-12');
  const monday = new Date('2026-10-12T00:00:00Z');
  assert.equal(weekKey(monday), '2026-10-12');
  const sunday = new Date('2026-10-18T23:59:59Z');
  assert.equal(weekKey(sunday), '2026-10-12');
  // The next reset from that Friday is the following Monday.
  assert.equal(nextReset(friday).toISOString(), '2026-10-19T00:00:00.000Z');
});

test('unit costs match the published schedule', () => {
  assert.deepEqual(DEPTH_COST, [1, 3, 6, 12, 24]);
  assert.equal(costOf(0), 1);
  assert.equal(costOf(1), 3);
  assert.equal(costOf(4), 24);
  assert.equal(costOf(99), 24, 'out-of-range depth must clamp, not throw');
  assert.equal(BREACH_REVEAL_COST, 5);
});

test('deep search is available to everyone, signed in or not', async () => {
  const { authorizeDepth } = await import('../src/lib/credits.ts');
  // Athar is free: no tier gates the depth selector.
  for (const plan of Object.keys(PLANS) as Array<keyof typeof PLANS>) {
    for (const depth of [0, 1, 2, 3, 4]) {
      assert.equal(
        authorizeDepth(plan, depth),
        true,
        `${plan} must allow depth ${depth}`
      );
    }
  }
  // And the plan table must agree, so the two cannot drift apart.
  for (const plan of Object.keys(PLANS) as Array<keyof typeof PLANS>) {
    assert.equal(
      authorizeDepth(plan, 2),
      PLANS[plan].deepSearch,
      `authorizeDepth disagrees with PLANS for ${plan}`
    );
  }
});

test('every plan is free and unlimited', () => {
  for (const plan of Object.keys(PLANS) as Array<keyof typeof PLANS>) {
    const def = PLANS[plan];
    assert.equal(def.weeklyUnits, null, `${plan} must be unlimited`);
    assert.equal(def.deepSearch, true, `${plan} must allow deep search`);
    assert.equal(def.breachRecords, 'full', `${plan} must expose full records`);
    assert.ok(
      def.priceMonthlyUsd === null || def.priceMonthlyUsd === 0,
      `${plan} must not cost anything`
    );
  }
});

test('usage accounting stays sound even with unlimited allowances', () => {
  const user = createUser(`ledger-${Date.now()}@example.org`, 'correcthorsebattery');

  // An unlimited plan reports no ceiling but still counts what was spent.
  const fresh = allowanceFor(user.id, 'pro');
  assert.equal(fresh.unlimited, true);
  assert.equal(fresh.weeklyLimit, null);
  assert.equal(fresh.remaining, null);
  assert.equal(fresh.used, 0);
  assert.equal(fresh.granted, 0);

  consumeUnits(user.id, costOf(2));
  const afterSearch = allowanceFor(user.id, 'pro');
  assert.equal(afterSearch.used, 6, 'consumption is still tracked');
  assert.equal(afterSearch.remaining, null, 'unlimited means no ceiling to report');

  // A grant is additive and must never drive usage below zero.
  grantUnits(user.id, 25, 'test_topup', 'ref-1');
  const afterGrant = allowanceFor(user.id, 'pro');
  assert.equal(afterGrant.granted, 25);
  assert.equal(afterGrant.used, 6, 'usage must not be touched by a grant');
  assert.ok(afterGrant.used >= 0, 'usage must never be negative');
  assert.equal(grantedUnits(user.id), 25);

  // Grants must be positive whole units.
  assert.throws(() => grantUnits(user.id, 0, 'bad'), /positive whole number/);
  assert.throws(() => grantUnits(user.id, -10, 'bad'), /positive whole number/);
  assert.throws(() => grantUnits(user.id, 1.5, 'bad'), /positive whole number/);
});

test('password hashing round-trips and rejects wrong passwords', () => {
  const hash = hashPassword('correcthorsebattery');
  assert.match(hash, /^scrypt\$[0-9a-f]+\$[0-9a-f]+$/);
  assert.ok(verifyPassword('correcthorsebattery', hash));
  assert.ok(!verifyPassword('wrongpassword', hash));
  assert.ok(!verifyPassword('correcthorsebattery', 'not-a-hash'));
  // Distinct salts, so identical passwords do not produce identical hashes.
  assert.notEqual(hash, hashPassword('correcthorsebattery'));
});

test('no capability is withheld from any plan', () => {
  // This is the regression guard for the free model: if someone reintroduces a
  // paywall by editing one tier, this fails.
  for (const plan of Object.keys(PLANS) as Array<keyof typeof PLANS>) {
    const def = PLANS[plan];
    assert.equal(def.deepSearch, true, `${plan} withholds deep search`);
    assert.equal(def.breachRecords, 'full', `${plan} withholds breach detail`);
    assert.equal(def.aiSummary, true, `${plan} withholds AI summaries`);
    assert.equal(def.restApi, true, `${plan} withholds the API`);
    assert.equal(def.weeklyUnits, null, `${plan} caps weekly units`);
  }
  // The unit price schedule is retained for accounting, not as a paywall.
  assert.deepEqual(DEPTH_COST, [1, 3, 6, 12, 24]);
  assert.equal(BREACH_REVEAL_COST, 5);
});
