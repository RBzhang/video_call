cmake_minimum_required(VERSION 3.16)

project(video_call VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

message(STATUS "OpenCV version: ${OpenCV_VERSION}")
message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "OpenCV libraries: ${OpenCV_LIBS}")
message(STATUS "OpenCV_DIR: ${OpenCV_DIR}")

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
    ${OpenCV_LIBS}
)

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
endif()

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
