# ShadowVision2 — دليل المنتج (v0.9 Studio)

> **الحالة:** الواجهة الأولى (User Studio) ولوحة المحرك (Engine Control) مبنيّتان،
> ومسار المنتج كامل من الاستيراد إلى التصدير، والكتالوج الموقّع يعمل من الطرفين
> (Python signing ↔ C++ verification).
> **ما ينقص للتشغيل الفعلي:** أوزان LaMa (تصدير) على جهاز — انظر §5.

---

## 1. ما بُني في هذه الجولة

### النواة C++ (جديدة منذ G0)

| الملف | الوظيفة |
|---|---|
| `native/src/sha256.cpp` | SHA-256 (FIPS) بلا تبعيات — أساس التحقق من الملفات |
| `native/src/manifest.cpp` | محلّل الكتالوج الموقّع + تحقق Ed25519 + رفض تجاوز المسارات |
| `native/third_party/ed25519/` | Ed25519 (Orson Peters، رخصة zlib) — 11 ملفاً |
| `native/src/segmenter.cpp` | `SegmentationSession`: encoder/decoder منفصلان + فلتر أكبر مكوّن |
| `native/src/ort_segmenter.cpp` | **MobileSAM الحقيقي عبر ONNX Runtime** (encoder + decoder) |
| `native/include/sv/sam_geometry.h` | هندسة SAM النقية: resize/pad/تحويل الإحداثيات/التطبيع |
| `native/include/sv/sv_c_api.h` | **ABI v2**: 13 دالة (كانت 6) |

### أندرويد — المنتج

| الملف | الوظيفة |
|---|---|
| `core/SvEngine.kt` | الجسر الوحيد: خيط واحد، إلغاء، تقارير، جلسة تقسيم، كتالوج |
| `core/ModelCatalog.kt` | الكتالوج الموقّع + التحقق من SHA-256 لكل ملف + منع المسارات |
| `core/MaskSurface.kt` | **سطح الرسم**: فرشاة، ماسك بنصف الدقة، تراجع محدود، تصدير 8-bit |
| `studio/StudioViewModel.kt` | آلة الحالة: استيراد → ماسك → إزالة → تصدير |
| `studio/StudioScreen.kt` | **الواجهة الأولى**: داكنة، Canvas، أدوات، تقدم حي، مقارنة قبل/بعد |
| `studio/ConsoleScreen.kt` | **الواجهة الثانية**: RAM لحظي، حراري، كتالوج، مسرّع، سجلات حية |
| `studio/StudioTheme.kt` | لوحة داكنة محايدة: اللون القوي الوحيد هو أحمر الماسك |
| `studio/StudioActivity.kt` | نشاط واحد، واجهتان، بلا تنقل سفلي |

### الأدوات

| الأداة | الوظيفة |
|---|---|
| `tools/sign_catalog.py` | توليد مفاتيح + توقيع + تحقق؛ Ed25519 من RFC 8032 بلا تبعيات |
| `tools/check_manifest.sh` | البوابة الأمنية: يفشل البناء إن ظهر `INTERNET` أو API شبكة |
| `tools/export_mobilesam_onnx.py` | تصدير MobileSAM إلى رسمين (encoder/decoder) بشكل ثابت |
| `tools/verify_g0.sh` | 7 بوابات بما فيها توقيع+تحقق كامل من الطرفين |

---

## 2. خط الأنابيب كما يعمل الآن

```
 PickVisualMedia
      │  (per-URI access, no storage permission)
      ▼
 decode + EXIF rotate (long side ≤ 3072)          [StudioViewModel]
      ▼
 SvEngine.prepareSegmentation(bitmap)             [encoder, ~0.4-1.2 s, background]
      ▼
 ┌───────────────┐         ┌──────────────────┐
 │ Smart tap     │         │ Brush / Eraser   │   [MaskSurface]
 │ ~30-80 ms     │         │ instant          │
 └───────┬───────┘         └────────┬─────────┘
         └───────────┬──────────────┘
                     ▼
          mask (8-bit, full res, feathered)
                     ▼
 SvEngine.inpaint(bitmap, mask)                  [native, one worker thread]
   crop(bbox+margin) → resize(512, /8) → dilate+feather
   → LaMa → upsample → masked blend + colour match
                     ▼
            Report{stages, peak RSS, thermal}   → Studio: before/after + export
                                                → Console: gauges + logs
```

**قرارات محفوظة في الكود وليست شعارات:**
- **الفرشاة هي الافتراضي**؛ التحديد الذكي يظهر فقط بعد جاهزية الـembedding، فلا يرى المستخدم انتظاراً.
- **الماسك بنصف الدقة** (1/4 الذاكرة) ويُكبَّر عند التصدير بتنعيم — لا فرق بصرياً، وفرق حقيقي في الذاكرة.
- **التراجع = لقطة عند حدّ الضربة** (٦ كحد أقصى)، لا نسخة صورة لكل حركة.
- **`params=none` لـLaMa** ⇒ الواجهة لا تعرض Steps/CFG/Seed إطلاقاً.

---

## 3. الأمان والخصوصية — كيف تُفرض فعلياً

| الادعاء | كيف يُفرض (لا كيف يُوعَد) |
|---|---|
| لا إنترنت | لا يوجد `<uses-permission INTERNET>` — التطبيق لا يستطيع فتح مقبس |
| لا GPS | لا أذونات موقع؛ ولا قراءة EXIF للموقع |
| لا تليمتري | لا SDK تحليلات، لا شبكة أصلاً |
| لا نماذج غير موثوقة | الكتالوج موقّع Ed25519؛ المفتاح الخاص خارج التطبيق؛ SHA-256 لكل ملف |
| لا تجاوز مسارات | `IsSafeRelativeFile` ترفض `..` و`/` و`:` |
| لا رفع تلقائي | `allowBackup=false` + قواعد استخراج بيانات تمنع النسخ السحابي |
| لا شبكة عند البناء | `tools/check_manifest.sh` يفشل CI عند أي إضافة |

**ما يُفحص في CI:** التوقيع في Python → التحقق في C++ على نفس البايتات → محاولة تعديل بايت واحد → يجب أن تفشل.

---

## 4. الواجهة الأولى (User Studio)

- **شريط علوي:** اسم المحرك المحمَّل + زر Console + Revert.
- **اللوحة:** السحب بإصبعين = تكبير (1×–6×)؛ نقرة سريعة = تحديد ذكي؛ سحب = فرشاة.
- **الأدوات:** Brush / Smart / Erase + تراجع/إعادة/مسح.
- **شريط نصف القطر:** 6–120 px مع معاينة المؤشر على اللوحة.
- **الزر الأساسي:** «Remove selection» — معطّل حتى يوجد ماسك فعلي.
- **التقدم الحي:** مرحلة مسماة (decoding / analysing / removing) + شريط تقدم.
- **بعد الإزالة:** زر Before/After + تصدير PNG + نسبة تغطية الماسك.

## 5. الواجهة الثانية (Engine Control)

- **موارد:** RSS الحقيقي (لا VRAM)، الحالة الحرارية، المسرّع الحالي.
- **كتالوج:** كل نموذج مع رخصته وحالته؛ زر Verify يعيد حساب SHA-256.
- **المسرّع:** CPU / QNN Auto فقط — والسبب مكتوب في الواجهة نفسها.
- **سجلات حية:** حلقة عرض بحد 300 سطر، ألوان للرفض/عدم التطابق.

---

## 6. ما ينقص لجلسة كاملة على الهاتف

```bash
# 1) كتالوج موقّع (يُشحن في assets)
python3 tools/sign_catalog.py sign --catalog models/catalog.txt \
        --key keys/dev.key --models models/
cp models/catalog.signed.txt android/app/src/main/assets/

# 2) أوزان LaMa الحقيقية
python3 tools/export_lama_onnx.py --ckpt big-lama.pt --out models/lama_fp32.onnx
adb push models/lama_fp32.onnx /data/local/tmp/
adb shell run-as ai.shadowvision cp /data/local/tmp/lama_fp32.onnx files/models/
#    ثم أعد التوقيع لأن sha256 تغيّر — وهذا مقصود

# 3) البناء
cd android && ./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### المحدد الحقيقي (MobileSAM)

```bash
python3 tools/export_mobilesam_onnx.py --ckpt weights/mobile_sam.pt --out-dir models
#   يطبع: parity: encoder max|diff|=… decoder max|diff|=…
#   إن تجاوز 1e-2 يتوقف بسكربت التصدير — لا تُشحن رسوم لا تطابق PyTorch
python3 tools/sign_catalog.py sign --catalog models/catalog.txt --key keys/dev.key --models models/
cp models/catalog.signed.txt android/app/src/main/assets/
```
عند التشغيل: إن تحقّق الكتالوج ووُجد الرسمان ⇒ `SvEngine.loadSegmenter(enc, dec)` يحمّل
MobileSAM. وإن فشل أحدهما يعود التطبيق إلى المحدد المدمج **ويقول ذلك في الشريط العلوي**،
ويظهر شعار «brush only» بدل زر تحديد صامت لا يعمل.

**هندسة SAM مكتوبة كدوال نقية** (`sam_geometry.h`) ومغطاة باختبارات: 1920×1080 → 1024×576
مع حشو 448 في الأسفل، و4032×3024 → 1024×768، والتحويل العكسي يعود داخل بكسل واحد.

### كيف نتأكد أن المحدد يعمل لا أنه يُترجم فقط

الترجمة (`-fsyntax-only`) لا تثبت شيئاً عن الأعطال الحقيقية، فكل أعطال هذا المسار
عند «الوصلات»: مصفوفة مربوطة بحجم مختلف عمّا يعلنه الرسم، ترتيب مدخلات خاطئ، قناع
يُقرأ قبل فحص الحالة، تحويل إحداثيات يُخطئ الحشو.

```bash
tools/make_test_graphs.py      # رسوم ONNX حقيقية بشكل MobileSAM لكنها "أوراكل":
                               # تُرجع قرصاً موجباً حول نقطة النقر في فضاء النموذج
tools/run_ort_integration.py   # 58 فحصاً عبر ORT الحقيقي
tools/mutation_test_segmenter.py  # يُدخل 9 أعطال شائعة ويشترط أن يسقط الاختبار
```

الملكية المُثبتة: **نقرة على بكسل (x,y) تُنتج قناعاً مركزه (x,y)**، والحشو لا
يولّد بكسل، والنقطة الشاردة الواحدة تُحذف بينما تبقى عند تعطيل `keep_largest`،
وتكرار الترميز يعطي نتيجة مطابقة بالبِت. الطفرات المقتولة: إسقاط القياس، تجاهل
الحشو أفقياً ورأسياً، قناع بطول خاطئ، `Ok` عند عدم توافق الأشكال، تعطيل فلتر أكبر
مكوّن، عكس العتبة، قياس أفقي مبني على الجانب الكامل.

**بدون أي من ذلك** يعمل التطبيق: محرك الإزالة المدمج (synthetic) + المحدد المدمج،
لأن الغرض من البناء الأول أن يكون قابلاً للتجربة لا أن يكون معطّلاً.

---

## 7. ما هو مدمج بـ«البديل» ويحتاج استبدالاً قبل الإصدار

| المكوّن | الحالة | ما يلزم |
|---|---|---|
| Segmenter | **ONNX الحقيقي منفَّذ** (`ort_segmenter.cpp`)؛ البديل المدمج يبقى للعمل بلا أوزان | تصدير الأوزان + `adb push` + إعادة التوقيع |
| Inpainter | built-in بلا أوزان | LaMa ONNX (المسار مبني ومختبر) |
| مفتاح التوقيع | `dev-0001` | مفتاح إصدار + تبديل `DEV_PUBLIC_KEY` |
| التوقيع في الإصدار | debug signing | keystore إصدار + Play App Signing |
| GPU | **مُسقَط بقرار** | لا شيء — هذا هو الصواب |

البنية تسمح بهذا الاستبدال **دون لمس الواجهة**: كل ما سبق خلف `ISegmenter*`
و`IInpaintBackend` و`ModelCatalog`.
