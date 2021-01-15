
echowithcolor(COLOR GREEN "-- Configure ${CMAKE_CURRENT_LIST_DIR}")

include("${CMAKE_CURRENT_LIST_DIR}/sample.custom-macro.cmake")

# ============ sample - [...] ============

file(
  GLOB SAMPLE_SRC_LIST
  RELATIVE "${PROJECT_SAMPLE_SRC_DIR}"
  ${PROJECT_SAMPLE_SRC_DIR}/*.cpp ${PROJECT_SAMPLE_SRC_DIR}/*.cc ${PROJECT_SAMPLE_SRC_DIR}/*.c ${PROJECT_SAMPLE_SRC_DIR}/*.cxx)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/sample")

foreach(SAMPLE_SRC_FILE IN LISTS SAMPLE_SRC_LIST)
  get_filename_component(SAMPLE_SRC_BIN_NAME "${SAMPLE_SRC_FILE}" NAME_WE)

  add_executable(${SAMPLE_SRC_BIN_NAME} ${SAMPLE_SRC_FILE})

  if(MSVC)
    set_property(TARGET ${SAMPLE_SRC_BIN_NAME} PROPERTY FOLDER "atframework/sample")
    add_target_properties(${SAMPLE_SRC_BIN_NAME} LINK_FLAGS /NODEFAULTLIB:library)
  endif()

  target_link_libraries(${SAMPLE_SRC_BIN_NAME} atapp)

  set_target_properties(
    ${SAMPLE_SRC_BIN_NAME}
    PROPERTIES INSTALL_RPATH_USE_LINK_PATH YES
               BUILD_WITH_INSTALL_RPATH NO
               BUILD_RPATH_USE_ORIGIN YES)
endforeach()
