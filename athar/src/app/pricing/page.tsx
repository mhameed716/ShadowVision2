import { cookies, headers } from 'next/headers';
import Link from 'next/link';
import { resolveLocale, dict, type Locale } from '@/lib/i18n';
import { corpusStats, loadSites } from '@/lib/engine/sites';
import { PROVIDERS } from '@/lib/engine/providers';

export const dynamic = 'force-dynamic';

export default async function PricingPage() {
  const jar = await cookies();
  const head = await headers();
  const locale = resolveLocale(jar.get('athar_locale')?.value, head.get('accept-language'));
  const stats = corpusStats(loadSites());
  const providerCount = PROVIDERS.length;

  const t = (en: string, ar: string) => (locale === 'ar' ? ar : en);

  const included = [
    {
      en: ['Unlimited searches', 'No weekly quota, no credits, no meter.'],
      ar: ['بحث غير محدود', 'بلا حصة أسبوعية ولا وحدات ولا عدّاد.']
    },
    {
      en: ['Deep search, depths 1–4', 'Recursive handle discovery from bios, link pages and structured cross-platform data.'],
      ar: ['البحث العميق ١–٤', 'تتبّع تكراري للمعرّفات من النبذات وصفحات الروابط والبيانات المهيكلة.']
    },
    {
      en: ['Every profile field', 'Name, bio, avatar, location, website, join date, user ID, verification state and audience counts.'],
      ar: ['كل حقول الملف الشخصي', 'الاسم والنبذة والصورة والموقع والموقع الإلكتروني وتاريخ الانضمام والمعرّف وحالة التوثيق وأعداد الجمهور.']
    },
    {
      en: ['JSON, CSV and PDF export', 'Every result, every field, no withholding.'],
      ar: ['تصدير JSON و CSV و PDF', 'كل نتيجة وكل حقل، بلا حجب.']
    },
    {
      en: ['Breach records, full', 'Requires the operator to supply a licensed provider key — see below.'],
      ar: ['سجلات الاختراق، كاملة', 'تتطلب من المشغّل توفير مفتاح مورّد مرخّص — انظر أدناه.']
    },
    {
      en: ['REST API', 'The same endpoints the dashboard uses.'],
      ar: ['واجهة REST', 'نفس نقاط النهاية التي تستخدمها لوحة التحكم.']
    }
  ];

  const limits = [
    {
      en: ['Breach corpora are not bundled', 'Breach data sits behind commercial contracts. Supply HIBP_API_KEY and the endpoint returns full records; without it the API reports not_configured rather than an empty result that would read as "no breaches found".'],
      ar: ['سجلات الاختراق غير مضمّنة', 'بيانات الاختراق خلف عقود تجارية. وفّر HIBP_APIKEY وستعيد النقطة السجلات كاملة؛ وبدونه تُبلغ الواجهة not_configured بدل نتيجة فارغة تُقرأ خطأً كـ«لا اختراقات».']
    },
    {
      en: ['Instagram, Facebook and Snapchat are limited', 'These platforms serve a login wall to anonymous requests. Athar will not report a match it cannot verify, so they are either probed through a public mirror or omitted rather than guessed at.'],
      ar: ['إنستقرام وفيسبوك وسناب شات محدودة', 'هذه المنصات تعرض جدار تسجيل دخول للطلبات المجهولة. أثر لن يُبلغ عن نتيجة لا يستطيع التحقق منها، لذا إما تُفحص عبر مرآة عامة أو تُستبعد بدل التخمين.']
    },
    {
      en: ['Phone lookup is not implemented', 'It depends almost entirely on commercial data sources; there is no strong open substitute.'],
      ar: ['البحث برقم الهاتف غير مُنفَّذ', 'يعتمد كليًا تقريبًا على مصادر بيانات تجارية؛ ولا يوجد بديل مفتوح قوي.']
    }
  ];

  return (
    <div className="athar-shell py-12">
      <p className="text-xs font-semibold uppercase tracking-wide text-athar-400">
        {t('Pricing', 'التسعير')}
      </p>
      <h1 className="mt-2 text-3xl font-bold tracking-tight sm:text-4xl">
        {t('Everything is free.', 'كل شيء مجاني.')}
      </h1>
      <p className="mt-3 max-w-2xl text-sm leading-relaxed text-slate-400">
        {t(
          'There are no plans, no credits and no paywall. Every capability is available to every visitor, signed in or not. Accounts exist only to keep a search history and exports — not to unlock anything.',
          'لا توجد باقات ولا وحدات ولا جدار دفع. كل قدرة متاحة لكل زائر، بحساب أو بدونه. الحسابات موجودة فقط لحفظ سجل البحث والتصدير — لا لفتح أي شيء.'
        )}
      </p>

      <div className="athar-card mt-8 !border-athar-500/40">
        <div className="grid gap-4 sm:grid-cols-3">
          <div>
            <p className="text-3xl font-bold text-athar-400">$0</p>
            <p className="mt-1 text-xs text-slate-400">{t('Forever', 'دائمًا')}</p>
          </div>
          <div>
            <p className="text-3xl font-bold text-slate-100">{stats.total}</p>
            <p className="mt-1 text-xs text-slate-400">{t('Endpoints per scan', 'مصدرًا لكل فحص')}</p>
          </div>
          <div>
            <p className="text-3xl font-bold text-slate-100">{providerCount}</p>
            <p className="mt-1 text-xs text-slate-400">
              {t('Structured API providers', 'مزوّد API منظّم')}
            </p>
          </div>
        </div>
        <Link href="/" className="athar-btn mt-6 w-full sm:w-auto">
          {t('Start searching', 'ابدأ البحث')}
        </Link>
      </div>

      <h2 className="mt-10 text-sm font-semibold uppercase tracking-wide text-slate-400">
        {t('Included for everyone', 'مضمّن للجميع')}
      </h2>
      <div className="mt-4 grid gap-3 sm:grid-cols-2">
        {included.map((item, index) => (
          <div key={index} className="athar-card">
            <p className="text-sm font-semibold text-slate-100">{item[locale][0]}</p>
            <p className="mt-1 text-xs leading-relaxed text-slate-400">{item[locale][1]}</p>
          </div>
        ))}
      </div>

      <h2 className="mt-10 text-sm font-semibold uppercase tracking-wide text-amber-300">
        {t('Honest limits', 'حدود صريحة')}
      </h2>
      <div className="mt-4 space-y-3">
        {limits.map((item, index) => (
          <div key={index} className="athar-card !border-amber-500/30">
            <p className="text-sm font-semibold text-amber-200">{item[locale][0]}</p>
            <p className="mt-1 text-xs leading-relaxed text-slate-400">{item[locale][1]}</p>
          </div>
        ))}
      </div>

      <p className="mt-8 text-xs text-slate-600">{dict.common.disclaimer[locale]}</p>
    </div>
  );
}
