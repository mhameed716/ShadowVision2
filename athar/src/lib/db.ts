import { DatabaseSync } from 'node:sqlite';
import fs from 'node:fs';
import path from 'node:path';

export type Plan = 'free' | 'starter' | 'pro' | 'enterprise';

export interface User {
  id: number;
  email: string;
  password_hash: string;
  plan: Plan;
  created_at: string;
}

export interface SearchRow {
  id: number;
  user_id: number | null;
  anon_key: string | null;
  query: string;
  kind: 'username' | 'email';
  depth: number;
  created_at: string;
  status: 'running' | 'done' | 'error';
  checked_count: number;
  found_count: number;
  units: number;
  locale: string;
}

export interface FindingRow {
  id: number;
  search_id: number;
  site_name: string;
  site_url_main: string;
  result_url: string;
  category: string;
  score: number;
  tier: number;
  via: string | null;
  profile_json: string;
  created_at: string;
}

const DATA_DIR = path.join(process.cwd(), 'data');
const DB_PATH = path.join(DATA_DIR, 'athar.db');

let cached: DatabaseSync | null = null;

export function db(): DatabaseSync {
  if (cached) return cached;
  if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });
  const handle = new DatabaseSync(DB_PATH);
  handle.exec('PRAGMA journal_mode = WAL');
  handle.exec('PRAGMA foreign_keys = ON');
  migrate(handle);
  cached = handle;
  return handle;
}

function migrate(handle: DatabaseSync): void {
  handle.exec(`
    CREATE TABLE IF NOT EXISTS users (
      id            INTEGER PRIMARY KEY AUTOINCREMENT,
      email         TEXT NOT NULL UNIQUE,
      password_hash TEXT NOT NULL,
      plan          TEXT NOT NULL DEFAULT 'free',
      created_at    TEXT NOT NULL
    );

    CREATE TABLE IF NOT EXISTS sessions (
      id         TEXT PRIMARY KEY,
      user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      created_at TEXT NOT NULL,
      expires_at TEXT NOT NULL
    );

    -- Units consumed per ISO-ish week bucket. Resets Monday 00:00 UTC.
    CREATE TABLE IF NOT EXISTS weekly_usage (
      user_id  INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      week_key TEXT    NOT NULL,
      units    INTEGER NOT NULL DEFAULT 0,
      PRIMARY KEY (user_id, week_key)
    );

    -- Anonymous daily scan budget, keyed by a salted hash of the client IP.
    CREATE TABLE IF NOT EXISTS anon_usage (
      anon_key TEXT PRIMARY KEY,
      day_key  TEXT NOT NULL,
      scans    INTEGER NOT NULL DEFAULT 0
    );

    CREATE TABLE IF NOT EXISTS searches (
      id            INTEGER PRIMARY KEY AUTOINCREMENT,
      user_id       INTEGER REFERENCES users(id) ON DELETE CASCADE,
      anon_key      TEXT,
      query         TEXT NOT NULL,
      kind          TEXT NOT NULL DEFAULT 'username',
      depth         INTEGER NOT NULL DEFAULT 0,
      created_at    TEXT NOT NULL,
      status        TEXT NOT NULL DEFAULT 'running',
      checked_count INTEGER NOT NULL DEFAULT 0,
      found_count   INTEGER NOT NULL DEFAULT 0,
      units         INTEGER NOT NULL DEFAULT 1,
      locale        TEXT NOT NULL DEFAULT 'en'
    );

    CREATE TABLE IF NOT EXISTS findings (
      id           INTEGER PRIMARY KEY AUTOINCREMENT,
      search_id    INTEGER NOT NULL REFERENCES searches(id) ON DELETE CASCADE,
      site_name    TEXT NOT NULL,
      site_url_main TEXT NOT NULL,
      result_url   TEXT NOT NULL,
      category     TEXT NOT NULL DEFAULT 'long_tail',
      score        REAL NOT NULL DEFAULT 0,
      tier         INTEGER NOT NULL DEFAULT 3,
      via          TEXT,
      profile_json TEXT NOT NULL DEFAULT '{}'
    );

    CREATE INDEX IF NOT EXISTS idx_findings_search ON findings(search_id);
    CREATE INDEX IF NOT EXISTS idx_searches_user   ON searches(user_id, created_at);

    -- Local ledger of granted units so dev-mode top-ups are auditable.
    -- Grants are additive on top of the plan allowance and expire with the
    -- same weekly bucket, so they can never be spent twice.
    CREATE TABLE IF NOT EXISTS unit_grants (
      id         INTEGER PRIMARY KEY AUTOINCREMENT,
      user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      week_key   TEXT    NOT NULL,
      units      INTEGER NOT NULL,
      reason     TEXT NOT NULL,
      reference  TEXT,
      created_at TEXT NOT NULL
    );
  `);
}

/** ISO week bucket that rolls over on Monday 00:00 UTC, matching the pricing page. */
export function weekKey(now: Date = new Date()): string {
  const d = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate()));
  const dayNum = (d.getUTCDay() + 6) % 7; // Monday = 0
  d.setUTCDate(d.getUTCDate() - dayNum);
  return d.toISOString().slice(0, 10);
}

export function dayKey(now: Date = new Date()): string {
  return now.toISOString().slice(0, 10);
}
