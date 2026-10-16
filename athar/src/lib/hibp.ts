/**
 * Breach-exposure research.
 *
 * This is the one capability that cannot be built from open data: breach
 * corpora sit behind commercial contracts. Athar integrates the official
 * Have I Been Pwned API and requires the operator's own paid key. Without a
 * key the endpoint reports `not_configured` rather than returning nothing and
 * looking healthy — a missing engine must never read as a clean result.
 */

export interface BreachRecord {
  name: string;
  title: string;
  domain: string;
  breachDate: string;
  addedDate: string;
  dataClasses: string[];
  isVerified: boolean;
  isSensitive: boolean;
  /** Only populated for plans entitled to full records. */
  identifiers?: string[];
}

export type BreachStatus =
  | { configured: true; mode: 'counts' | 'partial' | 'full'; records: BreachRecord[] }
  | {
      configured: false;
      mode: 'counts' | 'partial' | 'full';
      reason: 'no_api_key';
      remediation: string;
    };

const HIBP_BASE = 'https://haveibeenpwned.com/api/v3';

export function hibpConfigured(): boolean {
  return Boolean(process.env.HIBP_API_KEY);
}

export interface BreachOptions {
  /** Which tier of detail the caller is entitled to. */
  mode: 'counts' | 'partial' | 'full';
  apiKey?: string;
  fetchImpl?: typeof fetch;
}

export async function lookupBreaches(
  account: string,
  opts: BreachOptions
): Promise<BreachStatus> {
  const apiKey = opts.apiKey ?? process.env.HIBP_API_KEY;
  if (!apiKey) {
    return {
      configured: false,
      mode: opts.mode,
      reason: 'no_api_key',
      remediation:
        'Set HIBP_API_KEY to a Have I Been Pwned v3 API key purchased from haveibeenpwned.com. Breach corpora are commercially licensed and cannot be sourced from open data.'
    };
  }

  const doFetch = opts.fetchImpl ?? fetch;
  const url = `${HIBP_BASE}/breachedaccount/${encodeURIComponent(account)}?truncateResponse=false`;
  const response = await doFetch(url, {
    headers: {
      'hibp-api-key': apiKey,
      'User-Agent': 'Athar/0.1 (osint research)'
    }
  });

  if (response.status === 404) {
    return { configured: true, mode: opts.mode, records: [] };
  }
  if (response.status === 429) {
    throw new Error('rate_limited');
  }
  if (!response.ok) {
    throw new Error(`hibp_${response.status}`);
  }

  const raw = (await response.json()) as Array<{
    Name: string;
    Title: string;
    Domain: string;
    BreachDate: string;
    AddedDate: string;
    DataClasses: string[];
    IsVerified: boolean;
    IsSensitive: boolean;
    PwnCount?: number;
  }>;

  const records: BreachRecord[] = raw
    // Sensitive breaches are withheld unless the caller is entitled to full detail.
    .filter((entry) => opts.mode === 'full' || !entry.IsSensitive)
    .map((entry) => {
      const record: BreachRecord = {
        name: entry.Name,
        title: entry.Title,
        domain: entry.Domain,
        breachDate: entry.BreachDate,
        addedDate: entry.AddedDate,
        dataClasses: entry.DataClasses,
        isVerified: entry.IsVerified,
        isSensitive: entry.IsSensitive
      };
      // Partial tier: source, date and categories only — no identifiers.
      if (opts.mode === 'full') {
        record.identifiers = [account];
      }
      if (opts.mode === 'counts') {
        record.title = '[redacted]';
        record.domain = '';
        record.dataClasses = [];
      }
      return record;
    });

  return { configured: true, mode: opts.mode, records };
}
