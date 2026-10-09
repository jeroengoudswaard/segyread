# Builds segytest, segybench, gen_fixture and segyviewer.
#
# Qt: builds against Qt6 when it is found (Windows/vcpkg, Ubuntu), else falls
# back to Qt5 >= 5.15 (Rocky/RHEL 8, whose AppStream only ships Qt5). Force
# one explicitly with -DSEGY_QT_MAJOR=5 or -DSEGY_QT_MAJOR=6. Only segyviewer
# links Qt; segytest/segybench/gen_fixture don't need it at all.
#
# Point CMAKE_PREFIX_PATH (or Qt6_DIR / Qt5_DIR) at your Qt install if it
# isn't found automatically, e.g.:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x.x/gcc_64
# On Ubuntu with the qt6-base-dev apt package, no extra flag is needed.
cmake_minimum_required(VERSION 3.16)
project(segyread_native CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON) # compiles viewer/qt/app_icon.qrc (the app's window/taskbar icon)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- Qt: prefer 6, fall back to 5 ------------------------------------------
set(SEGY_QT_MAJOR "" CACHE STRING "Force the Qt major version (5 or 6); empty = prefer 6, fall back to 5")
if(SEGY_QT_MAJOR STREQUAL "6")
    find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)
    set(SEGY_QT_USED 6)
elseif(SEGY_QT_MAJOR STREQUAL "5")
    find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets Svg)
    set(SEGY_QT_USED 5)
else()
    find_package(Qt6 QUIET COMPONENTS Widgets Svg)
    if(Qt6_FOUND)
        set(SEGY_QT_USED 6)
    else()
        find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets Svg)
        set(SEGY_QT_USED 5)
    endif()
endif()
message(STATUS "Using Qt${SEGY_QT_USED}")
if(SEGY_QT_USED EQUAL 6 AND COMMAND qt_standard_project_setup)
    qt_standard_project_setup()
endif()

# GCC 8 (Rocky/RHEL 8's default) keeps std::filesystem in a separate
# libstdc++fs; GCC 9+ and MSVC don't need it, and MSVC would choke on the name.
set(SEGY_FS_LIBS "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    set(SEGY_FS_LIBS stdc++fs)
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# --- segycore: format/decode/pyramid/mmap/writer, no GUI dependency at all. -
add_library(segycore STATIC
    src/segy_format.cpp
    src/segy_decode.cpp
    src/segy_pyramid.cpp
    src/segy_select.cpp
    src/segy_writer.cpp
    $<$<PLATFORM_ID:Windows>:src/platform/segy_mmap_win32.cpp>
    $<$<NOT:$<PLATFORM_ID:Windows>>:src/platform/segy_mmap_posix.cpp>
)
target_include_directories(segycore PUBLIC include)
target_compile_options(segycore PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/arch:AVX2 /W4 /wd4100>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-mavx2 -Wall -Wextra -Wno-unused-parameter>
)
if(NOT MSVC)
    target_link_libraries(segycore PUBLIC pthread)
endif()
if(SEGY_FS_LIBS)
    target_link_libraries(segycore PUBLIC ${SEGY_FS_LIBS}) # propagates to everything below
endif()

# --- segyviewercore: renderer/loader/chrome/analysis/bandpass. Qt-free. -----
add_library(segyviewercore STATIC
    viewer/renderer.cpp
    viewer/loader.cpp
    viewer/chrome.cpp
    viewer/wiggle.cpp
    viewer/analysis.cpp
    viewer/axis_ticks.cpp
    viewer/fft.cpp
    viewer/bandpass.cpp
)
target_include_directories(segyviewercore PUBLIC viewer)
target_link_libraries(segyviewercore PUBLIC segycore)

# --- segyviewer: the Qt shell. ---------------------------------------------
set(SEGY_VIEWER_SOURCES
    viewer/qt/viewer_qt.h
    viewer/qt/viewer_qt.cpp
    viewer/qt/viewer_qt_icons.h
    viewer/qt/viewer_qt_icons.cpp
    viewer/qt/app_icon.qrc # app window/taskbar icon, both platforms -- see app_icon.qrc
    $<$<PLATFORM_ID:Windows>:viewer/qt/app.manifest>
    $<$<PLATFORM_ID:Windows>:viewer/qt/app.rc> # Windows exe icon -- see app.rc
)
if(SEGY_QT_USED EQUAL 6)
    # WIN32 links the GUI subsystem instead of the console one, so no cmd
    # window pops up alongside the app on Windows -- a no-op on Linux. Qt
    # still forwards argv to main() either way (qtmain/qtentrypoint).
    qt_add_executable(segyviewer WIN32 ${SEGY_VIEWER_SOURCES})
else()
    add_executable(segyviewer WIN32 ${SEGY_VIEWER_SOURCES})
endif()
target_link_libraries(segyviewer PRIVATE segyviewercore Qt${SEGY_QT_USED}::Widgets Qt${SEGY_QT_USED}::Svg)

# vcpkg's own post-build DLL copy (VCPKG_APPLOCAL_DEPS) only walks PE import
# dependencies, so it copies Qt Core/Gui/Widgets DLLs etc. but not the
# platform plugin (qwindows.dll) -- that's loaded dynamically by
# QGuiApplication via QPluginLoader, invisible to a dependency walker.
# Without it, Qt has no windowing backend and crashes hard in Qt Core's own
# init code instead of failing gracefully, so this has to be copied
# explicitly. windeployqt would normally do this, but vcpkg's Qt port
# doesn't ship it -- so replicate just the one plugin we actually need.
if(WIN32)
    get_target_property(_qt_widgets_dll Qt${SEGY_QT_USED}::Widgets IMPORTED_LOCATION_RELEASE)
    if(NOT _qt_widgets_dll)
        get_target_property(_qt_widgets_dll Qt${SEGY_QT_USED}::Widgets IMPORTED_LOCATION)
    endif()
    if(_qt_widgets_dll)
        get_filename_component(_qt_bin_dir "${_qt_widgets_dll}" DIRECTORY)
        get_filename_component(_qt_triplet_root "${_qt_bin_dir}" DIRECTORY)
        set(_qt_platform_plugin "${_qt_triplet_root}/Qt${SEGY_QT_USED}/plugins/platforms/qwindows.dll")
        if(EXISTS "${_qt_platform_plugin}")
            add_custom_command(TARGET segyviewer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:segyviewer>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_qt_platform_plugin}" "$<TARGET_FILE_DIR:segyviewer>/platforms/qwindows.dll"
                COMMENT "Deploying Qt platform plugin (qwindows.dll)"
            )
        endif()
    endif()
endif()

# --- segytest ---------------------------------------------------------------
add_executable(segytest test/test_main.cpp)
target_include_directories(segytest PRIVATE test)
target_link_libraries(segytest PRIVATE segyviewercore)

# --- segybench --------------------------------------------------------------
add_executable(segybench bench/bench_main.cpp)
target_include_directories(segybench PRIVATE test)
target_link_libraries(segybench PRIVATE segyviewercore) # wiggle.cpp benchmark needs it; pulls in segycore too

# --- gen_fixture (dev utility: writes a synthetic .sgy for manual testing) --
add_executable(gen_fixture test/gen_fixture_main.cpp src/segy_format.cpp)
target_include_directories(gen_fixture PRIVATE test include)
if(SEGY_FS_LIBS)
    target_link_libraries(gen_fixture PRIVATE ${SEGY_FS_LIBS})
endif()
