mir_add_wrapped_executable(mir_unit_tests_xdisp NOINSTALL
  test_lifecycle.cpp
  ${PROJECT_SOURCE_DIR}/src/xdisp/lifecycle.cpp
)

target_include_directories(mir_unit_tests_xdisp PRIVATE ${PROJECT_SOURCE_DIR}/src/xdisp)
target_link_libraries(mir_unit_tests_xdisp
  ${GTEST_BOTH_LIBRARIES}
  ${GMOCK_LIBRARIES}
  pthread
)
add_dependencies(mir_unit_tests_xdisp GMock)

if (MIR_RUN_UNIT_TESTS)
  mir_discover_tests_with_fd_leak_detection(mir_unit_tests_xdisp)
endif()
