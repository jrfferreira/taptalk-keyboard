cmake_minimum_required(VERSION 3.16)

# Version string = release number + short git hash (+ "-dirty" for an uncommitted
# tree), baked into the app descriptor so the firmware can show exactly which
# commit built it. app_desc->version is 32 bytes; this stays well under.
set(TAPTALK_RELEASE "0.2.0")
execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
execute_process(
    COMMAND git status --porcelain --untracked-files=no
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DIRTY
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT GIT_HASH)
    set(GIT_HASH "nogit")
endif()
if(GIT_DIRTY)
    set(GIT_HASH "${GIT_HASH}-dirty")
endif()
set(PROJECT_VER "${TAPTALK_RELEASE}+${GIT_HASH}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(taptalk-keyboard)
