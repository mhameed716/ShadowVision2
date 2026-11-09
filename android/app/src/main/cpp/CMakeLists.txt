# SPDX-License-Identifier: MIT
# The Android shared library. This file is what Gradle's externalNativeBuild
# actually points at; it reuses the host-buildable core in ../../../../native.
cmake_minimum_required(VERSION 3.22)
project(shadowvision_jni C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SV_NATIVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native)

# Same core sources as the desktop harness, so host and device run identical code.
add_library(sv_core STATIC
  ${SV_NATIVE_ROOT}/src/memory.cpp
  ${SV_NATIVE_ROOT}/src/kernels.cpp
  ${SV_NATIVE_ROOT}/src/image_io.cpp
  ${SV_NATIVE_ROOT}/src/inpaint_lama.cpp
  ${SV_NATIVE_ROOT}/src/synthetic_backend.cpp
  ${SV_NATIVE_ROOT}/src/sha256.cpp
  ${SV_NATIVE_ROOT}/src/manifest.cpp
  ${SV_NATIVE_ROOT}/src/segmenter.cpp
)

add_library(sv_ed25519 STATIC
  ${SV_NATIVE_ROOT}/third_party/ed25519/add_scalar.c
  ${SV_NATIVE_ROOT}/third_party/ed25519/fe.c
  ${SV_NATIVE_ROOT}/third_party/ed25519/ge.c
  ${SV_NATIVE_ROOT}/third_party/ed25519/key_exchange.c
  ${SV_NATIVE_ROOT}/third_party/ed25519/keypair.c
  ${SV_NATIVE_ROOT}/third_party/ed25519/sc.c
  ${SV_NATIVE_ROOT}/third_party/ed25519/seed.c
  ${SV_NATIVE_ROOT}/third_party/ed25519/sha512.c
  ${SV_NATIVE_ROOT}/third_party/ed25519/sign.c
  ${SV_NATIVE_ROOT}/third_party/ed25519/verify.c
)
target_include_directories(sv_ed25519 PUBLIC ${SV_NATIVE_ROOT}/third_party/ed25519)
target_compile_options(sv_ed25519 PRIVATE -w)
target_link_libraries(sv_core PUBLIC sv_ed25519)
target_include_directories(sv_core PUBLIC ${SV_NATIVE_ROOT}/include)
target_compile_options(sv_core PRIVATE -O2 -fvisibility=hidden -Wall -Wextra)

option(SV_WITH_ORT "Link ONNX Runtime into the app" OFF)
if(SV_WITH_ORT)
  add_library(onnxruntime SHARED IMPORTED)
  set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libonnxruntime.so)
  target_sources(sv_core PRIVATE ${SV_NATIVE_ROOT}/src/ort_backend.cpp ${SV_NATIVE_ROOT}/src/ort_segmenter.cpp)
  target_compile_definitions(sv_core PUBLIC SV_WITH_ORT=1)
  target_include_directories(sv_core PUBLIC ${SV_ORT_ROOT}/include)
  target_link_libraries(sv_core PUBLIC onnxruntime)
else()
  target_compile_definitions(sv_core PUBLIC SV_WITH_ORT=0)
endif()

add_library(shadowvision_jni SHARED
  ${SV_NATIVE_ROOT}/src/sv_c_api.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/jni_bridge.cpp)
target_link_libraries(shadowvision_jni PRIVATE sv_core android log)

# NDK r28 already emits 16 KB-aligned ELF segments; this is the belt-and-braces
# flag recommended by Google for older toolchains.
target_link_options(shadowvision_jni PRIVATE
  -Wl,-z,max-page-size=16384
  -Wl,--no-undefined
  -Wl,--gc-sections)
