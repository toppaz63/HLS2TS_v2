# Convertisseur HLS vers MPEG-TS Multicast DVB

Application C++ pour convertir des flux HLS (HTTP Live Streaming) contenant des marqueurs de discontinuité en flux MPEG-TS multicast conformes à la norme DVB.

## Fonctionnalités

- Conversion de flux HLS vers MPEG-TS multicast
- Gestion des marqueurs de discontinuité HLS
- Conformité avec les spécifications DVB pour les tables PSI/SI
- Interface web pour la gestion des flux
- Configuration via fichier JSON
- Système d'alertes pour la surveillance des flux

## Prérequis

- CMake 3.14 ou supérieur
- Compilateur C++ compatible C++17 (GCC 7+, Clang 5+, MSVC 2017+)
- Bibliothèques requises :
  - FFmpeg (libavformat, libavcodec, libavutil)
  - TSDuck
  - OpenSSL
  - spdlog
  - fmt
  - nlohmann_json
  - cpp-httplib

### Installation des dépendances

#### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev
sudo apt-get install -y libavcodec-dev libavformat-dev libavutil-dev
sudo apt-get install -y libspdlog-dev libfmt-dev nlohmann-json3-dev
