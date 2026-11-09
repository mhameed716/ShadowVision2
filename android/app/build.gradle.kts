// SPDX-License-Identifier: MIT
// ShadowVision2 — product app module.
//
// Architecture rule: the UI never touches native inference state directly. It
// calls SvEngine, which owns a single worker thread and a stable C ABI
// (native/include/sv/sv_c_api.h). Everything below that line is C++ and knows
// nothing about Android.
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose") version "2.1.0"
}

// Release signing. A debug-signed release build cannot be distributed: it
// expires, it is refused by some installers, and it cannot be updated by a
// properly signed build later. Credentials come from the environment (CI) or
// keystore.properties (local, git-ignored) — never from this file.
val keystoreProps = Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
fun secret(key: String, env: String): String? =
    keystoreProps.getProperty(key) ?: System.getenv(env)

val releaseStore: String? = secret("storeFile", "SV_KEYSTORE_FILE")
val releaseAlias: String? = secret("keyAlias", "SV_KEY_ALIAS")
val releaseStorePass: String? = secret("storePassword", "SV_KEYSTORE_PASSWORD")
val releaseKeyPass: String? = secret("keyPassword", "SV_KEY_PASSWORD")
val hasReleaseKey = listOf(releaseStore, releaseAlias, releaseStorePass, releaseKeyPass)
    .all { !it.isNullOrBlank() }

// NDK / ORT paths can be supplied without editing this file:
//   local.properties:  sv.ortRoot=/path/to/onnxruntime-android
// or Gradle property:   -Psv.ortRoot=...
val localProps = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
val ortRoot: String? = (project.findProperty("sv.ortRoot") as String?) ?: localProps.getProperty("sv.ortRoot")

android {
    namespace = "ai.shadowvision"
    compileSdk = 35
    ndkVersion = "28.0.13004108"   // r28 = 16 KB aligned by default

    defaultConfig {
        applicationId = "ai.shadowvision"
        minSdk = 29
        targetSdk = 35
        versionCode = 2
        versionName = "0.9-studio"
        resourceConfigurations += listOf("en", "ar")
        ndk { abiFilters += listOf("arm64-v8a") }   // one ABI; 32-bit is not a target
        // The G0 harness activity is a debug-only tool.
        manifestPlaceholders["g0Enabled"] = "true"
        externalNativeBuild {
            cmake {
                arguments += mutableListOf(
                    "-DANDROID_STL=c++_shared",
                    "-DSV_ANDROID=ON",
                    "-DSV_WITH_ORT=" + if (ortRoot != null) "ON" else "OFF"
                )
                if (ortRoot != null) arguments += "-DSV_ORT_ROOT=$ortRoot"
                // No -ffast-math: denormal flushing changes LaMa output silently and
                // destroys the bit-exact comparisons the G0 tests rely on.
                cppFlags += listOf("-O2", "-fvisibility=hidden")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
            versionNameSuffix = "-debug"
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            manifestPlaceholders["g0Enabled"] = "false"
            // A release build MUST be signed with a real key. Falling back to the
            // debug key silently produces an APK that looks shippable and is not,
            // so the fallback is opt-in (sv.allowDebugSigning) and logged as a
            // warning for anyone who asks for it.
            val allowDebugSign = (project.findProperty("sv.allowDebugSigning") == "true")
            if (hasReleaseKey) {
                signingConfig = signingConfigs.create("release") {
                    storeFile = file(releaseStore!!)
                    storePassword = releaseStorePass
                    keyAlias = releaseAlias
                    keyPassword = releaseKeyPass
                }
            } else if (allowDebugSign) {
                logger.warn("WARNING: release build is DEBUG-SIGNED (-Psv.allowDebugSigning=true). " +
                    "This APK must not be published.")
                signingConfig = signingConfigs.getByName("debug")
            } else {
                throw GradleException(
                    "No release signing key. Provide SV_KEYSTORE_FILE/SV_KEY_ALIAS/" +
                    "SV_KEYSTORE_PASSWORD/SV_KEY_PASSWORD (or keystore.properties), " +
                    "or pass -Psv.allowDebugSigning=true for a local, non-publishable " +
                    "build. Run tools/make_release_keystore.sh once to create one.")
            }
        }
    }

    packaging {
        jniLibs { useLegacyPackaging = false }   // uncompressed + 16 KB aligned
        resources.excludes += setOf("META-INF/*.kotlin_module", "META-INF/LICENSE*")
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    // SvEngine and both screens are coroutine-based; this was missing entirely,
    // so nothing in the app could compile.
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.exifinterface:exifinterface:1.3.7")

    val composeBom = platform("androidx.compose:compose-bom:2024.10.01")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-core")
}
