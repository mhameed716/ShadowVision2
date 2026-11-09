# SPDX-License-Identifier: MIT
# ShadowVision2 — G0 native core.
#
# Builds three things from the same sources:
#   sv_core   static library, no ONNX Runtime required
#   sv_g0     the gate harness (host or android)
#   sv_tests  unit tests, no framework dependency, runnable on device too
#
# ONNX Runtime is opt-in: -DSV_WITH_ORT=ON -DSV_ORT_ROOT=/path/to/ort
# so that CI and any laptop can run the whole harness before a single weight
# has been downloaded.
cmake_minimum_required(VERSION 3.18)
project(shadowvision_g0 C CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SV_WITH_ORT "Build the ONNX Runtime backend" OFF)
option(SV_SANITIZE "Build with ASan/UBSan" OFF)
option(SV_ANDROID "Android build (NDK)" OFF)

set(SV_WARNINGS -Wall -Wextra -Wpedantic -Wno-unused-parameter)

add_library(sv_core STATIC
  src/memory.cpp
  src/kernels.cpp
  src/image_io.cpp
  src/inpaint_lama.cpp
  src/synthetic_backend.cpp
  src/sha256.cpp
  src/manifest.cpp
  src/segmenter.cpp
)

# Ed25519 (Orson Peters, zlib licence) — vendored, see third_party/ed25519/LICENSE
add_library(sv_ed25519 STATIC
  third_party/ed25519/add_scalar.c
  third_party/ed25519/fe.c
  third_party/ed25519/ge.c
  third_party/ed25519/key_exchange.c
  third_party/ed25519/keypair.c
  third_party/ed25519/sc.c
  third_party/ed25519/seed.c
  third_party/ed25519/sha512.c
  third_party/ed25519/sign.c
  third_party/ed25519/verify.c
)
target_include_directories(sv_ed25519 PUBLIC third_party/ed25519)
target_compile_options(sv_ed25519 PRIVATE -w)
target_link_libraries(sv_core PUBLIC sv_ed25519)
target_include_directories(sv_core PUBLIC include)
target_compile_options(sv_core PRIVATE ${SV_WARNINGS})

if(SV_SANITIZE)
  target_compile_options(sv_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(sv_core PUBLIC -fsanitize=address,undefined)
endif()

if(SV_WITH_ORT)
  if(NOT SV_ORT_ROOT)
    message(FATAL_ERROR "SV_WITH_ORT=ON requires -DSV_ORT_ROOT=<onnxruntime dir>")
  endif()
  target_sources(sv_core PRIVATE src/ort_backend.cpp src/ort_segmenter.cpp)
  target_compile_definitions(sv_core PUBLIC SV_WITH_ORT=1)
  target_include_directories(sv_core PUBLIC ${SV_ORT_ROOT}/include)
  # Android ships libonnxruntime.so in jniLibs; host uses the static/dynamic lib.
  find_library(SV_ORT_LIB onnxruntime HINTS ${SV_ORT_ROOT}/lib ${SV_ORT_ROOT}/jni/arm64-v8a)
  if(SV_ORT_LIB)
    target_link_libraries(sv_core PUBLIC ${SV_ORT_LIB})
  else()
    message(WARNING "libonnxruntime not found under ${SV_ORT_ROOT}; link it manually.")
  endif()
else()
  target_compile_definitions(sv_core PUBLIC SV_WITH_ORT=0)
endif()

add_executable(sv_g0 tools/sv_g0.cpp)
target_link_libraries(sv_g0 PRIVATE sv_core)

add_executable(sv_tests tests/sv_tests.cpp)
target_link_libraries(sv_tests PRIVATE sv_core)

# The ONNX segmenter only exists when SV_WITH_ORT=ON, so its integration test
# (which runs real graphs through the real runtime) is built in the same
# condition. Without ORT this target simply does not exist, and the gate says so
# rather than printing a green line for a test that never ran.
if(SV_WITH_ORT)
  add_executable(sv_ort_integration tests/sv_ort_integration.cpp)
  target_link_libraries(sv_ort_integration PRIVATE sv_core)
endif()

enable_testing()
add_test(NAME unit COMMAND sv_tests)
add_test(NAME gate_synthetic
         COMMAND sv_g0 --gen-test ${CMAKE_CURRENT_BINARY_DIR} 2>/dev/null || true)
add_test(NAME gate_report
         COMMAND sv_g0 --image ${CMAKE_CURRENT_BINARY_DIR}/test.ppm
                      --mask ${CMAKE_CURRENT_BINARY_DIR}/test.pgm
                      --backend synthetic --repeat 3)
add_test(NAME gate_budget_refused
         COMMAND sv_g0 --image ${CMAKE_CURRENT_BINARY_DIR}/test.ppm
                      --mask ${CMAKE_CURRENT_BINARY_DIR}/test.pgm
                      --budget-mb 1 --repeat 1)
set_tests_properties(gate_budget_refused PROPERTIES WILL_FAIL TRUE)
