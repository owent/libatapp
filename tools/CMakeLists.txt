

EchoWithColor(COLOR GREEN "-- Configure Tools ${CMAKE_CURRENT_LIST_DIR}")

# ============ TOOLS - [...] ============

file(GLOB TOOLS_SRC_LIST
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/*.cc
    ${CMAKE_CURRENT_LIST_DIR}/*.c
    ${CMAKE_CURRENT_LIST_DIR}/*.cxx
)

# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools")

foreach(TOOLS_SRC_FILE IN LISTS TOOLS_SRC_LIST)
    get_filename_component(TOOLS_SRC_BIN_NAME "${TOOLS_SRC_FILE}" NAME_WE)

    add_executable(${TOOLS_SRC_BIN_NAME} ${TOOLS_SRC_FILE})

    target_include_directories(${TOOLS_SRC_BIN_NAME} 
        PRIVATE 
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>"
    )

    target_link_libraries(${TOOLS_SRC_BIN_NAME} atapp)

	install(TARGETS "${TOOLS_SRC_BIN_NAME}"
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

endforeach()
