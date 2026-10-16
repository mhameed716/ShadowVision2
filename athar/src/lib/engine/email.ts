import crypto from 'node:crypto';
import type { FetchLike } from './checker';
import { isValidEmail } from './checker';

/**
 * Reverse email lookup.
 *
 * Honest scope: unlike the username corpus, there is no large open dataset of
 * "is this address registered here" probes. Registration checks require a
 * per-site module, so this ships a small audited set and is built to be
 * extended. Results state which source produced them.
 */

export interface EmailHit {
  source: string;
  kind: 'registered' | 'account_exists' | 'unverified';
  detail: string;
  url?: string;
}

export interface EmailScanResult {
  address: string;
  hits: EmailHit[];
  checked: string[];
  failed: string[];
  gravatar?: {
    exists: boolean;
    displayName?: string;
    profileUrl?: string;
    location?: string;
    accounts: Array<{ url: string; username?: string }>;
  };
}

export function md5(value: string): string {
  return crypto.createHash('md5').update(value.trim().toLowerCase()).digest('hex');
}

/**
 * Gravatar is a public, keyless source: the profile is addressed by the MD5 of
 * the lowercased, trimmed address and returns 404 when no account exists.
 * Linked accounts often expose further handles.
 */
export async function checkGravatar(
  address: string,
  fetchImpl: FetchLike = fetch
): Promise<EmailScanResult['gravatar']> {
  const hash = md5(address);
  const url = `https://www.gravatar.com/${hash}.json`;
  try {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 10000);
    const response = await fetchImpl(url, {
      headers: { 'User-Agent': 'Athar/0.1 (+osint research)' },
      signal: controller.signal
    });
    clearTimeout(timer);
    if (response.status === 404) return { exists: false, accounts: [] };
    if (!response.ok) return undefined;
    const payload = (await response.json()) as {
      entry?: Array<{
        displayName?: string;
        profileUrl?: string;
        currentLocation?: string;
        accounts?: Array<{ url: string; username?: string }>;
      }>;
    };
    const entry = payload.entry?.[0];
    if (!entry) return { exists: false, accounts: [] };
    return {
      exists: true,
      displayName: entry.displayName,
      profileUrl: entry.profileUrl,
      location: entry.currentLocation,
      accounts: entry.accounts ?? []
    };
  } catch {
    return undefined;
  }
}

export interface EmailScanOptions {
  fetchImpl?: FetchLike;
}

export async function scanEmail(
  address: string,
  opts: EmailScanOptions = {}
): Promise<EmailScanResult> {
  const normalized = address.trim().toLowerCase();
  if (!isValidEmail(normalized)) {
    throw new Error('invalid_email');
  }
  const doFetch = opts.fetchImpl ?? fetch;
  const result: EmailScanResult = {
    address: normalized,
    hits: [],
    checked: ['gravatar'],
    failed: []
  };

  const gravatar = await checkGravatar(normalized, doFetch);
  if (gravatar === undefined) {
    result.failed.push('gravatar');
  } else {
    result.gravatar = gravatar;
    if (gravatar.exists) {
      result.hits.push({
        source: 'Gravatar',
        kind: 'account_exists',
        detail: gravatar.displayName
          ? `Profile registered to ${gravatar.displayName}`
          : 'A Gravatar profile is registered to this address',
        url: gravatar.profileUrl
      });
    }
  }

  return result;
}

/** Handles recovered from linked accounts become the next username search. */
export function handlesFromEmailResult(result: EmailScanResult): string[] {
  const out: string[] = [];
  for (const account of result.gravatar?.accounts ?? []) {
    if (account.username) out.push(account.username);
    else if (account.url) {
      const segments = account.url.split('/').filter(Boolean);
      const last = segments[segments.length - 1];
      if (last && /^[A-Za-z0-9_.-]{3,40}$/.test(last)) out.push(last);
    }
  }
  return [...new Set(out)].slice(0, 20);
}
