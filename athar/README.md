# Athar — أثر

An original OSINT search platform. Start from one identifier — a handle, an
email address, or a pasted profile URL — and stream matching public profiles
across hundreds of sources.

Athar is written from scratch. Its branding, copy, ranking model, category
taxonomy and UI are original work; it is not a copy of any existing product.

---

## What it does

| Capability | Status |
| --- | --- |
| Username search across 462 endpoints | Working |
| Live streaming results (SSE) | Working |
| False-positive filtering and ranking | Working |
| Deep search, depths 1–4 (recursive handle discovery) | Working |
| Reverse email lookup | Partial — Gravatar only, see below |
| Accounts, weekly unit budgets, plan tiers | Working |
| JSON / CSV / PDF evidence export | Working |
| Arabic (RTL) + English with live toggle | Working |
| Stripe checkout | Wired; needs keys, see below |
| Breach records | Integration point only; needs a licensed key, see below |
| Phone number lookup | Not implemented — see below |

## Quick start

```bash
npm install
npm run build
npm start          # http://localhost:3000
```

Checks:

```bash
npm run typecheck  # tsc --noEmit
npm test           # 25 engine + accounting tests
```

## Offline mode

```bash
ATHAR_OFFLINE=1 npm start
```

This routes every probe through a fixture transport
(`src/lib/engine/fixtures.ts`) instead of the network. It exists because the
sandbox this was built in has outbound access restricted to package registries,
so live probes to third-party platforms cannot complete there.

Offline mode is a **test double, not a data source**. It exercises the real
pipeline — corpus iteration, response evaluation, profile extraction, ranking,
budget accounting, persistence and export — but the profiles it returns are
synthetic. Run without the flag for real results.

## Architecture

```
src/lib/engine/
  catalog.ts    pure corpus metadata (client-safe, no Node builtins)
  sites.ts      loads data/sites.json (server-side)
  checker.ts    handle normalization, probing, evaluation, profile extraction
  scan.ts       orchestration + concurrency pool + async-generator event stream
  rank.ts       scoring and ordering
  extract.ts    deep-search lead extraction from bios and link pages
  email.ts      reverse email lookup
src/lib/
  db.ts         node:sqlite schema and week bucketing
  auth.ts       scrypt hashing, cookie sessions
  credits.ts    plan table, unit costs, allowances, grants
  export.ts     JSON / CSV / hand-rolled PDF writer
  hibp.ts       Have I Been Pwned integration
  i18n.ts       AR/EN dictionary
src/app/api/    search (SSE), auth, searches, export, checkout, breach
```

### How a hit is decided

Each corpus entry declares how to tell a claimed handle from a missing one:

- `status_code` — found unless the status is in `errorCode`, and never found on any 4xx.
- `message` — found unless one of `errorMsg` appears in the body.
- `response_url` — found unless the final URL collapses to the site's bare error URL.

The extra `status < 400` guard on `status_code` sites is deliberate: without it
a site that declares `errorCode: [403]` would report a rate-limited 404 as a
match.

### Filtering and ranking

Two filters run before anything reaches the user:

- **Soft-404 titles.** Sources often answer 200 and still render a not-found
  page. Only the title fields are matched, never the bio — a real person may
  legitimately write "404" about themselves.
- **Brand echoes and empty shells.** A source reflecting its own brand account,
  or one that merely repeats the searched handle with no data.

Remaining hits are scored by platform tier, log-scaled audience size, and how
much public data the profile carries.

### Unit model

| Action | Units |
| --- | --- |
| Search | 1 |
| Breach reveal | 5 |
| Deep search, depth 1 / 2 / 3 / 4 | 3 / 6 / 12 / 24 |

| Plan | Weekly units | Breach records | Deep search |
| --- | --- | --- | --- |
| Free | 20 | counts only | no |
| Starter | 50 | partial | no |
| Pro | 500 | full | depths 1–4 |
| Enterprise | unlimited | full | depths 1–4 |

Allowances reset Monday 00:00 UTC and do not roll over. Anonymous visitors get
5 masked scans per day, keyed by a salted hash of the client IP — the raw IP is
never stored.

Accounting invariant: `weekly_usage` is a pure consumption counter and is only
ever incremented. Grants live in their own ledger and are summed into the
allowance, so `used` can never go negative and `remaining` can never exceed the
published plan limit.

## Environment variables

| Variable | Purpose |
| --- | --- |
| `ATHAR_OFFLINE` | `1` enables the fixture transport |
| `ATHAR_ANON_SALT` | HMAC salt for anonymous rate-limit keys |
| `HIBP_API_KEY` | Have I Been Pwned v3 key, for breach lookups |
| `STRIPE_SECRET_KEY` | Enables real checkout |
| `STRIPE_PRICE_STARTER` / `STRIPE_PRICE_PRO` | Stripe price ids |

## Honest limitations

**Breach records are not included and cannot be.** Breach corpora sit behind
commercial contracts. `/api/breach` integrates the official Have I Been Pwned
API and requires the operator's own paid key. Without one it returns
`configured: false` with a remediation message rather than an empty result that
would read as "no breaches found". A missing engine must never look like a
clean scan.

**Reverse email lookup is thin.** There is no large open dataset of "is this
address registered here" probes — each source needs its own module. Only
Gravatar is implemented (public, keyless, addressed by the MD5 of the
lowercased address). `email.ts` is structured to be extended.

**Phone lookup is not implemented.** It depends almost entirely on commercial
data sources; there is no strong open substitute.

**The PDF export is Latin-script only.** Arabic needs font subsetting and
bidirectional shaping, which a hand-rolled Type1 Helvetica writer cannot do.
The UI and the JSON and CSV exports are fully bilingual.

**482 sites is not 820+.** The corpus is the Sherlock Project's current list,
MIT licensed (see `data/SITES-LICENSE`). Larger open lists exist and the loader
takes any file in the same shape.

## Responsible use

Athar queries public sources and returns what those sources publish. It is not
a consumer reporting agency and must not be used for FCRA-covered decisions.
Whoever operates it is responsible for complying with the laws that apply to
them and to the people whose data is returned.

## Attribution

The endpoint corpus is derived from the
[Sherlock Project](https://github.com/sherlock-project/sherlock), MIT licensed.
A copy of that license is at `data/SITES-LICENSE`.
