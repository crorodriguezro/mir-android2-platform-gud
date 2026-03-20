add_definitions(-DANDROID)

find_package(Protobuf REQUIRED)

include_directories(
  ${ANDROID_HEADERS_INCLUDE_DIRS}
  ${PROJECT_SOURCE_DIR}/src/platforms/android/include
)
add_definitions(
    -DMIR_SERVER_GRAPHICS_PLATFORM_ABI_STRING="${MIR_SERVER_GRAPHICS_PLATFORM_ABI}"
    -DMIR_SERVER_PLATFORM_PATH="${MIR_SERVER_PLATFORM_PATH}"
    )

set(
  ANDROID_UNIT_TEST_SOURCES
)

# disable all implicit fallthrough warnings for GCC > 7.0
if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 7.0)
	add_compile_options(-Wno-implicit-fallthrough)
endif()

add_subdirectory(server)
add_subdirectory(client)

mir_add_wrapped_executable(mir_unit_tests_android2
  ${ANDROID_UNIT_TEST_SOURCES}
  ##  $<TARGET_OBJECTS:mir-test-doubles-udev>
  $<TARGET_OBJECTS:mirgl>
)

# Export all symbols to dynamic symbol table so that dlsym(RTLD_DEFAULT, "main")
# works correctly in RefCountedLibrary, preventing it from trying to dlopen the PIE executable
target_link_options(mir_unit_tests_android2 PRIVATE -rdynamic)

add_dependencies(mir_unit_tests_android2 GMock)

# Get the GTest version from pkg-config file to let server/test_hwc_fallback_gl_renderer.cpp
# include GMock fixes for older GMock version conditionally.
# If the pkg-config file for gtest is not installed, assumed the older version
# and use the fix.
pkg_check_modules(GTEST "gtest")
if (NOT GTEST_FOUND OR "${GTEST_VERSION}" VERSION_LESS "1.9.0")
  target_compile_definitions(mir_unit_tests_android2 PRIVATE -DUSE_GMOCK_FIXES)
endif()

target_link_libraries(
  mir_unit_tests_android2
  mirsharedandroid-static
  client_platform_common
  server_platform_common
  mir-test-doubles-platform-static

  ${MIRTEST_LDFLAGS}
  ${PROTOBUF_LITE_LIBRARIES}
  ${GTEST_BOTH_LIBRARIES}
  ${GMOCK_LIBRARIES}
  ${Boost_PROGRAM_OPTIONS_LIBRARY}
  ${LIBHARDWARE_LDFLAGS} ${LIBHARDWARE_LIBRARIES}
  ${DEVICEINFO_LDFLAGS} ${DEVICEINFO_LIBRARIES}
  -lhwc2 -lgralloc -lhybris-eglplatformcommon -lsync
  ${ANDROID_PROPERTIES_LDFLAGS} ${ANDROID_PROPERTIES_LIBRARIES}
  ${WAYLAND_SERVER_LDFLAGS} ${WAYLAND_SERVER_LIBRARIES}
  dl
)

if (MIR_RUN_UNIT_TESTS)
  mir_discover_tests_with_fd_leak_detection(mir_unit_tests_android2)
endif (MIR_RUN_UNIT_TESTS)
