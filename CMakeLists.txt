cmake_minimum_required(VERSION 3.21)

# Static CRT on Windows so the DLLs load inside the Windows camera service and any app with no extra runtimes.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(PS3EyeVCam VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS3EYE_BUILD_TESTS "Build the portable unit tests" ON)
# The 32-bit DirectShow filter (for 32-bit apps on Windows 10) needs nothing but the pixel
# conversions, so its build skips libusb, vcpkg and everything else:
#   cmake -S . -B build32 -A Win32 -DPS3EYE_FILTER_ONLY=ON
option(PS3EYE_FILTER_ONLY "Only build the DirectShow filter DLL" OFF)

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /EHsc /MP /Zc:__cplusplus)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# ---- pixel conversions (portable, no dependencies) -------------------------------
add_library(ps3eye_convert STATIC src/core/convert.cpp)
target_include_directories(ps3eye_convert PUBLIC src/core)

# ---- DirectShow virtual camera filter (Windows 10 path) ---------------------------
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(PS3EYE_DSHOW_NAME PS3EyeDShow64)
    else()
        set(PS3EYE_DSHOW_NAME PS3EyeDShow32)
    endif()
    add_library(PS3EyeDShow SHARED
        src/dshow/DShowMain.cpp
        src/dshow/Filter.cpp
        src/dshow/MediaTypes.cpp
        src/dshow/SharedFrames.cpp
        src/source/Config.cpp
        src/source/Controls.cpp
        src/dshow/PS3EyeDShow.def)
    set_target_properties(PS3EyeDShow PROPERTIES OUTPUT_NAME ${PS3EYE_DSHOW_NAME})
    target_link_libraries(PS3EyeDShow PRIVATE ps3eye_convert strmiids ole32 oleaut32 advapi32 shell32)
endif()

if(PS3EYE_FILTER_ONLY)
    return()
endif()

# ---- libusb -------------------------------------------------------------------
find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)
# vcpkg searches its debug/lib first with Visual Studio generators, so look up the release
# and debug builds next to the headers explicitly, then fall back to a normal search (Linux).
get_filename_component(LIBUSB_ROOT "${LIBUSB_INCLUDE_DIR}/../.." ABSOLUTE)
find_library(LIBUSB_LIBRARY_RELEASE NAMES libusb-1.0 usb-1.0 PATHS "${LIBUSB_ROOT}/lib" NO_DEFAULT_PATH)
find_library(LIBUSB_LIBRARY_RELEASE NAMES libusb-1.0 usb-1.0 REQUIRED)
find_library(LIBUSB_LIBRARY_DEBUG NAMES libusb-1.0 usb-1.0 PATHS "${LIBUSB_ROOT}/debug/lib" NO_DEFAULT_PATH)
include(SelectLibraryConfigurations)
select_library_configurations(LIBUSB)

# ---- portable camera core (libusb driver + pixel conversion) -------------------
add_library(ps3eye_core STATIC src/core/ps3eye_camera.cpp)
target_include_directories(ps3eye_core PUBLIC src/core PRIVATE ${LIBUSB_INCLUDE_DIR})
target_link_libraries(ps3eye_core PUBLIC ps3eye_convert ${LIBUSB_LIBRARIES})
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(ps3eye_core PUBLIC Threads::Threads)
endif()

if(PS3EYE_BUILD_TESTS)
    enable_testing()
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE ps3eye_core)
    add_test(NAME core COMMAND test_core)
endif()

if(WIN32)
    find_package(wil CONFIG REQUIRED)
    find_package(cppwinrt CONFIG REQUIRED)

    set(MF_LIBS mfplat mf mfuuid mfsensorgroup ole32 oleaut32 propsys shlwapi advapi32 shell32 windowsapp)

    # ---- the virtual camera media source (Windows 11, loaded by the Windows Camera Frame Server)
    add_library(PS3EyeVCam SHARED
        src/source/Activator.cpp
        src/source/Config.cpp
        src/source/Controls.cpp
        src/source/EnumNames.cpp
        src/source/MediaSource.cpp
        src/source/MediaStream.cpp
        src/source/MFTools.cpp
        src/source/Tools.cpp
        src/source/WinTrace.cpp
        src/source/dllmain.cpp
        src/source/PS3EyeVCam.def)
    target_compile_definitions(PS3EyeVCam PRIVATE _WINDOWS _USRDLL)
    target_link_libraries(PS3EyeVCam PRIVATE ps3eye_core WIL::WIL Microsoft::CppWinRT ${MF_LIBS})

    # ---- Windows 10: the process that owns the camera and shares frames with the DirectShow filters
    add_executable(PS3EyeServer WIN32
        src/dshow/Server.cpp
        src/dshow/SharedFrames.cpp
        src/source/Config.cpp)
    target_link_libraries(PS3EyeServer PRIVATE ps3eye_core advapi32 shell32 ole32)

    # ---- setup / test / settings tool
    add_executable(PS3EyeSetup
        src/setup/setup.cpp
        src/setup/usbdriver.cpp
        src/source/Config.cpp
        src/setup/setup.manifest)
    target_link_libraries(PS3EyeSetup PRIVATE ps3eye_core ${MF_LIBS} strmiids setupapi newdev cfgmgr32)
    target_link_options(PS3EyeSetup PRIVATE /MANIFESTUAC:NO)   # UAC level comes from setup.manifest
endif()
