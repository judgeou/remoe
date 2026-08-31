# Project-specific R8 rules belong here.

# The WebRTC AAR exposes Java classes and methods to its native JNI library by
# their binary names, but does not currently ship consumer keep rules. Renaming
# or removing them makes the release build crash during application startup.
-keep class org.webrtc.** { *; }

# WebRTC's JNI_OnLoad also calls the jni_zero bootstrap classes exclusively
# from native code. R8 otherwise sees them as unused and removes all of them.
-keep class org.jni_zero.** { *; }
