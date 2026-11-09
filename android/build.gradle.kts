// ShadowVision2 — root build script.
// Versions are pinned deliberately: AGP >= 8.5.1 + NDK r28 are required for
// 16 KB page-size alignment (Google Play requirement for anything targeting
// Android 15+; see docs/decisions-ar.md §7).
plugins {
    id("com.android.application") version "8.7.3" apply false
    id("org.jetbrains.kotlin.android") version "2.1.0" apply false
}
