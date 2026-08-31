plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val libwebrtcVersion = "144.7559.09"
val okhttpVersion = "5.3.0"
val cameraXVersion = "1.5.3"
val releaseStoreFile = providers.environmentVariable("REMOE_RELEASE_STORE_FILE").orNull
val releaseStorePassword = providers.environmentVariable("REMOE_RELEASE_STORE_PASSWORD").orNull
val releaseKeyAlias = providers.environmentVariable("REMOE_RELEASE_KEY_ALIAS").orNull
val releaseKeyPassword = providers.environmentVariable("REMOE_RELEASE_KEY_PASSWORD").orNull
val releaseSigningConfigured = listOf(
    releaseStoreFile, releaseStorePassword, releaseKeyAlias, releaseKeyPassword,
).all { !it.isNullOrBlank() }

if (gradle.startParameter.taskNames.any { it.contains("release", ignoreCase = true) } &&
    !releaseSigningConfigured) {
    throw GradleException(
        "Release signing requires REMOE_RELEASE_STORE_FILE, REMOE_RELEASE_STORE_PASSWORD, " +
            "REMOE_RELEASE_KEY_ALIAS, and REMOE_RELEASE_KEY_PASSWORD",
    )
}

android {
    namespace = "top.ozaoza.remoe"
    compileSdk = 35

    defaultConfig {
        applicationId = "top.ozaoza.remoe"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    signingConfigs {
        if (releaseSigningConfigured) {
            create("remoeRelease") {
                storeFile = file(releaseStoreFile!!)
                storePassword = releaseStorePassword
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPassword
                enableV1Signing = true
                enableV2Signing = true
                enableV3Signing = true
                enableV4Signing = true
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("remoeRelease")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("io.github.webrtc-sdk:android:$libwebrtcVersion")
    implementation("com.squareup.okhttp3:okhttp:$okhttpVersion")
    implementation("androidx.activity:activity-ktx:1.10.1")
    implementation("androidx.camera:camera-camera2:$cameraXVersion")
    implementation("androidx.camera:camera-lifecycle:$cameraXVersion")
    implementation("androidx.camera:camera-view:$cameraXVersion")
    implementation("com.google.mlkit:barcode-scanning:17.3.0")
    testImplementation("junit:junit:4.13.2")
}
