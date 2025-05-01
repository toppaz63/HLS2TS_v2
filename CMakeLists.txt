cmake_minimum_required(VERSION 3.14)
project(hls-to-dvb VERSION 1.0.0)

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options de compilation
option(BUILD_TESTS "Build tests" OFF)
option(ENABLE_SANITIZERS "Enable sanitizers in debug builds" OFF)

# Configuration des répertoires
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Trouver les packages requis
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# TSDuck
pkg_check_modules(TSDUCK REQUIRED tsduck)

# FFmpeg
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)

# Bibliothèques header-only (incluses directement)
include_directories(${CMAKE_SOURCE_DIR}/third_party)

# Inclure les répertoires d'en-têtes
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${TSDUCK_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    /opt/homebrew/Cellar/spdlog/1.15.2/include/
    /opt/homebrew/Cellar/fmt/11.1.4/include/
    /opt/homebrew/Cellar/nlohmann-json/3.12.0/include/
    /opt/homebrew/Cellar/cpp-httplib/0.20.0/include
)

# Ajouter les sous-répertoires
add_subdirectory(src)

# Copier les fichiers de configuration
file(COPY ${CMAKE_SOURCE_DIR}/config DESTINATION ${CMAKE_BINARY_DIR})

# Copier les fichiers statiques de l'interface web
file(COPY ${CMAKE_SOURCE_DIR}/src/web/static DESTINATION ${CMAKE_BINARY_DIR}/web)

# Tests (conditionnels)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Documentation des commandes de compilation
message(STATUS "Configuration terminée. Pour compiler le projet :")
message(STATUS "  mkdir build && cd build")
message(STATUS "  cmake ..")
message(STATUS "  make")
message(STATUS "Pour exécuter l'application :")
message(STATUS "  ./bin/hls-to-dvb")
