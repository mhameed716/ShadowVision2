'use client';

import { useLocale } from './LocaleProvider';

export function LangToggle() {
  const { locale, setLocale } = useLocale();
  return (
    <button
      type="button"
      onClick={() => setLocale(locale === 'en' ? 'ar' : 'en')}
      className="athar-btn-ghost !px-3 !py-1.5 !text-xs"
      aria-label="Switch language"
    >
      {locale === 'en' ? 'العربية' : 'English'}
    </button>
  );
}
