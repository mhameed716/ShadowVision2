import Link from 'next/link';
import { cookies, headers } from 'next/headers';
import { redirect } from 'next/navigation';
import { resolveLocale, dict, type Locale } from '@/lib/i18n';
import { currentUser } from '@/lib/auth';
import { AuthForm } from '@/components/AuthForm';

export const dynamic = 'force-dynamic';

export default async function SignupPage() {
  const jar = await cookies();
  const head = await headers();
  const locale = resolveLocale(jar.get('athar_locale')?.value, head.get('accept-language'));
  if (await currentUser()) redirect('/dashboard');
  const a = dict.auth;

  return (
    <div className="athar-shell max-w-md py-16">
      <h1 className="mb-6 text-2xl font-bold tracking-tight">{a.signUp[locale]}</h1>
      <AuthForm mode="signup" />
      <p className="mt-4 text-center text-xs text-slate-500">
        {a.haveAccount[locale]}{' '}
        <Link href="/login" className="text-athar-400 hover:underline">
          {a.signIn[locale]}
        </Link>
      </p>
    </div>
  );
}
