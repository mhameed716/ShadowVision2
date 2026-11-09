// ShadowVision2 — app module (G0 harness shell).
//
// Architecture rule (docs/design-review-ar.md §4): the UI never touches native
// inference state directly. It calls SvEngine (Kotlin) which owns a single
// worker thread and a stable C ABI (native/sv_jni.cpp). Everything below that
// line is C++ and knows nothing about Android.
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// NDK paths can be overridden without editing this file:
//   local.properties:  ndk.dir=/path/to/ndk/28.0.13004108
// or GRADLE property:   -Psv.ortRoot=/path/to/onnxruntime-android
val localProps = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
val ortRoot: String? = (project.findProperty("sv.ortRoot") as String?) ?: localProps.getProperty("sv.ortRoot")

android {
    namespace = "ai.shadowvision.g0"
    compileSdk = 35
    ndkVersion = "28.0.13004108"   // r28 = 16 KB aligned by default

    defaultConfig {
        applicationId = "ai.shadowvision.g0"
        minSdk = 29                 // scoped storage + QNN baseline; no NNAPI reliance
        targetSdk = 35
        versionCode = 1
        versionName = "0.1-g0"
        ndk { abiFilters += listOf("arm64-v8a") }   // one ABI: 32-bit is not a target
        externalNativeBuild {
            cmake {
                // Only pass -DSV_ORT_ROOT when configured: an empty value makes
                // CMake treat SV_ORT_ROOT as "defined but empty".
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
            path = file("../../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures { viewBinding = true }

    buildTypes {
        debug {
            isMinifyEnabled = false
            // Dev console is only reachable in debug builds; release hides it.
            buildConfigField("boolean", "SV_DEV_CONSOLE", "true")
        }
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            buildConfigField("boolean", "SV_DEV_CONSOLE", "false")
        }
    }

    packaging {
        jniLibs { useLegacyPackaging = false }   // uncompressed + 16 KB aligned
        resources.excludes += setOf("META-INF/*.kotlin_module")
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")

    // ONNX Runtime: pick ONE. QNN build only works on Snapdragon and is ~7 MB
    // larger, so it is a separate product flavor (see below), never the default.
    // implementation("com.microsoft.onnxruntime:onnxruntime-android:1.23.2")
    // implementation("com.microsoft.onnxruntime:onnxruntime-android-qnn:1.23.2")
}
