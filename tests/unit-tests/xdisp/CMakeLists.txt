mir_add_wrapped_executable(mir_unit_tests_xdisp NOINSTALL
  test_lifecycle.cpp
  test_child_termination.cpp
  test_pixel_format.cpp
  ${PROJECT_SOURCE_DIR}/src/xdisp/lifecycle.cpp
  ${PROJECT_SOURCE_DIR}/src/xdisp/child_termination.cpp
)

target_include_directories(mir_unit_tests_xdisp PRIVATE
  ${PROJECT_SOURCE_DIR}/src/xdisp
  ${PROJECT_SOURCE_DIR}/src/utils
  ${DRM_INCLUDE_DIRS}
)
target_link_libraries(mir_unit_tests_xdisp
  ${GTEST_BOTH_LIBRARIES}
  ${GMOCK_LIBRARIES}
  pthread
)
add_dependencies(mir_unit_tests_xdisp GMock)

if (MIR_RUN_UNIT_TESTS)
  mir_discover_tests_with_fd_leak_detection(mir_unit_tests_xdisp)
endif()

find_program(XDISP_PYTHON3_EXECUTABLE python3)
if (XDISP_PYTHON3_EXECUTABLE)
  add_test(NAME xdisp_usb_role_recovery_policy
    COMMAND ${XDISP_PYTHON3_EXECUTABLE}
      ${PROJECT_SOURCE_DIR}/tests/unit-tests/xdisp/test_usb_role_recovery_policy.py)
endif()
