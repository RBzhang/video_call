cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(video_call VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(VIDEO_CALL_ENABLE_ASAN "Enable AddressSanitizer for MSVC builds" OFF)

if(VIDEO_CALL_ENABLE_ASAN AND MSVC)
    # MSVC AddressSanitizer is incompatible with Edit-and-Continue and runtime checks.
    string(REPLACE "/ZI" "/Zi" _video_call_debug_flags "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REGEX REPLACE "[ ]*/RTC[1su][ ]*" " " _video_call_debug_flags "${_video_call_debug_flags}")
    set(CMAKE_CXX_FLAGS_DEBUG "${_video_call_debug_flags}" CACHE STRING
        "Flags used by the C++ compiler during DEBUG builds." FORCE)

    get_filename_component(_video_call_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(VIDEO_CALL_ASAN_RUNTIME_DLL
            "${_video_call_msvc_bin_dir}/clang_rt.asan_dynamic-x86_64.dll")
    else()
        set(VIDEO_CALL_ASAN_RUNTIME_DLL
            "${_video_call_msvc_bin_dir}/clang_rt.asan_dynamic-i386.dll")
    endif()
    if(NOT EXISTS "${VIDEO_CALL_ASAN_RUNTIME_DLL}")
        message(FATAL_ERROR
            "MSVC AddressSanitizer runtime is missing: ${VIDEO_CALL_ASAN_RUNTIME_DLL}. "
            "Install the C++ AddressSanitizer component for this toolchain.")
    endif()
    message(STATUS "MSVC AddressSanitizer runtime: ${VIDEO_CALL_ASAN_RUNTIME_DLL}")
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS
    Gui
    Widgets
    Network
    Multimedia
)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS
    Gui
    Widgets
    Network
    Multimedia
)

if(NOT DEFINED OpenCV_DIR)
    set(OpenCV_DIR
        "C:/Opencv/opencv/build/x64/vc16/lib"
        CACHE PATH
        "Directory containing OpenCVConfig.cmake"
    )
endif()

find_package(OpenCV 4.12 REQUIRED COMPONENTS
    core
    imgcodecs
    imgproc
    videoio
)

set(VIDEO_CALL_OPENCV_TARGETS
    opencv_core
    opencv_imgcodecs
    opencv_imgproc
    opencv_videoio
)

set(VIDEO_CALL_QT_RUNTIME_TARGETS
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Multimedia
)

if(WIN32)
    get_target_property(VIDEO_CALL_QT_CORE_DEBUG_DLL
        Qt${QT_VERSION_MAJOR}::Core
        IMPORTED_LOCATION_DEBUG)
    get_target_property(VIDEO_CALL_QT_CORE_RELEASE_DLL
        Qt${QT_VERSION_MAJOR}::Core
        IMPORTED_LOCATION_RELEASE)
    foreach(qt_runtime_dll IN ITEMS
            "${VIDEO_CALL_QT_CORE_DEBUG_DLL}"
            "${VIDEO_CALL_QT_CORE_RELEASE_DLL}")
        if(NOT qt_runtime_dll OR qt_runtime_dll MATCHES "-NOTFOUND$" OR NOT EXISTS "${qt_runtime_dll}")
            message(FATAL_ERROR
                "Qt${QT_VERSION_MAJOR}::Core is missing a Debug or Release runtime DLL; "
                "cannot configure Windows deployment.")
        endif()
    endforeach()

    get_filename_component(VIDEO_CALL_QT_BIN_DIR
        "${VIDEO_CALL_QT_CORE_DEBUG_DLL}"
        DIRECTORY)
    get_filename_component(VIDEO_CALL_QT_PREFIX_DIR
        "${VIDEO_CALL_QT_BIN_DIR}"
        DIRECTORY)
    set(VIDEO_CALL_QT_PLUGIN_DIR "${VIDEO_CALL_QT_PREFIX_DIR}/plugins")
    set(VIDEO_CALL_QT_PLATFORM_PLUGIN_DEBUG
        "${VIDEO_CALL_QT_PLUGIN_DIR}/platforms/qwindowsd.dll")
    set(VIDEO_CALL_QT_PLATFORM_PLUGIN_RELEASE
        "${VIDEO_CALL_QT_PLUGIN_DIR}/platforms/qwindows.dll")
    foreach(qt_platform_plugin IN ITEMS
            "${VIDEO_CALL_QT_PLATFORM_PLUGIN_DEBUG}"
            "${VIDEO_CALL_QT_PLATFORM_PLUGIN_RELEASE}")
        if(NOT EXISTS "${qt_platform_plugin}")
            message(FATAL_ERROR "Qt Windows platform plugin is missing: ${qt_platform_plugin}")
        endif()
    endforeach()
endif()

function(video_call_require_opencv_configuration target configuration)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "OpenCV imported target '${target}' is unavailable. Check OpenCV_DIR: ${OpenCV_DIR}")
    endif()

    string(TOUPPER "${configuration}" configuration_upper)
    get_target_property(import_library ${target} IMPORTED_IMPLIB_${configuration_upper})
    get_target_property(runtime_library ${target} IMPORTED_LOCATION_${configuration_upper})
    if(NOT import_library OR import_library MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
            "OpenCV target '${target}' has no ${configuration} import library. "
            "Install matching Debug and Release OpenCV libraries; do not mix CRT configurations.")
    endif()
    if(NOT runtime_library OR runtime_library MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
            "OpenCV target '${target}' has no ${configuration} runtime DLL. "
            "Install matching Debug and Release OpenCV libraries; do not rely on PATH to select a DLL.")
    endif()
    if(NOT EXISTS "${import_library}" OR NOT EXISTS "${runtime_library}")
        message(FATAL_ERROR
            "OpenCV target '${target}' ${configuration} files are missing:\n"
            "  import library: ${import_library}\n"
            "  runtime DLL: ${runtime_library}")
    endif()
    message(STATUS "OpenCV ${target} ${configuration}: implib=${import_library}; dll=${runtime_library}")
endfunction()

foreach(opencv_target IN LISTS VIDEO_CALL_OPENCV_TARGETS)
    video_call_require_opencv_configuration(${opencv_target} DEBUG)
    video_call_require_opencv_configuration(${opencv_target} RELEASE)
endforeach()

function(video_call_configure_target target)
    if(MSVC)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        if(VIDEO_CALL_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
            target_link_options(${target} PRIVATE /INCREMENTAL:NO)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VIDEO_CALL_ASAN_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
    endif()
endfunction()

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        videodisplaylabel.cpp
        videodisplaylabel.h
        cameraworker.cpp
        cameraworker.h
        jpegframeencoder.cpp
        jpegframeencoder.h
        jpegframedecoder.cpp
        jpegframedecoder.h
        audiopacketprotocol.cpp
        audiopacketprotocol.h
        audiojitterbuffer.cpp
        audiojitterbuffer.h
        audioudptransport.cpp
        audioudptransport.h
        audioworker.cpp
        audioworker.h
        remotevideodecoder.cpp
        remotevideodecoder.h
        videoframerateutils.cpp
        videoframerateutils.h
        videopacketprotocol.cpp
        videopacketprotocol.h
        videoframereassembler.cpp
        videoframereassembler.h
        videoudptransport.cpp
        videoudptransport.h
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(video_call
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET video_call APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(video_call SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(video_call
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(video_call PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Multimedia
    ${VIDEO_CALL_OPENCV_TARGETS}
)

if(WIN32)
    foreach(runtime_target IN LISTS VIDEO_CALL_QT_RUNTIME_TARGETS VIDEO_CALL_OPENCV_TARGETS)
        add_custom_command(TARGET video_call POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${runtime_target}>"
                "$<TARGET_FILE_DIR:video_call>"
            VERBATIM)
    endforeach()
    add_custom_command(TARGET video_call POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:video_call>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${VIDEO_CALL_QT_PLATFORM_PLUGIN_DEBUG},${VIDEO_CALL_QT_PLATFORM_PLUGIN_RELEASE}>"
            "$<TARGET_FILE_DIR:video_call>/platforms"
        VERBATIM)
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(video_packet_protocol_test
        tests/videopacketprotocol_test.cpp
        videopacketprotocol.cpp
        videopacketprotocol.h
    )

    target_link_libraries(video_packet_protocol_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
    )

    target_include_directories(video_packet_protocol_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_test(
        NAME video_packet_protocol_test
        COMMAND video_packet_protocol_test
    )

    add_executable(video_frame_reassembler_test
        tests/videoframereassembler_test.cpp
        videoframereassembler.cpp
        videoframereassembler.h
        videopacketprotocol.cpp
        videopacketprotocol.h
    )

    target_link_libraries(video_frame_reassembler_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Network
    )

    target_include_directories(video_frame_reassembler_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_test(
        NAME video_frame_reassembler_test
        COMMAND video_frame_reassembler_test
    )

    add_executable(video_udp_transport_test
        tests/videoudptransport_test.cpp
        videoudptransport.cpp
        videoudptransport.h
        videoframereassembler.cpp
        videoframereassembler.h
        videopacketprotocol.cpp
        videopacketprotocol.h
    )

    target_link_libraries(video_udp_transport_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Network
    )

    target_include_directories(video_udp_transport_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_test(
        NAME video_udp_transport_test
        COMMAND video_udp_transport_test
    )

    add_executable(jpeg_frame_encoder_test
        tests/jpegframeencoder_test.cpp
        jpegframeencoder.cpp
        jpegframeencoder.h
        videopacketprotocol.h
    )

    target_link_libraries(jpeg_frame_encoder_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        opencv_core
        opencv_imgcodecs
    )

    target_include_directories(jpeg_frame_encoder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${OpenCV_INCLUDE_DIRS}
    )

    add_test(
        NAME jpeg_frame_encoder_test
        COMMAND jpeg_frame_encoder_test
    )

    add_executable(jpeg_frame_decoder_test
        tests/jpegframedecoder_test.cpp
        jpegframedecoder.cpp
        jpegframedecoder.h
        videopacketprotocol.h
    )

    target_link_libraries(jpeg_frame_decoder_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        opencv_core
        opencv_imgcodecs
        opencv_imgproc
    )

    target_include_directories(jpeg_frame_decoder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_test(
        NAME jpeg_frame_decoder_test
        COMMAND jpeg_frame_decoder_test
    )

    add_executable(audio_packet_protocol_test
        tests/audiopacketprotocol_test.cpp
        audiopacketprotocol.cpp
        audiopacketprotocol.h
    )

    target_link_libraries(audio_packet_protocol_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
    )

    target_include_directories(audio_packet_protocol_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_test(
        NAME audio_packet_protocol_test
        COMMAND audio_packet_protocol_test
    )

    add_executable(audio_jitter_buffer_test
        tests/audiojitterbuffer_test.cpp
        audiojitterbuffer.cpp
        audiojitterbuffer.h
        audiopacketprotocol.h
    )

    target_link_libraries(audio_jitter_buffer_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
    )

    target_include_directories(audio_jitter_buffer_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_test(
        NAME audio_jitter_buffer_test
        COMMAND audio_jitter_buffer_test
    )

    add_executable(audio_udp_transport_test
        tests/audioudptransport_test.cpp
        audioudptransport.cpp
        audioudptransport.h
        audiopacketprotocol.cpp
        audiopacketprotocol.h
    )

    target_link_libraries(audio_udp_transport_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Network
    )

    target_include_directories(audio_udp_transport_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_test(
        NAME audio_udp_transport_test
        COMMAND audio_udp_transport_test
    )

    add_executable(video_frame_rate_utils_test
        tests/videoframerateutils_test.cpp
        videoframerateutils.cpp
        videoframerateutils.h
    )

    target_link_libraries(video_frame_rate_utils_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
    )

    target_include_directories(video_frame_rate_utils_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_test(
        NAME video_frame_rate_utils_test
        COMMAND video_frame_rate_utils_test
    )

    set(mainwindow_exit_smoke_sources ${PROJECT_SOURCES})
    list(REMOVE_ITEM mainwindow_exit_smoke_sources main.cpp)
    add_executable(mainwindow_exit_smoke_test
        tests/mainwindow_exit_smoke_test.cpp
        ${mainwindow_exit_smoke_sources}
    )

    target_link_libraries(mainwindow_exit_smoke_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Network
        Qt${QT_VERSION_MAJOR}::Multimedia
        ${VIDEO_CALL_OPENCV_TARGETS}
    )

    target_include_directories(mainwindow_exit_smoke_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_test(
        NAME mainwindow_exit_smoke_test
        COMMAND mainwindow_exit_smoke_test
    )
    set_tests_properties(mainwindow_exit_smoke_test PROPERTIES TIMEOUT 20)

    # CTest does not inherit Qt Creator's run environment. The smoke test
    # creates a QApplication, so point it at the platform plugin directory
    # explicitly instead of relying on a developer's PATH/QT_PLUGIN_PATH.
    if(WIN32)
        set_tests_properties(mainwindow_exit_smoke_test PROPERTIES
            ENVIRONMENT
                "QT_PLUGIN_PATH=${VIDEO_CALL_QT_PLUGIN_DIR};QT_QPA_PLATFORM_PLUGIN_PATH=${VIDEO_CALL_QT_PLUGIN_DIR}/platforms")
    endif()

    set(VIDEO_CALL_TEST_TARGETS
        video_packet_protocol_test
        video_frame_reassembler_test
        video_udp_transport_test
        jpeg_frame_encoder_test
        jpeg_frame_decoder_test
        audio_packet_protocol_test
        audio_jitter_buffer_test
        audio_udp_transport_test
        video_frame_rate_utils_test
        mainwindow_exit_smoke_test
    )
endif()

video_call_configure_target(video_call)
foreach(video_call_test_target IN LISTS VIDEO_CALL_TEST_TARGETS)
    video_call_configure_target(${video_call_test_target})
endforeach()

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.video_call)
endif()
set_target_properties(video_call PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
# Keep the install tree intentionally minimal: the packaging script adds only
# runtime dependencies next to this executable.
install(TARGETS video_call RUNTIME DESTINATION .)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(video_call)
endif()
