import { db, weekKey, dayKey, type Plan } from './db';

/**
 * Mirrors the published pricing model: a search costs 1 unit, a breach reveal 5,
 * and a deep search costs 3/6/12/24 units by depth 1..4. Allowances reset
 * Monday 00:00 UTC and unused units do not roll over.
 */
export interface PlanDef {
  id: Plan;
  weeklyUnits: number | null; // null = unlimited
  priceMonthlyUsd: number | null;
  breachRecords: 'counts' | 'partial' | 'full';
  deepSearch: boolean;
  aiSummary: boolean;
  restApi: boolean;
}

export const PLANS: Record<Plan, PlanDef> = {
  free: {
    id: 'free',
    weeklyUnits: 20,
    priceMonthlyUsd: 0,
    breachRecords: 'counts',
    deepSearch: false,
    aiSummary: false,
    restApi: false
  },
  starter: {
    id: 'starter',
    weeklyUnits: 50,
    priceMonthlyUsd: 20,
    breachRecords: 'partial',
    deepSearch: false,
    aiSummary: true,
    restApi: false
  },
  pro: {
    id: 'pro',
    weeklyUnits: 500,
    priceMonthlyUsd: 49.99,
    breachRecords: 'full',
    deepSearch: true,
    aiSummary: true,
    restApi: false
  },
  enterprise: {
    id: 'enterprise',
    weeklyUnits: null,
    priceMonthlyUsd: null,
    breachRecords: 'full',
    deepSearch: true,
    aiSummary: true,
    restApi: true
  }
};

export const ANON_DAILY_SCANS = 5;
export const DEPTH_COST = [1, 3, 6, 12, 24]; // index = depth 0..4
export const BREACH_REVEAL_COST = 5;

export function costOf(depth: number): number {
  return DEPTH_COST[Math.max(0, Math.min(depth, DEPTH_COST.length - 1))];
}

/**
 * Deep search is an entitlement, not just a price: Free and Starter cannot run
 * it at all. Derived from the plan table so the two can never drift apart.
 */
export function authorizeDepth(plan: Plan, depth: number): boolean {
  return depth === 0 || PLANS[plan].deepSearch;
}

export interface Allowance {
  plan: Plan;
  /** Plan limit plus any units granted on top this week. null = unlimited. */
  weeklyLimit: number | null;
  used: number;
  remaining: number | null;
  unlimited: boolean;
  /** Units granted above the plan allowance in the current week bucket. */
  granted: number;
  resetsAt: string;
}

/** Units granted on top of the plan allowance within the current week bucket. */
export function grantedUnits(userId: number, key: string = weekKey()): number {
  const row = db()
    .prepare('SELECT COALESCE(SUM(units), 0) AS total FROM unit_grants WHERE user_id = ? AND week_key = ?')
    .get(userId, key) as { total: number | bigint } | undefined;
  return Number(row?.total ?? 0);
}

export function allowanceFor(userId: number, plan: Plan): Allowance {
  const def = PLANS[plan];
  const key = weekKey();
  const row = db()
    .prepare('SELECT units FROM weekly_usage WHERE user_id = ? AND week_key = ?')
    .get(userId, key) as { units: number } | undefined;
  // Usage is only ever incremented, so it can never go negative.
  const used = Math.max(0, row?.units ?? 0);
  const granted = grantedUnits(userId, key);
  const limit = def.weeklyUnits === null ? null : def.weeklyUnits + granted;
  return {
    plan,
    weeklyLimit: limit,
    used,
    remaining: limit === null ? null : Math.max(0, limit - used),
    unlimited: def.weeklyUnits === null,
    granted,
    resetsAt: nextReset().toISOString()
  };
}

/** Next Monday 00:00 UTC. */
export function nextReset(now: Date = new Date()): Date {
  const d = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate()));
  const dayNum = (d.getUTCDay() + 6) % 7; // Monday = 0
  d.setUTCDate(d.getUTCDate() - dayNum + 7);
  return d;
}

export function consumeUnits(userId: number, units: number): void {
  const key = weekKey();
  db()
    .prepare(
      `INSERT INTO weekly_usage (user_id, week_key, units) VALUES (?, ?, ?)
       ON CONFLICT(user_id, week_key) DO UPDATE SET units = units + excluded.units`
    )
    .run(userId, key, units);
}

/**
 * Add units on top of the plan allowance for the current week bucket.
 *
 * Grants are recorded in their own ledger and summed into the allowance, so
 * `weekly_usage` stays a pure consumption counter. Writing a negative delta
 * into usage instead would drive `used` below zero and let `remaining` exceed
 * the published plan limit.
 */
export function grantUnits(userId: number, units: number, reason: string, reference?: string): void {
  if (!Number.isInteger(units) || units <= 0) {
    throw new Error('a grant must be a positive whole number of units');
  }
  db()
    .prepare(
      `INSERT INTO unit_grants (user_id, week_key, units, reason, reference, created_at)
       VALUES (?, ?, ?, ?, ?, ?)`
    )
    .run(userId, weekKey(), units, reason, reference ?? null, new Date().toISOString());
}

export interface AnonBudget {
  limit: number;
  used: number;
  remaining: number;
}

export function anonBudgetFor(key: string): AnonBudget {
  const today = dayKey();
  const row = db().prepare('SELECT scans, day_key FROM anon_usage WHERE anon_key = ?').get(key) as
    | { scans: number; day_key: string }
    | undefined;
  const used = row && row.day_key === today ? row.scans : 0;
  return { limit: ANON_DAILY_SCANS, used, remaining: Math.max(0, ANON_DAILY_SCANS - used) };
}

export function consumeAnonScan(key: string): void {
  const today = dayKey();
  db()
    .prepare(
      `INSERT INTO anon_usage (anon_key, day_key, scans) VALUES (?, ?, 1)
       ON CONFLICT(anon_key) DO UPDATE SET
         scans = CASE WHEN excluded.day_key = anon_usage.day_key
                      THEN anon_usage.scans + 1 ELSE 1 END,
         day_key = excluded.day_key`
    )
    .run(key, today);
}
