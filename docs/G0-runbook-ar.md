# G0 — دليل التشغيل (Runbook)

> **الحالة:** هيكل G0 مبني ومُتحقَّق منه على المضيف (33 اختباراً، ASan نظيف).
> **ما يتبقى لإغلاق البوابة:** تشغيل `sv_g0` بمسار LaMa الحقيقي على جهازين.

---

## 1. ما الذي بُني فعلاً

```
native/                        ← النواة، تُبنى على المضيف وعلى أندرويد من نفس المصادر
  include/sv/
    common.h                   ImageRGB/Mask/Status/CancelToken/Stats
    memory.h                   MemoryGovernor (سقف صارم + peak + VmHWM)
    kernels.h                  downsample/resize/dilate/feather/blend
    backend.h                  IInpaintBackend — الحد الوحيد الذي يعرف ORT
    inpaint_lama.h             المسار الكامل: crop → resize → run → blend
    synthetic_backend.h        بديل حتمي بلا أوزان (للتشغيل على CI)
    ort_backend.h              تنفيذ ORT/QNN (مبني فقط مع -DSV_WITH_ORT=ON)
    sv_c_api.h                 ABI ثابت (v1) — العقد الوحيد الذي تراه Kotlin
  src/…                        التنفيذ
  tests/sv_tests.cpp           33 تحقّقاً بلا إطار اختبار (يعمل على الجهاز أيضاً)
  tools/sv_g0.cpp              هارب البوابة: PASS/FAIL لكل معيار
  CMakeLists.txt

android/
  settings.gradle.kts  build.gradle.kts  gradle.properties
  app/build.gradle.kts         AGP 8.7.3، NDK r28، arm64 فقط، 16KB
  app/src/main/AndroidManifest.xml
  app/src/main/cpp/            CMakeLists + jni_bridge.cpp (RegisterNatives)
  app/src/main/java/…/SvEngine.kt      خيط واحد + إلغاء + تقرير
  app/src/main/java/…/MainActivity.kt  واجهة G0 (قياس فقط، ليست المنتج)

tools/verify_g0.sh             بوابة واحدة: بناء + اختبارات + فحص JNI + تقرير
tools/export_lama_onnx.py      تصدير LaMa إلى ONNX بالعقد الصحيح
tools/ci-shim/                 بديل jni.h/android/log.h لفحص الأنواع في CI
models/README.md               بيان النماذج + مخاطرة الترخيص R1
.github/workflows/g0.yml       CI يبني ويشغّل البوابة على كل دفعة
```

**قاعدة معمارية محققة:** النواة لا تحتوي على `#include <onnxruntime…>` إلا في ملف واحد، ولا تحتوي على أي كود أندرويد إلا في `sv_c_api.cpp`. لذلك تُبنى وتُختبر بالكامل على حاسوب بلا NDK — وهذا ما يسمح بالتكرار السريع قبل لمس الهاتف.

---

## 2. التشغيل على المضيف (30 ثانية)

```bash
./tools/verify_g0.sh          # بناء + 33 اختباراً + تقرير البوابة
./tools/verify_g0.sh --asan   # نفس الشيء تحت ASan/UBSan
```

المخرج المتوقع (آخر تقرير حقيقي على هذه الشجرة):

```
time (best)    : 38.9 ms
  - lama.crop_prepare   7.2 ms
  - lama.run            3.0 ms     ← synthetic؛ ليس قياس النموذج
  - lama.blend         28.7 ms
tracked peak   : 6.9 MB / budget 1200 MB
peak RSS       : 29.2 MB
[PASS] time_total_ms / tracked_peak_mb / process_rss_mb / governor_headroom_mb
=== G0: PASS ===
```

**تحذير تفسيري:** هذه الأرقام تقيس الهندسة (crop/resize/blend/budget)، **لا تقيس LaMa**. لا تُنشر ولا تُقارن بجهاز حقيقي. القياس الحقيقي هو §4.

---

## 3. خيارات الهارب

| الخيار | المعنى |
|---|---|
| `--backend synthetic\|lama` | `synthetic` = فحص السباكة، `lama` = القياس الحقيقي |
| `--model path.onnx` | مطلوب مع `lama` |
| `--work 512` | الضلع الأطول للقصاصة داخل الشبكة |
| `--repeat N` | أفضل زمن من N محاولات (الأولى تُهمَل حرارياً) |
| `--budget-mb N` | سقف الذاكرة المتتبَّعة (الافتراضي 1200) |
| `--rss-limit-mb N` | سقف بوابة عملية الذروة (الافتراضي 700) |
| `--emulate-ns F` | كلفة مصطنعة ns/بكسل لمحاكاة جهاز |
| `--fail-run` / `--fail-alloc` | حقن فشل لإثبات أن المسار لا ينهار |
| `--gen-test DIR` | توليد `test.ppm` + `test.pgm` (1920×1440، حواف حادة) |
| `--json` | تقرير للماكينة (يُرفع كأثر في CI) |

---

## 4. القياس الحقيقي: LaMa على المضيف أولاً

```bash
pip install torch onnx onnxruntime onnxconverter-common
pip install git+https://github.com/advimman/lama.git
python tools/export_lama_onnx.py --ckpt big-lama.pt --out models/lama_fp32.onnx
sha256sum models/lama_fp32.onnx        # ضعه في models/manifest.json

# ORT للمضيف (نزّل حزمة linux-x64 من إصدارات onnxruntime على GitHub)
cmake -S native -B native/build-ort -DSV_WITH_ORT=ON -DSV_ORT_ROOT=$PWD/onnxruntime
cmake --build native/build-ort -j
native/build-ort/sv_g0 --backend lama --model models/lama_fp32.onnx \
  --image native/build/test.ppm --mask native/build/test.pgm --repeat 3 --out lama.ppm
```

**بوابة القبول على المضيف:** لا معنى لرقم المضيف كزمن، لكن **جودة الحدود** تُحكم هنا أولاً: افتح `lama.ppm` وتحقق من عدم وجود هالة حول الماسك. إن ظهرت هالة، المشكلة في blend/feather لا في النموذج — وأصلحها قبل الهاتف.

---

## 5. القياس على الهاتف

```bash
# 1) البناء (يحتاج NDK r28 + JDK 17)
cd android && ./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk

# 2) البنية والقدرات — سجّلها قبل أي رقم
adb shell getprop ro.product.cpu.abi
adb shell getprop ro.soc.model
adb shell cat /proc/meminfo | head -3

# 3) نموذج على الجهاز (مسار التطبيق الخاص، لا sdcard عام)
adb push models/lama_fp32.onnx /data/local/tmp/lama_fp32.onnx
adb shell run-as ai.shadowvision.g0 cp /data/local/tmp/lama_fp32.onnx files/

# 4) شغّل من الواجهة (زر run gate) أو من هارب NDK على الجهاز
```

**ما يجب أن يُسجَّل من كل جهاز (وإلا فالقياس بلا معنى):**

| الحقل | من أين |
|---|---|
| `ro.soc.model` | شريحة دقيقة (تحدد مسار QNN) |
| `total_ms` لكل مرحلة | تقرير G0 |
| `peak_rss_mb` | VmHWM — لا VRAM |
| `thermal_status` قبل/بعد | PowerManager |
| عدد العمليات قبل الخنق | 10 عمليات متتالية |

**الهدف:** إزالة < 4 ثوانٍ على جهاز متوسط، ونقرة SAM < 150ms، وذروة < 700MB، و10 عمليات بلا انهيار.

---

## 6. مسار ORT/LaMa: ما هو مكتمل وما هو مؤجَّل بوعي

**مكتمل ومُختبر:** العقد (image+mask → image)، الأشكال الثابتة، إدارة الجلسات بسقف اثنتين، محاسبة الأوزان في الحاكم، fallback من QNN إلى CPU بلا انهيار، تحويل NCHW/NHWC و0..1.

**مؤجَّل صراحةً (لا نخدع فيه):**
1. **`npu_partition_ratio` ترجع `-1`** حتى يُبنى قارئ optrace. رقم مُلفَّق 100% أسوأ من `-1` صادق.
2. **مخزن القوالب (arena pinning)** غير منفّذ؛ ORT يدير ذاكرته.
3. **حلقة الخطوات (denoise loop) في C++** غير موجودة — لا لزوم لها مع LaMa (نداء واحد). تُضاف عند بوابة L2، وبنفس التصميم (CancelToken بين الخطوات).
4. **آلية `CancelToken` لا تُترجم بعد إلى `cancel_flag` في C API** — تُوصَّل عندما تصبح المرحلة طويلة فعلاً (L2). حالياً الإلغاء يُحترم بين المراحل فقط.

---

## 7. لوحة النتائج (تُملأ عند القياس)

| الجهاز | SoC | backend | time | peak RSS | thermal قبل→بعد | G0 |
|---|---|---|---|---|---|---|
| — | — | synthetic | 39 ms | 29 MB | — | PASS (سباكة فقط) |
| — | — | lama fp32 CPU | — | — | — | — |
| — | — | lama fp16 CPU | — | — | — | — |

**قاعدة:** لا يُملأ سطر بمقاسَّين مختلفين. العمود الفارغ يعني «لم يُقس»، لا «جيد».
