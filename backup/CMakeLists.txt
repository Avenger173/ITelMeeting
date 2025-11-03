cmake_minimum_required(VERSION 3.16)

project(SmartMeet VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#添加头文件
include_directories("D:/lib/ffmpeg-7.1.1-full_build-shared/include")
include_directories("D:/Opencv/opencv-4.8.0-windows/opencv/build/include")
#添加库路径
link_directories(
  "D:/lib/ffmpeg-7.1.1-full_build-shared/lib"
  "D:/Opencv/opencv-4.8.0-windows/opencv/build/x64/vc16/lib"
)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network Sql Multimedia)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network Sql Multimedia)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(SmartMeet
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        videocapture.h videocapture.cpp
        audiocapture.h audiocapture.cpp

        avrecorder.h avrecorder.cpp


    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET SmartMeet APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(SmartMeet SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(SmartMeet
            ${PROJECT_SOURCES}
        )
    endif()
endif()
# 设置 FFmpeg 相关库名称（确保是你 .lib 目录中的实际文件名）
set(FFMPEG_LIBS
    avformat
    avcodec
    avutil
    swresample
    swscale
    avdevice
)
target_link_libraries(SmartMeet
  PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Network
  Qt${QT_VERSION_MAJOR}::Sql
  Qt${QT_VERSION_MAJOR}::Multimedia
  "D:/lib/ffmpeg-7.1.1-full_build-shared/lib/avformat.lib"
  "D:/lib/ffmpeg-7.1.1-full_build-shared/lib/avcodec.lib"
  "D:/lib/ffmpeg-7.1.1-full_build-shared/lib/avutil.lib"
  "D:/lib/ffmpeg-7.1.1-full_build-shared/lib/swresample.lib"
  "D:/lib/ffmpeg-7.1.1-full_build-shared/lib/swscale.lib"
  "D:/lib/ffmpeg-7.1.1-full_build-shared/lib/avdevice.lib"
  "D:/Opencv/opencv-4.8.0-windows/opencv/build/x64/vc16/lib/opencv_world480d.lib"
)
#自动拷贝DLL文件到构建目录(仅适用windows+fffmpeg DLL)
add_custom_command(TARGET SmartMeet POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
  "D:/lib/ffmpeg-7.1.1-full_build-shared/bin" $<TARGET_FILE_DIR:SmartMeet>
)
# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.SmartMeet)
endif()
set_target_properties(SmartMeet PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS SmartMeet
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(SmartMeet)
endif()
