// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
// Standalone diagnostic application; it is intentionally independent of the host library build.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "aoahid-device-check"
include(":app")
