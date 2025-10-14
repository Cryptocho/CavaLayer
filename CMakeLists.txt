cmake_minimum_required(VERSION 3.20)

project(cavalayer)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLCLIENT REQUIRED wayland-client)
pkg_check_modules(WLEGL REQUIRED wayland-egl)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GL REQUIRED gl)
pkg_check_modules(TVG REQUIRED thorvg)

include_directories(
    include 
    ${WLCLIENT_INCLUDE_DIRS}
    ${WLEGL_INCLUDE_DIRS}
)

link_directories(
    ${WLCLIENT_LIBRARY_DIRS}
)

link_libraries(
    ${WLCLIENT_LIBRARIES}
    ${WLEGL_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GL_LIBRARIES}
    ${TVG_LIBRARIES}
)

file(GLOB_RECURSE SOURCES
    "src/*.cpp"
    "src/*.c"
)

set_source_files_properties(
    src/layer-shell-client-protocol.c
    src/xdg-shell-protocol.c
    PROPERTIES LANGUAGE C
)

add_executable(${PROJECT_NAME} ${SOURCES})