plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val libwebrtcVersion = "144.7559.09"
val okhttpVersion = "5.3.0"
val cameraXVersion = "1.5.3"

android {
    namespace = "top.ozaoza.remoe"
    compileSdk = 35

    defaultConfig {
        applicationId = "top.ozaoza.remoe"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
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
