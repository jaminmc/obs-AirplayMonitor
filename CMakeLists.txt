cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  # obs-deps Qt still asks for AGL. The macOS 26 SDK does not ship that framework.
  if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_airplay_opengl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    set(_airplay_opengl_filtered "")
    foreach(_airplay_opengl_lib IN LISTS _airplay_opengl_libs)
      if(_airplay_opengl_lib MATCHES "AGL")
        continue()
      endif()
      list(APPEND _airplay_opengl_filtered "${_airplay_opengl_lib}")
    endforeach()
    set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_airplay_opengl_filtered}")
  endif()
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/airplay/plist.cpp
    src/airplay/plist.hpp
    src/airplay/features.cpp
    src/airplay/features.hpp
    src/airplay/mirror_packet.cpp
    src/airplay/mirror_packet.hpp
    src/airplay/rtsp.cpp
    src/airplay/rtsp.hpp
    src/airplay/ntp.cpp
    src/airplay/ntp.hpp
    src/airplay/crypto.cpp
    src/airplay/crypto.hpp
    src/airplay/aead.cpp
    src/airplay/aead.hpp
    src/airplay/pairing.cpp
    src/airplay/pairing.hpp
    src/airplay/nacl_random.c
    third_party/tweetnacl/tweetnacl.c
    src/airplay/fairplay.cpp
    src/airplay/fairplay.hpp
    src/airplay/handshake.cpp
    src/airplay/handshake.hpp
    src/airplay/library.cpp
    src/airplay/library.hpp
    src/airplay/net.hpp
    src/airplay/portable.hpp
    src/airplay/socket.cpp
    src/airplay/socket.hpp
    src/airplay/session.cpp
    src/airplay/session.hpp
    src/airplay/discovery.cpp
    src/airplay/discovery.hpp
    src/airplay/rtp_audio.cpp
    src/airplay/rtp_audio.hpp
    src/encode/bitstream.cpp
    src/encode/bitstream.hpp
    src/encode/encoded_au.hpp
    src/encode/obs_video.cpp
    src/encode/obs_video.hpp
    src/audio/aac_eld.hpp
    src/ui/monitor_dialog.cpp
    src/ui/monitor_dialog.hpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)

set_source_files_properties(third_party/tweetnacl/tweetnacl.c PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")

# AAC-ELD: AudioToolbox on macOS, fdk-aac loaded at run time elsewhere.
if(APPLE)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/audio/aac_eld.mm)
  set_source_files_properties(src/audio/aac_eld.mm PROPERTIES COMPILE_FLAGS "-fno-objc-arc")
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE "-framework CoreFoundation" "-framework AudioToolbox" "-framework Foundation"
  )
else()
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/audio/aac_eld_fdk.cpp)
endif()

if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 bcrypt)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE _USE_MATH_DEFINES)
elseif(NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# FairPlay SAP, the sender side of the legacy AirPlay key exchange (LGPL, see third_party/fairplay-sap).
set(AIRPLAY_FAIRPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fairplay-sap/ports")
set(AIRPLAY_FAIRPLAY_SOURCES
    "${AIRPLAY_FAIRPLAY_DIR}/c/fairplay_sapcore.c"
    "${AIRPLAY_FAIRPLAY_DIR}/c/fairplay_sapcore.h"
    "${AIRPLAY_FAIRPLAY_DIR}/cpp/fairplay_sender.cpp"
    "${AIRPLAY_FAIRPLAY_DIR}/cpp/fairplay_sender.hpp"
    "${AIRPLAY_FAIRPLAY_DIR}/cpp/fairplay_tables.inc"
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${AIRPLAY_FAIRPLAY_SOURCES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${AIRPLAY_FAIRPLAY_DIR}")

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# The protocol tests check against CoreFoundation and Security, and the probe encodes with VideoToolbox.
if(APPLE)
  add_executable(
    airplay_protocol_tests
    tests/test_protocol.cpp
    src/airplay/plist.cpp
    src/airplay/features.cpp
    src/airplay/mirror_packet.cpp
    src/airplay/rtsp.cpp
    src/airplay/ntp.cpp
    src/airplay/rtp_audio.cpp
    src/airplay/crypto.cpp
    src/airplay/aead.cpp
    src/airplay/pairing.cpp
    src/airplay/nacl_random.c
    third_party/tweetnacl/tweetnacl.c
    src/airplay/fairplay.cpp
    src/airplay/handshake.cpp
    src/airplay/discovery.cpp
    src/encode/bitstream.cpp
    ${AIRPLAY_FAIRPLAY_SOURCES}
  )
  target_include_directories(airplay_protocol_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${AIRPLAY_FAIRPLAY_DIR}")
  target_compile_features(airplay_protocol_tests PRIVATE cxx_std_20)
  target_compile_definitions(airplay_protocol_tests PRIVATE AIRPLAY_FAIRPLAY_STUB)
  target_link_libraries(
    airplay_protocol_tests
    PRIVATE "-framework CoreFoundation" "-framework Security"
  )

  # Standalone sender. Same protocol code as the plugin, with a generated picture and tone.
  add_executable(
    airplay_probe
    tools/airplay_probe.cpp
    src/airplay/plist.cpp
    src/airplay/features.cpp
    src/airplay/mirror_packet.cpp
    src/airplay/rtsp.cpp
    src/airplay/ntp.cpp
    src/airplay/rtp_audio.cpp
    src/airplay/crypto.cpp
    src/airplay/aead.cpp
    src/airplay/pairing.cpp
    src/airplay/nacl_random.c
    third_party/tweetnacl/tweetnacl.c
    src/airplay/fairplay.cpp
    src/airplay/handshake.cpp
    src/airplay/socket.cpp
    src/airplay/session.cpp
    src/encode/vt_encoder.mm
    src/audio/aac_eld.mm
    ${AIRPLAY_FAIRPLAY_SOURCES}
  )
  target_include_directories(airplay_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${AIRPLAY_FAIRPLAY_DIR}")
  target_compile_features(airplay_probe PRIVATE cxx_std_20)
  target_compile_options(airplay_probe PRIVATE -Wno-shorten-64-to-32 -Wno-comma)
  set_source_files_properties(
    src/encode/vt_encoder.mm
    src/audio/aac_eld.mm
    TARGET_DIRECTORY airplay_probe
    PROPERTIES COMPILE_FLAGS "-fno-objc-arc"
  )
  target_link_libraries(
    airplay_probe
    PRIVATE
      "-framework VideoToolbox"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework CoreFoundation"
      "-framework AudioToolbox"
      "-framework Security"
      "-framework Foundation"
      "-framework CoreGraphics"
  )
endif()
