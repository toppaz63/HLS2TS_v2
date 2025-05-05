cmake_minimum_required(VERSION 3.14)
project(hls-to-dvb VERSION 1.0.0)

# C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options de compilation
option(BUILD_TESTS "Build tests" OFF)
option(ENABLE_SANITIZERS "Enable sanitizers in debug builds" OFF)

# Configuration des répertoires
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Création du répertoire de logs lors de la compilation
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/logs)

# Trouver les packages requis
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Configuration directe pour OpenSSL 3.5.0 sur macOS
if(APPLE)
    # Définir directement le chemin vers votre version d'OpenSSL
    set(OPENSSL_ROOT_DIR "/opt/homebrew/Cellar/openssl@3/3.5.0")
    
    # Vérifier que ce chemin existe
    if(EXISTS "${OPENSSL_ROOT_DIR}")
        message(STATUS "Using OpenSSL at: ${OPENSSL_ROOT_DIR}")
        
        # Configurer les chemins pour les en-têtes et les bibliothèques
        set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
        set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libcrypto.dylib")
        set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libssl.dylib")
        
        # Vérifier que les fichiers existent
        if(EXISTS "${OPENSSL_INCLUDE_DIR}/openssl/ssl.h" AND 
           EXISTS "${OPENSSL_CRYPTO_LIBRARY}" AND 
           EXISTS "${OPENSSL_SSL_LIBRARY}")
            set(OPENSSL_FOUND TRUE)
            message(STATUS "OpenSSL configuration successful")
            message(STATUS "  Include dir: ${OPENSSL_INCLUDE_DIR}")
            message(STATUS "  SSL lib: ${OPENSSL_SSL_LIBRARY}")
            message(STATUS "  Crypto lib: ${OPENSSL_CRYPTO_LIBRARY}")
            
            # Inclure OpenSSL dans le chemin d'include global
            include_directories(${OPENSSL_INCLUDE_DIR})
        else()
            message(WARNING "OpenSSL files not found at ${OPENSSL_ROOT_DIR}")
        endif()
    else()
        message(WARNING "OpenSSL directory not found at ${OPENSSL_ROOT_DIR}")
        # Essayer d'utiliser la méthode standard
        find_package(OpenSSL)
    endif()
else()
    # Sur les autres plateformes, utiliser find_package standard
    find_package(OpenSSL)
endif()



# Ajouter les chemins de modules CMake personnalisés
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# TSDuck - d'abord essayer pkg-config
pkg_check_modules(TSDUCK tsduck)
if(NOT TSDUCK_FOUND)
    # Recherche manuelle si pkg-config échoue
    find_path(TSDUCK_INCLUDE_DIRS tsduck/tsduck.h
        PATHS
            /opt/homebrew/Cellar/tsduck/*/include
            /opt/homebrew/include
            /usr/local/include
            /usr/include
    )
    
    # Rechercher spécifiquement le fichier de bibliothèque
    find_library(TSDUCK_LIBRARIES_FULL_PATH
        NAMES libtsduck.dylib tsduck
        PATHS
            /opt/homebrew/Cellar/tsduck/*/lib
            /opt/homebrew/lib
            /usr/local/lib
            /usr/lib
    )
    
    if(TSDUCK_INCLUDE_DIRS AND TSDUCK_LIBRARIES_FULL_PATH)
        set(TSDUCK_FOUND TRUE)
        get_filename_component(TSDUCK_LIB_DIR ${TSDUCK_LIBRARIES_FULL_PATH} DIRECTORY)
        set(TSDUCK_LIBRARIES tsduck)
        set(TSDUCK_LIBRARY_DIRS ${TSDUCK_LIB_DIR})
        message(STATUS "TSDuck found manually: ${TSDUCK_LIBRARIES_FULL_PATH}")
    else()
        message(FATAL_ERROR "TSDuck library not found. Please install TSDuck.")
    endif()
endif()

# FFmpeg
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)

# Recherche des bibliothèques et en-têtes tiers
find_package(spdlog QUIET)
find_package(fmt QUIET)
find_package(nlohmann_json QUIET)

# Inclure les répertoires d'en-têtes
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${TSDUCK_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    /opt/homebrew/Cellar/spdlog/1.15.2/include/
    /opt/homebrew/Cellar/fmt/11.1.4/include/
    /opt/homebrew/Cellar/nlohmann-json/3.12.0/include/
    /opt/homebrew/Cellar/cpp-httplib/0.20.0/include
)

# Inclusion conditionnelle des dépendances
if(NOT spdlog_FOUND)
    message(STATUS "spdlog package not found, looking in standard paths")
    include_directories(
        /usr/include/spdlog
        /usr/local/include/spdlog
        /opt/homebrew/include/spdlog
    )
endif()

if(NOT fmt_FOUND)
    message(STATUS "fmt package not found, looking in standard paths")
    include_directories(
        /usr/include/fmt
        /usr/local/include/fmt
        /opt/homebrew/include/fmt
    )
endif()

if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json package not found, looking in standard paths")
    include_directories(
        /usr/include/nlohmann
        /usr/local/include/nlohmann
        /opt/homebrew/include/nlohmann
    )
endif()

# Chercher httplib
find_path(HTTPLIB_INCLUDE_DIR httplib.h
    PATHS
        /usr/include
        /usr/local/include
        /opt/homebrew/include
        /opt/homebrew/Cellar/cpp-httplib/*/include
)

if(HTTPLIB_INCLUDE_DIR)
    include_directories(${HTTPLIB_INCLUDE_DIR})
else()
    message(FATAL_ERROR "Failed to find httplib.h. Please install cpp-httplib.")
endif()

# Afficher des informations de débogage sur TSDuck
message(STATUS "TSDUCK_FOUND: ${TSDUCK_FOUND}")
message(STATUS "TSDUCK_INCLUDE_DIRS: ${TSDUCK_INCLUDE_DIRS}")
message(STATUS "TSDUCK_LIBRARIES: ${TSDUCK_LIBRARIES}")
message(STATUS "TSDUCK_LIBRARY_DIRS: ${TSDUCK_LIBRARY_DIRS}")
if(TSDUCK_LIBRARIES_FULL_PATH)
    message(STATUS "TSDUCK_LIBRARIES_FULL_PATH: ${TSDUCK_LIBRARIES_FULL_PATH}")
endif()

# Ajouter explicitement les répertoires de bibliothèques à la recherche
link_directories(
    ${TSDUCK_LIBRARY_DIRS}
    ${FFMPEG_LIBRARY_DIRS}
    /opt/homebrew/lib
    /usr/local/lib
)

# Sources pour l'exécutable principal
set(SOURCES
    src/main.cpp
    src/core/Application.cpp
    src/core/config.cpp
    src/core/StreamManager.cpp
    src/core/SegmentBuffer.cpp
    src/alerting/AlertManager.cpp
    src/hls/HLSClient.cpp
    src/mpegts/MPEGTSConverter.cpp
    src/mpegts/DVBProcessor.cpp
    src/multicast/MulticastSender.cpp
    src/web/WebServer.cpp
)

# Créer l'exécutable
add_executable(hls-to-dvb ${SOURCES})

# Lier avec les bibliothèques
if(TSDUCK_LIBRARIES_FULL_PATH)
    # Si nous avons le chemin complet, l'utiliser directement
    target_link_libraries(hls-to-dvb ${TSDUCK_LIBRARIES_FULL_PATH})
else()
    # Sinon, utiliser le nom de la bibliothèque
    target_link_libraries(hls-to-dvb ${TSDUCK_LIBRARIES})
endif()

# Lier avec les bibliothèques requises
target_link_libraries(hls-to-dvb
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)

# Lier avec OpenSSL si trouvé
if(OPENSSL_FOUND)
    # Si nous utilisons notre configuration manuelle sur macOS
    if(APPLE AND OPENSSL_SSL_LIBRARY AND OPENSSL_CRYPTO_LIBRARY)
        target_link_libraries(hls-to-dvb
            ${OPENSSL_SSL_LIBRARY}
            ${OPENSSL_CRYPTO_LIBRARY}
        )
    elseif(OpenSSL_FOUND)
        # Si find_package a trouvé OpenSSL
        target_link_libraries(hls-to-dvb
            OpenSSL::SSL
            OpenSSL::Crypto
        )
    endif()
endif()

# Ajouter -ldl sur les plateformes qui le supportent
if(UNIX AND NOT APPLE)
    target_link_libraries(hls-to-dvb -ldl)
endif()

# Ajouter les liens vers les bibliothèques si elles ont été trouvées en tant que packages
if(spdlog_FOUND)
    target_link_libraries(hls-to-dvb spdlog::spdlog)
endif()

if(fmt_FOUND)
    target_link_libraries(hls-to-dvb fmt::fmt)
endif()

if(nlohmann_json_FOUND)
    target_link_libraries(hls-to-dvb nlohmann_json::nlohmann_json)
endif()

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

# Définir une option de compilation pour indiquer si OpenSSL est disponible
if(OPENSSL_FOUND)
    target_compile_definitions(hls-to-dvb PRIVATE HAVE_OPENSSL=1)
else()
    target_compile_definitions(hls-to-dvb PRIVATE HAVE_OPENSSL=0)
endif()

# Copier les fichiers de configuration
file(COPY ${CMAKE_SOURCE_DIR}/config DESTINATION ${CMAKE_BINARY_DIR})

# Créer le répertoire web si nécessaire
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/web)

# Copier les fichiers statiques de l'interface web si le répertoire existe
if(EXISTS ${CMAKE_SOURCE_DIR}/src/web/static)
    file(COPY ${CMAKE_SOURCE_DIR}/src/web/static DESTINATION ${CMAKE_BINARY_DIR}/web)
endif()

# Tests (conditionnels)
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests)
        add_subdirectory(tests)
    else()
        message(WARNING "BUILD_TESTS is ON but tests directory not found")
    endif()
endif()

# Documentation des commandes de compilation
message(STATUS "Configuration terminée. Pour compiler le projet :")
message(STATUS "  mkdir -p build && cd build")
message(STATUS "  cmake ..")
message(STATUS "  make")
message(STATUS "Pour exécuter l'application :")
message(STATUS "  ./bin/hls-to-dvb ../config/config.json")
