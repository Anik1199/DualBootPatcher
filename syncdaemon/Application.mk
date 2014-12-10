APP_BUILD_SCRIPT := Android.mk

#APP_ABI := armeabi-v7a arm64-v8a x86 x86_64
APP_ABI := armeabi-v7a x86 x86_64

APP_PLATFORM := android-21

# GNU libstdc++ is needed for C++11's <thread>
APP_STL := gnustl_static
APP_CPPFLAGS := -std=c++11
NDK_TOOLCHAIN_VERSION := 4.9