import type { Metadata } from 'next';
import { cookies, headers } from 'next/headers';
import './globals.css';
import { resolveLocale, dict, type Locale } from '@/lib/i18n';
import { SiteNav } from '@/components/SiteNav';
import { LocaleProvider } from '@/components/LocaleProvider';
import { corpusStats, loadSites } from '@/lib/engine/sites';

export const metadata: Metadata = {
  title: 'Athar — OSINT username, email and footprint search',
  description:
    'Athar is an original OSINT search engine. Start from one identifier and stream matching public profiles across hundreds of sources.',
  applicationName: 'Athar'
};

export default async function RootLayout({
  children
}: {
  children: React.ReactNode;
}) {
  const jar = await cookies();
  const head = await headers();
  const locale = resolveLocale(jar.get('athar_locale')?.value, head.get('accept-language'));
  const stats = corpusStats(loadSites());

  return (
    <html lang={locale} dir={locale === 'ar' ? 'rtl' : 'ltr'}>
      <body className="min-h-screen bg-ink-950 antialiased">
        <LocaleProvider initial={locale}>
          <SiteNav locale={locale} endpointCount={stats.total} />
          <main className="athar-grid-bg min-h-[70vh]">{children}</main>
          <footer className="border-t border-ink-800 py-8 text-xs text-slate-500">
            <div className="athar-shell space-y-2">
              <p>{dict.common.disclaimer[locale as Locale]}</p>
              <p>{dict.common.attribution[locale as Locale]}</p>
              <p className="text-slate-600">
                {stats.total} endpoints · {stats.byCategory.social} social ·{' '}
                {stats.byCategory.creator} creator · {stats.byCategory.developer} developer ·{' '}
                {stats.byCategory.gaming} gaming · {stats.byCategory.commerce} commerce ·{' '}
                {stats.byCategory.long_tail} long tail
              </p>
            </div>
          </footer>
        </LocaleProvider>
      </body>
    </html>
  );
}
