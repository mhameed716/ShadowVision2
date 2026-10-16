import type { RankedHit } from './engine/rank';
import type { ScanSummary } from './engine/scan';
import { CATEGORY_LABELS, type Category } from './engine/sites';

/**
 * Evidence export. JSON and CSV carry every field the caller is entitled to;
 * the PDF is an evidence report.
 *
 * Known limitation: the PDF is rendered in Latin script only. Arabic requires
 * font subsetting and bidirectional shaping, which needs a real font engine —
 * a hand-rolled Type1 Helvetica writer cannot shape it. The UI and the JSON and
 * CSV exports are fully bilingual.
 */

export interface ExportRow {
  site: string;
  url: string;
  category: string;
  tier: number;
  score: number;
  displayName: string;
  bio: string;
  location: string;
  counts: string;
  links: string;
  via: string;
}

export function toRows(hits: RankedHit[]): ExportRow[] {
  return hits.map((hit) => ({
    site: hit.siteName,
    url: hit.resultUrl,
    category: hit.category,
    tier: hit.tier,
    score: hit.score,
    displayName: hit.profile.displayName ?? '',
    bio: hit.profile.bio ?? '',
    location: hit.profile.location ?? '',
    counts: Object.entries(hit.profile.counts ?? {})
      .map(([k, v]) => `${k}=${v}`)
      .join('; '),
    links: (hit.profile.links ?? []).join(' '),
    via: hit.via ?? ''
  }));
}

export function exportJson(
  summary: ScanSummary,
  hits: RankedHit[]
): string {
  return JSON.stringify(
    {
      generator: 'Athar',
      version: 1,
      generatedAt: new Date().toISOString(),
      summary,
      hits: hits.map((hit) => ({
        site: hit.siteName,
        siteUrl: hit.siteUrlMain,
        resultUrl: hit.resultUrl,
        category: hit.category,
        tier: hit.tier,
        score: hit.score,
        via: hit.via,
        profile: hit.profile
      }))
    },
    null,
    2
  );
}

function csvEscape(value: string): string {
  if (/[",\n\r]/.test(value)) return `"${value.replace(/"/g, '""')}"`;
  return value;
}

export function exportCsv(hits: RankedHit[]): string {
  const headers = [
    'site', 'url', 'category', 'tier', 'score',
    'displayName', 'bio', 'location', 'counts', 'links', 'via'
  ];
  const lines = [headers.join(',')];
  for (const row of toRows(hits)) {
    lines.push(
      [
        row.site, row.url, row.category, String(row.tier), String(row.score),
        row.displayName, row.bio, row.location, row.counts, row.links, row.via
      ]
        .map((v) => csvEscape(String(v ?? '').replace(/[\r\n]+/g, ' ')))
        .join(',')
    );
  }
  return lines.join('\r\n');
}

interface PdfLine {
  text: string;
  bold?: boolean;
  size?: number;
  gapAfter?: number;
}

function sanitize(value: string): string {
  // WinAnsiEncoding Type1 fonts: strip anything outside Latin-1.
  return value
    .replace(/[^\x20-\x7E\xA0-\xFF]/g, '?')
    .replace(/\\/g, '\\\\')
    .replace(/\(/g, '\\(')
    .replace(/\)/g, '\\)');
}

function wrap(text: string, max: number): string[] {
  const words = text.split(/\s+/);
  const lines: string[] = [];
  let current = '';
  for (const word of words) {
    if ((current + ' ' + word).trim().length > max) {
      if (current) lines.push(current.trim());
      current = word;
    } else {
      current = `${current} ${word}`;
    }
  }
  if (current.trim()) lines.push(current.trim());
  return lines.length > 0 ? lines : [''];
}

/** Minimal PDF 1.4 writer: Helvetica + Helvetica-Bold, multiple pages. */
export function exportPdf(
  summary: ScanSummary,
  hits: RankedHit[]
): Buffer {
  const lines: PdfLine[] = [
    { text: 'Athar — Evidence Report', bold: true, size: 18, gapAfter: 4 },
    { text: `Generated: ${new Date().toISOString()}`, size: 9, gapAfter: 2 },
    { text: `Query handle: ${summary.handle}`, size: 9, gapAfter: 2 },
    {
      text: `Checked ${summary.checked} of ${summary.total} endpoints · ${summary.found} matches · ${summary.elapsedMs} ms`,
      size: 9,
      gapAfter: 2
    },
    { text: `Deep search depth reached: ${summary.depthReached}`, size: 9, gapAfter: 10 },
    { text: 'Coverage by category', bold: true, size: 12, gapAfter: 4 }
  ];

  for (const [category, count] of Object.entries(summary.byCategory)) {
    const label = CATEGORY_LABELS[category as Category]?.en ?? category;
    lines.push({ text: `  ${label}: ${count}`, size: 10, gapAfter: 1 });
  }

  lines.push({ text: 'Matches', bold: true, size: 12, gapAfter: 4 });

  hits.forEach((hit, index) => {
    lines.push({
      text: `${index + 1}. ${hit.siteName}  [tier ${hit.tier} · score ${hit.score}]`,
      bold: true,
      size: 10,
      gapAfter: 1
    });
    lines.push({ text: `   ${hit.resultUrl}`, size: 8, gapAfter: 1 });
    if (hit.profile.displayName) {
      lines.push({ text: `   Name: ${hit.profile.displayName}`, size: 8, gapAfter: 1 });
    }
    if (hit.profile.bio) {
      for (const wrapped of wrap(hit.profile.bio, 92).slice(0, 2)) {
        lines.push({ text: `   Bio: ${wrapped}`, size: 8, gapAfter: 1 });
      }
    }
    const counts = Object.entries(hit.profile.counts ?? {});
    if (counts.length > 0) {
      lines.push({
        text: `   Counts: ${counts.map(([k, v]) => `${k}=${v}`).join(', ')}`,
        size: 8,
        gapAfter: 1
      });
    }
    if (hit.via) lines.push({ text: `   Found via handle: ${hit.via}`, size: 8, gapAfter: 1 });
    lines.push({ text: '', size: 8, gapAfter: 4 });
  });

  if (summary.leads.handles.length > 0) {
    lines.push({ text: 'Leads — handles', bold: true, size: 12, gapAfter: 3 });
    for (const wrapped of wrap(summary.leads.handles.join(', '), 90)) {
      lines.push({ text: `  ${wrapped}`, size: 9, gapAfter: 1 });
    }
  }
  if (summary.leads.emails.length > 0) {
    lines.push({ text: 'Leads — email addresses', bold: true, size: 12, gapAfter: 3 });
    for (const email of summary.leads.emails) {
      lines.push({ text: `  ${email}`, size: 9, gapAfter: 1 });
    }
  }

  lines.push({ text: '', gapAfter: 8 });
  lines.push({
    text: 'Not a consumer reporting agency. Not for FCRA-covered decisions.',
    size: 8,
    gapAfter: 1
  });
  lines.push({
    text: 'Endpoint corpus derived from the MIT-licensed Sherlock Project.',
    size: 8,
    gapAfter: 1
  });

  return renderPdf(lines);
}

function renderPdf(lines: PdfLine[]): Buffer {
  const PAGE_W = 595.28;
  const PAGE_H = 841.89;
  const MARGIN = 48;
  const USABLE_W = PAGE_W - MARGIN * 2;

  const pages: string[][] = [];
  let current: string[] = [];
  let y = PAGE_H - MARGIN;

  const flush = () => {
    pages.push(current);
    current = [];
    y = PAGE_H - MARGIN;
  };

  for (const line of lines) {
    const size = line.size ?? 10;
    const leading = size * 1.35;
    const maxChars = Math.floor(USABLE_W / (size * 0.5));
    const wrapped = line.text === '' ? [''] : wrap(line.text, Math.max(20, maxChars));
    for (const piece of wrapped) {
      if (y - leading < MARGIN) flush();
      const font = line.bold ? '/F2' : '/F1';
      current.push(
        `BT ${font} ${size} Tf 1 0 0 1 ${MARGIN} ${y.toFixed(2)} Tm (${sanitize(piece)}) Tj ET`
      );
      y -= leading;
    }
    y -= line.gapAfter ?? 0;
  }
  if (current.length > 0) pages.push(current);
  if (pages.length === 0) pages.push(['BT /F1 10 Tf 1 0 0 1 48 793 Tm () Tj ET']);

  const objects: string[] = [];
  const pageCount = pages.length;
  const firstPageObj = 4; // 1=Catalog, 2=Pages, 3=Font, 4..=pages, then contents
  const contentStart = firstPageObj + pageCount;

  objects[1] = `<< /Type /Catalog /Pages 2 0 R >>`;
  objects[2] = `<< /Type /Pages /Kids [${pages
    .map((_, i) => `${firstPageObj + i} 0 R`)
    .join(' ')}] /Count ${pageCount} >>`;
  objects[3] = `<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>`;

  pages.forEach((content, i) => {
    const contentObj = contentStart + i;
    objects[firstPageObj + i] =
      `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ${PAGE_W} ${PAGE_H}] ` +
      `/Resources << /Font << /F1 3 0 R /F2 ${3 + pageCount + pages.length + 1} 0 R >> >> ` +
      `/Contents ${contentObj} 0 R >>`;
    objects[contentObj] = `<< /Length ${Buffer.byteLength(content.join('\n'))} >>\nstream\n${content.join('\n')}\nendstream`;
  });

  const boldFontObj = 3 + pageCount + pages.length + 1;
  objects[boldFontObj] =
    `<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>`;

  const totalObjects = boldFontObj + 1;
  const chunks: Buffer[] = [];
  const offsets: number[] = new Array(totalObjects).fill(0);
  let position = 0;

  const push = (text: string | Buffer) => {
    const buf = typeof text === 'string' ? Buffer.from(text, 'latin1') : text;
    chunks.push(buf);
    position += buf.length;
  };

  push('%PDF-1.4\n%\xE2\xE3\xCF\xD3\n');
  for (let i = 1; i < totalObjects; i += 1) {
    offsets[i] = position;
    push(`${i} 0 obj\n${objects[i]}\nendobj\n`);
  }

  const xrefStart = position;
  let xref = `xref\n0 ${totalObjects}\n0000000000 65535 f \n`;
  for (let i = 1; i < totalObjects; i += 1) {
    xref += `${String(offsets[i]).padStart(10, '0')} 00000 n \n`;
  }
  push(xref);
  push(
    `trailer\n<< /Size ${totalObjects} /Root 1 0 R >>\nstartxref\n${xrefStart}\n%%EOF\n`
  );

  return Buffer.concat(chunks);
}
