'use client';

import { useState } from 'react';
import { useRouter } from 'next/navigation';
import { useLocale } from './LocaleProvider';
import { dict } from '@/lib/i18n';

export function AuthForm({ mode }: { mode: 'login' | 'signup' }) {
  const { locale } = useLocale();
  const a = dict.auth;
  const router = useRouter();
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    setBusy(true);
    setError(null);
    const response = await fetch(`/api/auth/${mode === 'login' ? 'login' : 'register'}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email, password })
    });
    const body = (await response.json().catch(() => null)) as { message?: string } | null;
    setBusy(false);
    if (!response.ok) {
      setError(body?.message ?? (mode === 'login' ? a.invalid[locale] : a.taken[locale]));
      return;
    }
    router.push('/dashboard');
    router.refresh();
  };

  return (
    <form onSubmit={submit} className="athar-card space-y-4">
      <div>
        <label className="athar-label" htmlFor="email">
          {a.email[locale]}
        </label>
        <input
          id="email"
          type="email"
          required
          value={email}
          onChange={(e) => setEmail(e.target.value)}
          className="athar-input"
          autoComplete="email"
          dir="ltr"
        />
      </div>
      <div>
        <label className="athar-label" htmlFor="password">
          {a.password[locale]}
        </label>
        <input
          id="password"
          type="password"
          required
          minLength={mode === 'signup' ? 8 : undefined}
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          className="athar-input"
          autoComplete={mode === 'login' ? 'current-password' : 'new-password'}
          dir="ltr"
        />
      </div>
      {error && <p className="text-sm text-red-400">{error}</p>}
      <button type="submit" className="athar-btn w-full" disabled={busy}>
        {busy ? dict.common.loading[locale] : mode === 'login' ? a.signIn[locale] : a.signUp[locale]}
      </button>
    </form>
  );
}
