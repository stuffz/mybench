cmake_minimum_required(VERSION 3.21)
project(mybench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Svg)

# Distros differ on how Qt6 is built: Arch (and CachyOS) build it with
# indirect-extern-access, so a binary that takes copy relocations against
# Qt's exported data symbols refuses to load there with
#   "error due to GNU_PROPERTY_1_NEEDED_INDIRECT_EXTERN_ACCESS".
# Compiling our own translation units for indirect access makes the binary
# portable across both; the codegen cost is negligible for a GUI.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-mno-direct-extern-access HAVE_NO_DIRECT_EXTERN_ACCESS)

qt_standard_project_setup()

# src/ is grouped by layer: app/ (process glue: backend, RPC, theme, icons),
# shell/ (window chrome), dialogs/ (modal dialogs), editor/ (query tab and
# results), views/ (admin panels and inspectors), ui/ (shared widget helpers).
qt_add_executable(mybench
    src/app/main.cpp
    src/app/theme.cpp
    src/app/api.cpp
    src/app/backend.cpp
    src/app/icons.cpp
    src/shell/mainwindow.cpp
    src/shell/servertabbar.cpp
    src/shell/statusstrip.cpp
    src/shell/sidebar.cpp
    src/shell/workspace.cpp
    src/dialogs/connectionsdialog.cpp
    src/dialogs/prefsdialog.cpp
    src/dialogs/aboutdialog.cpp
    src/dialogs/shortcutsdialog.cpp
    src/dialogs/importrowsdialog.cpp
    src/dialogs/querydialog.cpp
    src/editor/editortab.cpp
    src/editor/sqleditor.cpp
    src/editor/sqlscan.cpp
    src/editor/sqlhighlighter.cpp
    src/editor/resultpage.cpp
    src/editor/resultgrid.cpp
    src/editor/resultmodel.cpp
    src/editor/planview.cpp
    src/views/panelbase.cpp
    src/views/processlistview.cpp
    src/views/usersview.cpp
    src/views/serverinfoview.cpp
    src/views/innodbview.cpp
    src/views/historyview.cpp
    src/views/dashboardview.cpp
    src/views/statcard.cpp
    src/views/inspectorview.cpp
    src/views/graphcanvas.cpp
    src/views/graphview.cpp
    src/views/chart.cpp
    src/ui/tableutil.cpp
    src/ui/widgets.cpp
    src/ui/switchbox.cpp
    src/ui/fmt.cpp
    resources.qrc
)

target_link_libraries(mybench PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg)

# Single-exe mode: the Go backend linked in as a c-archive instead of shipping
# mybench-backend(.exe) beside the client. The Taskfile's gui:build:windows
# builds the archive (backend/cmd/mybench-backend-lib) and passes its path.
option(MYBENCH_EMBED_BACKEND "Link the Go backend c-archive into the executable" OFF)
if(MYBENCH_EMBED_BACKEND)
    if(NOT EXISTS "${MYBENCH_BACKEND_ARCHIVE}")
        message(FATAL_ERROR "MYBENCH_EMBED_BACKEND needs -DMYBENCH_BACKEND_ARCHIVE=<libmybench-backend.a>")
    endif()
    target_compile_definitions(mybench PRIVATE MYBENCH_EMBED_BACKEND)
    target_link_libraries(mybench PRIVATE "${MYBENCH_BACKEND_ARCHIVE}")
    if(WIN32)
        # What the Go runtime and net stack import beyond kernel32/user32.
        target_link_libraries(mybench PRIVATE ws2_32 winmm ntdll)
    endif()
endif()

# Windows icon + version metadata. Both come from build/windows/, so bumping
# info.json for a release (RELEASE.md) covers this binary as well; nothing is
# duplicated here. WIN32_EXECUTABLE below already keeps the console hidden.
if(WIN32)
    enable_language(RC)
    set(WINRES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/windows")
    if(EXISTS "${WINRES_DIR}/info.json" AND EXISTS "${WINRES_DIR}/icon.ico")
        file(READ "${WINRES_DIR}/info.json" MYBENCH_INFO_JSON)
        string(JSON MYBENCH_VERSION GET "${MYBENCH_INFO_JSON}" fixed file_version)
        string(JSON MYBENCH_INFO_BLOCK GET "${MYBENCH_INFO_JSON}" info 0000)
        string(JSON MYBENCH_COMPANY GET "${MYBENCH_INFO_BLOCK}" CompanyName)
        string(JSON MYBENCH_DESCRIPTION GET "${MYBENCH_INFO_BLOCK}" FileDescription)
        string(JSON MYBENCH_COPYRIGHT GET "${MYBENCH_INFO_BLOCK}" LegalCopyright)
        string(JSON MYBENCH_PRODUCT GET "${MYBENCH_INFO_BLOCK}" ProductName)
        # VERSIONINFO wants 1,2,3,4 rather than the dotted form.
        string(REPLACE "." "," MYBENCH_VERSION_COMMAS "${MYBENCH_VERSION}")
        # A backslash in an .rc string is an escape, so keep the path in the
        # forward-slash form windres also accepts.
        file(TO_CMAKE_PATH "${WINRES_DIR}/icon.ico" MYBENCH_ICON)
        configure_file(app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
        target_sources(mybench PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
        message(STATUS "Windows resources: icon + version ${MYBENCH_VERSION}")
    else()
        # Not fatal: the client still builds, it just ships the default icon.
        message(WARNING "build/windows/{info.json,icon.ico} missing — no exe icon")
    endif()
endif()

# macOS bundle: Info.plist and icon, from the same single version source as
# the Windows resources above. Unsigned beyond the ad-hoc signature the
# install task applies, so this covers a local install, not distribution.
if(APPLE)
    set(MACRES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build")
    file(READ "${MACRES_DIR}/windows/info.json" MYBENCH_INFO_JSON)
    string(JSON MYBENCH_VERSION GET "${MYBENCH_INFO_JSON}" fixed file_version)
    string(JSON MYBENCH_INFO_BLOCK GET "${MYBENCH_INFO_JSON}" info 0000)
    string(JSON MYBENCH_COPYRIGHT GET "${MYBENCH_INFO_BLOCK}" LegalCopyright)
    configure_file(Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
    set_target_properties(mybench PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    )
    target_sources(mybench PRIVATE "${MACRES_DIR}/macos/icon.icns")
    set_source_files_properties(
        "${MACRES_DIR}/macos/icon.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources
    )
    message(STATUS "macOS bundle: icon + version ${MYBENCH_VERSION}")
endif()

if(UNIX AND NOT APPLE AND HAVE_NO_DIRECT_EXTERN_ACCESS)
    target_compile_options(mybench PRIVATE -mno-direct-extern-access)
endif()
target_include_directories(mybench PRIVATE src)

# The result grid is the hot path; keep it optimised even in dev builds.
set_property(TARGET mybench PROPERTY WIN32_EXECUTABLE TRUE)

# Off by default so the packaging builds (deb, arch, windows, macOS bundle)
# configure exactly what they did before; gui:test turns it on. A cross build
# could not run what it built anyway.
option(MYBENCH_BUILD_TESTS "Build the QTest suite under tests/" OFF)
if(MYBENCH_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_subdirectory(tests)
endif()
