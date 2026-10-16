# Athar — أثر

An original OSINT search platform. Start from one identifier — a handle, an
email address, or a pasted profile URL — and stream matching public profiles
across hundreds of sources.

Athar is written from scratch. Its branding, copy, ranking model, category
taxonomy and UI are original work; it is not a copy of any existing product.

---

## What it does

**Everything in this project is free.** There are no plans, no units, no
credits and no paid tier. Deep search, full exports and the whole endpoint
corpus are available to every visitor, including anonymous ones.

| Capability | Status |
| --- | --- |
| Username search across 491 endpoints | Working |
| Live streaming results (SSE) | Working |
| Profile pulling — name, bio, join date, audience, links | Working, 12 sources |
| False-positive filtering and ranking | Working |
| Deep search, depths 1–4 (recursive handle discovery) | Working, free for all |
| Reverse email lookup | Partial — Gravatar only, see below |
| Accounts and search history | Working |
| JSON / CSV / PDF evidence export | Working |
| Arabic (RTL) + English with live toggle | Working |
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
npm test           # 29 engine + accounting tests
```

Node 22.5.0 or newer — the persistence layer uses the built-in `node:sqlite`.

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
data/
  sites.json         481 Sherlock entries (MIT)
  sites.athar.json   ~66 original entries, merged over the base by name
src/lib/engine/
  catalog.ts    pure corpus metadata (client-safe, no Node builtins)
  sites.ts      loads and merges both corpus files (server-side)
  checker.ts    handle normalization, probing, evaluation, profile extraction
  providers.ts  12 keyless JSON providers for profile pulling
  scan.ts       orchestration + concurrency pool + async-generator event stream
  rank.ts       scoring and ordering
  extract.ts    deep-search lead extraction from bios, links and structured fields
  email.ts      reverse email lookup
src/lib/
  db.ts         node:sqlite schema and week bucketing
  auth.ts       scrypt hashing, cookie sessions
  credits.ts    usage ledger (no longer gates anything)
  export.ts     JSON / CSV / hand-rolled PDF writer
  hibp.ts       Have I Been Pwned integration
  i18n.ts       AR/EN dictionary
src/app/api/    search (SSE), auth, searches, export, breach
```

Client components must import `catalog.ts`, never `sites.ts` — the latter reads
files with `node:fs` and breaks the browser bundle.

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
- **Anti-bot interstitials.** A CDN challenge page also answers 200, and its
  title is its own branding rather than a person. PyPI's "Client Challenge"
  page scored as a real match until this filter existed.
- **Brand echoes and empty shells.** A source reflecting its own brand account,
  or one that merely repeats the searched handle with no data.

Remaining hits are scored by platform tier, log-scaled audience size, and how
much public data the profile carries.

Every probe is accounted for exactly once. The summary reports `found`,
`notFound`, `failed`, `skipped` and `filtered`, and those five always sum to
`checked` — an invariant the test suite enforces.

### No unit model

An earlier revision of this project had four paid plans and a weekly unit
budget. That was removed: every capability is free and unlimited for every
visitor, anonymous or signed in.

`src/lib/credits.ts` still exists, because usage is still *recorded* — the
ledger is useful for spotting abuse and for the per-user search history. The
plan table now grants everything to everyone:

| Plan | Weekly units | Breach records | Deep search |
| --- | --- | --- | --- |
| Free | unlimited | full | depths 1–4 |
| Starter | unlimited | full | depths 1–4 |
| Pro | unlimited | full | depths 1–4 |
| Enterprise | unlimited | full | depths 1–4 |

The unit costs (`DEPTH_COST`, `BREACH_REVEAL_COST`) are retained as accounting
labels so historical rows still decode, but nothing is ever refused for lack of
credit. The only remaining limit is an anti-abuse ceiling on anonymous scans
(`ATHAR_ANON_DAILY_SCANS`, default 200/day), keyed by a salted hash of the
client IP — the raw IP is never stored.

Accounting invariant: `weekly_usage` is a pure consumption counter and is only
ever incremented. Grants live in their own ledger and are summed into the
allowance, so `used` can never go negative.

### Profile pulling

A hit on its own is weak evidence: "this URL answered 200" says little about
who owns it. Where a platform publishes a keyless JSON endpoint, Athar calls it
and merges the structured answer over the scraped HTML, because the API carries
identifiers, join dates and audience counts the page never exposes.

| Provider | What it adds |
| --- | --- |
| GitHub | name, bio, company, location, website, join date, user id, followers |
| GitLab, DEV Community, npm, Keybase | name, bio, links, join date |
| Reddit, Bluesky, Hacker News | name, karma, join date |
| Mastodon | display name, note, follower counts, custom fields, joined date |
| Steam | persona, profile state, real name, avatar, join date |
| Wikipedia | page extract, first publish date |
| Gravatar | display name, location, accounts (email lookup) |

`src/lib/engine/providers.ts` holds all twelve. Four of them — Mastodon, Steam,
Wikipedia, Gravatar — have no corpus entry at all and are queried directly as
standalone hits.

A provider that fails is never allowed to discard an HTML hit. `mergeProfiles`
prefers the structured value field by field, unions the link lists and lets the
API's audience counts win, so the result is at worst equal to the scrape.

Deep search also mines structured handles, not just bio text: GitHub's
`twitter_username`, Keybase proof URLs, and Mastodon profile fields are read
directly, which finds cross-platform links that a regular expression over a bio
misses.

## Environment variables

| Variable | Purpose |
| --- | --- |
| `ATHAR_OFFLINE` | `1` enables the fixture transport |
| `ATHAR_ANON_SALT` | HMAC salt for anonymous rate-limit keys |
| `ATHAR_ANON_DAILY_SCANS` | Anti-abuse ceiling for anonymous scans (default 200) |
| `HIBP_API_KEY` | Have I Been Pwned v3 key, for breach lookups |

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

**491 endpoints is not 820+.** The corpus is the Sherlock Project's current
list (MIT licensed, see `data/SITES-LICENSE`) merged with about sixty original
entries in `data/sites.athar.json`. Larger open lists exist and the loader takes
any file in the same shape.

**Some platforms cannot be probed anonymously, and no list entry changes that.**

- **Facebook** is not in the corpus. Sherlock removed it on 2023-02-04 because
  its login wall returns a page that looks like a profile for every query, so it
  produces nothing but false positives. Re-adding the entry would mean reporting
  a match for any username typed into the box.
- **Instagram and Twitter/X** are present, but their entries resolve through
  third-party mirrors (`imginn.com` and `nitter.privacydev.net`) because the
  platforms themselves require authentication. Those mirrors go down without
  notice; when one does, the site reports an error rather than a false match.
- **Snapchat** is present via its public profile page.

Athar will not authenticate to a platform on your behalf or scrape behind a
login. That is a terms-of-service line, not a missing feature.

**Live results were not verifiable end to end.** This project was built in a
sandbox whose outbound network is restricted to package registries, so almost
every probe fails there. The GitHub provider was verified against the real
`api.github.com`; the rest of the corpus and the other eleven providers are
verified by unit test and by offline-mode pipeline coverage only. Expect to
tune the corpus against the real network on your own machine.

## Responsible use

Athar queries public sources and returns what those sources publish. It is not
a consumer reporting agency and must not be used for FCRA-covered decisions.
Whoever operates it is responsible for complying with the laws that apply to
them and to the people whose data is returned.

## Attribution

The endpoint corpus is derived from the
[Sherlock Project](https://github.com/sherlock-project/sherlock), MIT licensed.
A copy of that license is at `data/SITES-LICENSE`.
