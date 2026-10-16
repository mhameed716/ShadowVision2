import Link from 'next/link';
import { currentUser } from '@/lib/auth';
import { dict, type Locale } from '@/lib/i18n';
import { LangToggle } from './LangToggle';

export async function SiteNav({
  locale,
  endpointCount
}: {
  locale: Locale;
  endpointCount: number;
}) {
  const user = await currentUser();
  const nav = dict.nav;

  return (
    <header className="sticky top-0 z-40 border-b border-ink-800 bg-ink-950/85 backdrop-blur">
      <div className="athar-shell flex h-14 items-center justify-between gap-4">
        <Link href="/" className="flex items-center gap-2">
          <span className="grid h-7 w-7 place-items-center rounded-md bg-athar-500 font-bold text-ink-950">
            {locale === 'ar' ? 'أ' : 'A'}
          </span>
          <span className="text-sm font-semibold tracking-tight">
            {dict.brand[locale]}
            <span className="ms-2 hidden text-[10px] font-normal text-slate-500 sm:inline">
              {endpointCount} endpoints
            </span>
          </span>
        </Link>

        <nav className="flex items-center gap-1 sm:gap-2">
          <Link href="/pricing" className="athar-btn-ghost !px-3 !py-1.5 !text-xs">
            {nav.pricing[locale]}
          </Link>
          <LangToggle />
          {user ? (
            <>
              <Link href="/dashboard" className="athar-btn-ghost !px-3 !py-1.5 !text-xs">
                {nav.dashboard[locale]}
              </Link>
              <form action="/api/auth/logout" method="post">
                <button type="submit" className="athar-btn-ghost !px-3 !py-1.5 !text-xs">
                  {nav.signOut[locale]}
                </button>
              </form>
            </>
          ) : (
            <>
              <Link href="/login" className="athar-btn-ghost !px-3 !py-1.5 !text-xs">
                {nav.signIn[locale]}
              </Link>
              <Link href="/signup" className="athar-btn !px-3 !py-1.5 !text-xs">
                {nav.signUp[locale]}
              </Link>
            </>
          )}
        </nav>
      </div>
    </header>
  );
}
