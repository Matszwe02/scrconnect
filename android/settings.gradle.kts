pluginManagement {
    repositories {
        gradlePluginPortal()
        google()
        mavenCentral()
        maven {
            url = uri("https://jitpack.io")
        }
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        // Robolectric's Android framework (80 MB), which Maven Central often refuses with HTTP 429: Google's mirror of it
        exclusiveContent {
            forRepository {
                maven {
                    url = uri("https://maven-central.storage-download.googleapis.com/maven2")
                }
            }
            filter {
                includeModule("org.robolectric", "android-all-instrumented")
            }
        }
        google()
        mavenCentral()
        maven {
            url = uri("https://jitpack.io")
        }
    }
}

buildscript {
    repositories {
        google()
        mavenCentral()
    }
    dependencies {
        classpath("org.ow2.asm:asm-util:9.6")
    }
}

rootProject.name = "scrconnect"
