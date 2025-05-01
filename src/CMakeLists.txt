set(SOURCES
    main.cpp
    core/Application.cpp
    core/Config.cpp
    core/StreamManager.cpp
    core/SegmentBuffer.cpp
    alerting/AlertManager.cpp
    hls/HLSClient.cpp
    mpegts/MPEGTSConverter.cpp
    mpegts/DVBProcessor.cpp
    multicast/MulticastSender.cpp
    web/WebServer.cpp
)

add_executable(hls-to-dvb ${SOURCES})

target_link_libraries(hls-to-dvb
    ${TSDUCK_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    -ldl
)

# Flags de compilation
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hls-to-dvb PRIVATE -Wall -Wextra -pedantic)
    
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND ENABLE_SANITIZERS)
        target_compile_options(hls-to-dvb PRIVATE -fsanitize=address,undefined)
        target_link_options(hls-to-dvb PRIVATE -fsanitize=address,undefined)
    endif()
elseif(MSVC)
    target_compile_options(hls-to-dvb PRIVATE /W4)
endif()