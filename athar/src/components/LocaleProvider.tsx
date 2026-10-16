'use client';

import { createContext, useCallback, useContext, useState } from 'react';
import type { Locale } from '@/lib/i18n';

interface LocaleContextValue {
  locale: Locale;
  setLocale: (locale: Locale) => void;
}

const LocaleContext = createContext<LocaleContextValue>({
  locale: 'en',
  setLocale: () => {}
});

export function LocaleProvider({
  initial,
  children
}: {
  initial: Locale;
  children: React.ReactNode;
}) {
  const [locale, setLocaleState] = useState<Locale>(initial);

  const setLocale = useCallback((next: Locale) => {
    setLocaleState(next);
    document.cookie = `athar_locale=${next}; path=/; max-age=31536000; samesite=lax`;
    document.documentElement.lang = next;
    document.documentElement.dir = next === 'ar' ? 'rtl' : 'ltr';
  }, []);

  return (
    <LocaleContext.Provider value={{ locale, setLocale }}>{children}</LocaleContext.Provider>
  );
}

export function useLocale(): LocaleContextValue {
  return useContext(LocaleContext);
}
