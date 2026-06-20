install(PROGRAMS alaska DESTINATION bin)
install(PROGRAMS alaska++ DESTINATION bin)

# alaska-transform decides at runtime whether to request the refcount passes,
# but that choice is fixed at configure time (the passes are only registered in
# the plugin when ALASKA_ENABLE_REFCOUNT is on). Bake the decision into the
# installed script via configure_file rather than installing the source as-is.
if(ALASKA_ENABLE_REFCOUNT)
  set(ALASKA_ENABLE_REFCOUNT_PY "True")
else()
  set(ALASKA_ENABLE_REFCOUNT_PY "False")
endif()
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/alaska-transform
  ${CMAKE_CURRENT_BINARY_DIR}/alaska-transform
  @ONLY)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/alaska-transform DESTINATION bin)

install(PROGRAMS alaska-transform-baseline DESTINATION bin)
install(PROGRAMS alaska-type-infer DESTINATION bin)

install(PROGRAMS alaska-clang DESTINATION bin)
install(PROGRAMS ld.alaska-clang DESTINATION bin)
# install(PROGRAMS alaska-config DESTINATION bin)

add_executable(alaska-config alaska-config.cpp)
install(TARGETS alaska-config)
