export type Locale = 'en' | 'ar';
export const LOCALES: Locale[] = ['en', 'ar'];

export function isLocale(value: string | null | undefined): value is Locale {
  return value === 'en' || value === 'ar';
}

export function resolveLocale(cookie: string | undefined, header: string | null): Locale {
  if (isLocale(cookie)) return cookie;
  if (header?.toLowerCase().startsWith('ar')) return 'ar';
  return 'en';
}

export const dict = {
  brand: { en: 'Athar', ar: 'أثر' },
  brandTagline: {
    en: 'A live OSINT search engine for usernames, emails and phone leads.',
    ar: 'محرّك بحث استخباري مباشر للمعرّفات والبريد الإلكتروني وأرقام الهاتف.'
  },
  nav: {
    search: { en: 'Search', ar: 'البحث' },
    tools: { en: 'Tools', ar: 'الأدوات' },
    pricing: { en: 'Pricing', ar: 'الأسعار' },
    dashboard: { en: 'Dashboard', ar: 'لوحة التحكم' },
    signIn: { en: 'Sign in', ar: 'تسجيل الدخول' },
    signUp: { en: 'Create account', ar: 'إنشاء حساب' },
    signOut: { en: 'Sign out', ar: 'تسجيل الخروج' }
  },
  search: {
    title: { en: 'Start from one identifier.', ar: 'ابدأ من معرّف واحد.' },
    subtitle: {
      en: 'Enter a handle, an @mention or a pasted profile URL. Matches stream in as each source answers.',
      ar: 'أدخل اسم مستخدم أو إشارة @ أو رابط ملف شخصي. تظهر النتائج فور ردّ كل مصدر.'
    },
    placeholder: { en: 'username, @handle or profile URL', ar: 'اسم مستخدم أو @handle أو رابط' },
    scan: { en: 'Scan', ar: 'فحص' },
    stop: { en: 'Stop', ar: 'إيقاف' },
    depth: { en: 'Deep search', ar: 'البحث العميق' },
    depthOff: { en: 'Off', ar: 'معطّل' },
    endpoints: { en: 'Supported endpoints', ar: 'المصادر المدعومة' },
    streaming: { en: 'Streaming results', ar: 'بث النتائج' },
    live: { en: 'Live', ar: 'مباشر' },
    progress: { en: 'Checked', ar: 'تم فحص' },
    found: { en: 'sites found', ar: 'موقعًا موجودًا' },
    elapsed: { en: 'elapsed', ar: 'منقضٍ' },
    ready: { en: 'Ready', ar: 'جاهز' },
    noResults: {
      en: 'No matches yet. Results appear as sources respond.',
      ar: 'لا نتائج بعد. تظهر النتائج فور ردّ المصادر.'
    },
    budget: {
      en: 'No account · {n} free scans per day · details masked',
      ar: 'بدون حساب · {n} عمليات فحص مجانية يوميًا · التفاصيل محجوبة'
    },
    budgetUsed: { en: 'Free scans left today', ar: 'الفحوصات المجانية المتبقية اليوم' },
    unitsLeft: { en: 'Search units left this week', ar: 'وحدات البحث المتبقية هذا الأسبوع' },
    needAccount: {
      en: 'Create a free account to search from the dashboard.',
      ar: 'أنشئ حسابًا مجانيًا للبحث من لوحة التحكم.'
    },
    rateLimited: {
      en: 'Daily limit reached. Create a free account to keep searching.',
      ar: 'وصلت للحد اليومي. أنشئ حسابًا مجانيًا لمتابعة البحث.'
    },
    noUnits: {
      en: 'Weekly units exhausted. Resets Monday 00:00 UTC.',
      ar: 'نفدت وحدات الأسبوع. تُعاد الاثنين 00:00 بتوقيت UTC.'
    },
    via: { en: 'via', ar: 'عبر' }
  },
  results: {
    title: { en: 'Matches', ar: 'النتائج' },
    rank: { en: 'Rank', ar: 'الترتيب' },
    profile: { en: 'Profile', ar: 'الملف الشخصي' },
    source: { en: 'Source', ar: 'المصدر' },
    open: { en: 'Open', ar: 'فتح' },
    followers: { en: 'followers', ar: 'متابع' },
    following: { en: 'following', ar: 'يتابع' },
    posts: { en: 'posts', ar: 'منشور' },
    subscribers: { en: 'subscribers', ar: 'مشترك' },
    export: { en: 'Export', ar: 'تصدير' },
    leads: { en: 'Leads found', ar: 'خيوط مكتشفة' },
    handles: { en: 'Handles', ar: 'معرّفات' },
    emails: { en: 'Emails', ar: 'بريد إلكتروني' },
    deepSearchOn: {
      en: 'Deep search follows handles found in bios and link pages.',
      ar: 'البحث العميق يتتبّع المعرّفات الموجودة في النبذات وصفحات الروابط.'
    }
  },
  pricing: {
    title: { en: 'Credits for search and enrichment.', ar: 'وحدات للبحث والإثراء.' },
    subtitle: {
      en: 'Start with free credits. Allowances reset Monday 00:00 UTC and do not roll over.',
      ar: 'ابدأ بوحدات مجانية. تُعاد الحصص الاثنين 00:00 UTC ولا تُرحَّل.'
    },
    monthly: { en: 'Monthly', ar: 'شهري' },
    yearly: { en: 'Yearly', ar: 'سنوي' },
    perWeek: { en: 'search units / week', ar: 'وحدة بحث / أسبوع' },
    unlimited: { en: 'Unlimited', ar: 'غير محدود' },
    choose: { en: 'Choose', ar: 'اختر' },
    contact: { en: 'Contact sales', ar: 'تواصل مع المبيعات' },
    startFree: { en: 'Start free', ar: 'ابدأ مجانًا' },
    unitCosts: { en: 'What a unit costs', ar: 'تكلفة الوحدة' },
    oneSearch: { en: 'Search — 1 unit', ar: 'بحث — وحدة واحدة' },
    reveal: { en: 'Breach reveal — 5 units', ar: 'كشف سجل اختراق — 5 وحدات' },
    deep: { en: 'Deep search — 3 / 6 / 12 / 24 units by depth', ar: 'بحث عميق — 3 / 6 / 12 / 24 وحدة حسب العمق' }
  },
  auth: {
    email: { en: 'Email', ar: 'البريد الإلكتروني' },
    password: { en: 'Password', ar: 'كلمة المرور' },
    signIn: { en: 'Sign in', ar: 'تسجيل الدخول' },
    signUp: { en: 'Create account', ar: 'إنشاء حساب' },
    noAccount: { en: "Don't have an account?", ar: 'ليس لديك حساب؟' },
    haveAccount: { en: 'Already registered?', ar: 'لديك حساب بالفعل؟' },
    invalid: { en: 'Invalid email or password.', ar: 'بريد إلكتروني أو كلمة مرور غير صحيحة.' },
    taken: { en: 'An account with that email already exists.', ar: 'يوجد حساب بهذا البريد بالفعل.' },
    weak: { en: 'Password must be at least 8 characters.', ar: 'كلمة المرور يجب ألا تقل عن 8 أحرف.' }
  },
  dashboard: {
    title: { en: 'Dashboard', ar: 'لوحة التحكم' },
    history: { en: 'Search history', ar: 'سجل البحث' },
    plan: { en: 'Plan', ar: 'الباقة' },
    usage: { en: 'Usage this week', ar: 'الاستهلاك هذا الأسبوع' },
    resets: { en: 'Resets', ar: 'تُعاد' },
    empty: { en: 'No searches yet.', ar: 'لا توجد عمليات بحث بعد.' },
    open: { en: 'Open', ar: 'فتح' },
    manage: { en: 'Manage plan', ar: 'إدارة الباقة' },
    upgrade: { en: 'Upgrade', ar: 'ترقية' },
    breach: { en: 'Breach exposure', ar: 'سجلات الاختراق' },
    notConfigured: {
      en: 'Not configured — breach corpora require a licensed provider key.',
      ar: 'غير مُعدّ — سجلات الاختراق تتطلب مفتاح مورّد مرخّص.'
    },
    configure: { en: 'Set HIBP_API_KEY to enable.', ar: 'اضبط HIBP_API_KEY للتفعيل.' }
  },
  common: {
    loading: { en: 'Loading', ar: 'جارٍ التحميل' },
    error: { en: 'Something went wrong.', ar: 'حدث خطأ ما.' },
    back: { en: 'Back', ar: 'رجوع' },
    close: { en: 'Close', ar: 'إغلاق' },
    copy: { en: 'Copy', ar: 'نسخ' },
    copied: { en: 'Copied', ar: 'تم النسخ' },
    language: { en: 'العربية', ar: 'English' },
    disclaimer: {
      en: 'Not a consumer reporting agency. Not for FCRA-covered decisions. You are responsible for lawful use.',
      ar: 'ليست وكالة تقارير استهلاكية. ليست لقرارات خاضعة لقانون FCRA. أنت مسؤول عن الاستخدام القانوني.'
    },
    syntheticBanner: {
      en: 'DEMO MODE — results below are synthetic fixtures, not real lookups. This sandbox has no outbound access to third-party platforms, so every probe is answered locally. The engine, ranking, filtering and billing are the real code paths; the profiles are not real people.',
      ar: 'وضع العرض — النتائج أدناه بيانات تجريبية مُصطنعة وليست فحوصات حقيقية. هذه البيئة لا تملك وصولًا خارجيًا إلى المنصات، لذا يُجاب كل فحص محليًا. المحرّك والترتيب والتصفية والمحاسبة هي الأكواد الحقيقية، أما الملفات الشخصية فليست لأشخاص حقيقيين.'
    },
    syntheticTag: {
      en: 'synthetic',
      ar: 'تجريبي'
    },
    attribution: {
      en: 'Endpoint corpus derived from the MIT-licensed Sherlock Project.',
      ar: 'قائمة المصادر مشتقّة من مشروع Sherlock المرخّص بـ MIT.'
    }
  }
} as const;

export type DictKey = keyof typeof dict;

export function t(locale: Locale, group: DictKey): Record<string, string> {
  const block = dict[group] as Record<string, { en: string; ar: string }>;
  const out: Record<string, string> = {};
  for (const [key, value] of Object.entries(block)) out[key] = value[locale];
  return out;
}

export function fill(template: string, vars: Record<string, string | number>): string {
  return template.replace(/\{(\w+)\}/g, (_, key) => String(vars[key] ?? ''));
}

export const PLAN_LABELS: Record<string, { en: string; ar: string }> = {
  free: { en: 'Free', ar: 'مجاني' },
  starter: { en: 'Starter', ar: 'المبتدئ' },
  pro: { en: 'Pro', ar: 'المحترف' },
  enterprise: { en: 'Enterprise', ar: 'المؤسسات' }
};
