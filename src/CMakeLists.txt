# We need MIR_CLIENT_PLATFORM_PATH in both libmirclient and the platform
# implementations
set(MIR_CLIENT_PLATFORM_PATH ${MIR_LIB_PATH}/client-platform)

# Add the core and platform implementations before exposing any private APIs
add_subdirectory(gl/)
add_subdirectory(platforms/)

# E4-T06 production boundary: these are standalone Mir clients/daemons.
# They intentionally do not link to Android2 server implementation objects.
mir_add_wrapped_executable(mirgud NOINSTALL ${PROJECT_SOURCE_DIR}/src/utils/gud_screencast.cpp)
target_include_directories(mirgud PRIVATE SYSTEM ${DRM_INCLUDE_DIRS})
target_link_libraries(mirgud
  ${MIRCLIENT_LDFLAGS} ${MIRCLIENT_LIBRARIES}
  ${Boost_PROGRAM_OPTIONS_LIBRARY}
  ${EGL_LIBRARIES}
  ${GLESv2_LIBRARIES}
  ${DRM_LDFLAGS} ${DRM_LIBRARIES}
)
install(PROGRAMS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mirgud.bin
  DESTINATION libexec/lomiri-xdisp
  RENAME mirgud)
add_subdirectory(xdisp)

add_definitions(-DMIRCORE_LIBDIR="${MIRCORE_LIBDIR}")
add_executable(wrapper wrapper.c)
target_compile_definitions(wrapper PUBLIC
    EXECUTABLE_FORMAT=\"%s.bin\"
    _DEFAULT_SOURCE
    _BSD_SOURCE)

set(
  MIR_GENERATED_INCLUDE_DIRECTORIES
  ${MIR_GENERATED_INCLUDE_DIRECTORIES}
  PARENT_SCOPE)

set(MIR_PLATFORM_OBJECTS ${MIR_PLATFORM_OBJECTS} PARENT_SCOPE)
set(MIR_PLATFORM_REFERENCES ${MIR_PLATFORM_REFERENCES} PARENT_SCOPE)
set(MIR_CLIENT_PLATFORM_PATH ${MIR_CLIENT_PLATFORM_PATH} PARENT_SCOPE)
set(MIR_SERVER_PLATFORM_PATH ${MIR_SERVER_PLATFORM_PATH} PARENT_SCOPE)

# We need the ABI versions in the tests
set(MIR_SERVER_GRAPHICS_PLATFORM_ABI ${MIR_SERVER_GRAPHICS_PLATFORM_ABI} PARENT_SCOPE)
set(MIR_SERVER_GRAPHICS_PLATFORM_VERSION ${MIR_SERVER_GRAPHICS_PLATFORM_VERSION} PARENT_SCOPE)
set(MIR_CLIENT_PLATFORM_ABI ${MIR_CLIENT_PLATFORM_ABI} PARENT_SCOPE)
set(MIR_CLIENT_PLATFORM_VERSION ${MIR_CLIENT_PLATFORM_VERSION} PARENT_SCOPE)
