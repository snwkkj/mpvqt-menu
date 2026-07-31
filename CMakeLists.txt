cmake_minimum_required(VERSION 3.21)

project(mpvqt-menu VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv>=0.37.0)
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Threads REQUIRED)

add_executable(menu-helper src/helper_main.cpp)
target_link_libraries(menu-helper PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

set(HELPER_BLOB "${CMAKE_CURRENT_BINARY_DIR}/helper_blob.o")
add_custom_command(
    OUTPUT "${HELPER_BLOB}"
    COMMAND "${CMAKE_LINKER}" -r -b binary menu-helper -o helper_blob.o
    COMMAND "${CMAKE_OBJCOPY}"
            --rename-section .data=.rodata,alloc,load,readonly,data,contents
            helper_blob.o
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS menu-helper
    VERBATIM
)
set_source_files_properties("${HELPER_BLOB}" PROPERTIES
    GENERATED TRUE
    EXTERNAL_OBJECT TRUE
)

add_library(menu SHARED src/menu.cpp src/plugin.cpp "${HELPER_BLOB}")

set_target_properties(menu PROPERTIES
    PREFIX ""
    OUTPUT_NAME "menu"
    POSITION_INDEPENDENT_CODE ON
)

set_source_files_properties(src/plugin.cpp PROPERTIES
    COMPILE_DEFINITIONS MPV_CPLUGIN_DYNAMIC_SYM
)
target_include_directories(menu PRIVATE ${MPV_INCLUDE_DIRS})
target_link_libraries(menu PRIVATE
    Qt6::Core
    Threads::Threads
)
target_link_options(menu PRIVATE -Wl,-z,noexecstack)

include(GNUInstallDirs)
install(TARGETS menu LIBRARY DESTINATION .)
install(FILES src/lua/menu.lua DESTINATION .)

set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGE_FILE_NAME "mpvqt-menu")
include(CPack)
