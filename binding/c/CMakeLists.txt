set(PROJECT_BINDING_NAME "atapp_c")
EchoWithColor(COLOR GREEN "-- Configure ${CMAKE_CURRENT_LIST_DIR}")

# ============ c binding ============

file(GLOB BINDING_SRC_LIST RELATIVE "${CMAKE_CURRENT_LIST_DIR}"
    ${CMAKE_CURRENT_LIST_DIR}/atframe/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/atframe/*.cc
    ${CMAKE_CURRENT_LIST_DIR}/atframe/*.c
    ${CMAKE_CURRENT_LIST_DIR}/atframe/*.cxx
)

# ============ libatbus - src ============
if(BUILD_SHARED_LIBS)   
    add_library(${PROJECT_BINDING_NAME} SHARED ${BINDING_SRC_LIST} ${PROJECT_3RD_PARTY_SRC_LIST})
    target_compile_definitions(${PROJECT_BINDING_NAME}
        PRIVATE LIBATAPP_MACRO_API_NATIVE=1 LIBATAPP_MACRO_API_DLL=1
    )
    set_target_properties(${PROJECT_BINDING_NAME} PROPERTIES INTERFACE_COMPILE_DEFINITIONS LIBATAPP_MACRO_API_DLL=1)
else ()
    add_library(${PROJECT_BINDING_NAME} STATIC ${BINDING_SRC_LIST} ${PROJECT_3RD_PARTY_SRC_LIST})
    target_compile_definitions(${PROJECT_BINDING_NAME}
        PRIVATE LIBATAPP_MACRO_API_NATIVE=1
    )
endif()

set_target_properties(${PROJECT_BINDING_NAME} PROPERTIES 
    C_VISIBILITY_PRESET "hidden"
    CXX_VISIBILITY_PRESET "hidden"
)

target_include_directories(${PROJECT_BINDING_NAME} 
    PRIVATE 
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>"
)

target_link_libraries(${PROJECT_BINDING_NAME}
    PUBLIC atapp
)

install(TARGETS ${PROJECT_BINDING_NAME}
    EXPORT ${PROJECT_LIBATAPP_EXPORT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES ${CMAKE_CURRENT_LIST_DIR}/atframe/libatapp_c.h
    DESTINATION include/atframe
)

install(FILES "${3RD_PARTY_ATFRAME_UTILS_INC_DIR}/config/compile_optimize.h"
    DESTINATION include/atframe/config
)

list(APPEND PROJECT_LIBATAPP_EXPORT_TARGETS ${PROJECT_BINDING_NAME})
