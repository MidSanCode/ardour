# Keep the native entry points: they are called from Java through JNI.
-keep class org.ardour.ardour.** { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
