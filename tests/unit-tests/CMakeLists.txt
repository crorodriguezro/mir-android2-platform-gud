CMAKE_DEPENDENT_OPTION(
  MIR_RUN_UNIT_TESTS
  "Run unit tests as part of default testing"
  ON
  "MIR_BUILD_UNIT_TESTS"
  OFF)


add_subdirectory(client)
add_subdirectory(platforms)
add_subdirectory(xdisp)

add_executable(test_h264_rgb_to_nv12
  test_h264_rgb_to_nv12.c
  ${PROJECT_SOURCE_DIR}/src/utils/h264_rgb_to_nv12.c)
target_include_directories(test_h264_rgb_to_nv12 PRIVATE
  ${PROJECT_SOURCE_DIR}/src/utils)

add_executable(test_h264_latest_presenter_state
  test_h264_latest_presenter_state.c
  ${PROJECT_SOURCE_DIR}/src/utils/h264_latest_presenter_state.c)
target_include_directories(test_h264_latest_presenter_state PRIVATE
  ${PROJECT_SOURCE_DIR}/src/utils)

if (MIR_RUN_UNIT_TESTS)
  add_test(NAME h264_rgb_to_nv12 COMMAND test_h264_rgb_to_nv12)
  add_test(NAME h264_latest_presenter_state COMMAND test_h264_latest_presenter_state)
endif()
