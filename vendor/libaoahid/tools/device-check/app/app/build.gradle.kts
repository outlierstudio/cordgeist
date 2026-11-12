// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
// The application uses only platform APIs so an evidence APK has no third-party runtime code.
plugins {
    id("com.android.application")
}

android {
    namespace = "dev.aoahid.devicecheck"
    compileSdk = 36

    defaultConfig {
        applicationId = "dev.aoahid.devicecheck"
        minSdk = 23
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
