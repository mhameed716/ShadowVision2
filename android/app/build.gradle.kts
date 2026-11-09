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
            signingConfig = signingConfigs.getByName("debug")  // replace before Play
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
