add_executable(xdispd xdispd.cpp lifecycle.cpp child_termination.cpp)
set(XDISP_MIRGUD_PATH "/usr/libexec/lomiri-xdisp/mirgud" CACHE STRING
  "Path to the standalone mirgud child executable")
target_include_directories(xdispd PRIVATE SYSTEM
  ${GIO_INCLUDE_DIRS}
  ${UDEV_INCLUDE_DIRS}
  ${DRM_INCLUDE_DIRS}
)
target_compile_definitions(xdispd PRIVATE
  XDISP_MIRGUD_PATH="${XDISP_MIRGUD_PATH}"
)
target_link_libraries(xdispd
  ${GIO_LDFLAGS} ${GIO_LIBRARIES}
  ${UDEV_LDFLAGS} ${UDEV_LIBRARIES}
  ${DRM_LDFLAGS} ${DRM_LIBRARIES}
)

install(TARGETS xdispd RUNTIME DESTINATION libexec/lomiri-xdisp)
install(FILES ${PROJECT_SOURCE_DIR}/data/xdisp.service DESTINATION lib/systemd/system)
install(FILES ${PROJECT_SOURCE_DIR}/data/org.lomiri.XDisp.service DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system-services)
install(FILES ${PROJECT_SOURCE_DIR}/data/org.lomiri.XDisp.conf DESTINATION /etc/dbus-1/system.d)
