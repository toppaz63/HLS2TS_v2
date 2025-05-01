#include "MulticastSender.h"
#include "../alerting/AlertManager.h"
#include "spdlog/spdlog.h"

#include <chrono>
#include <cstring>
#include <iostream>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define close closesocket
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

// Constantes
constexpr size_t PACKET_SIZE = 1316;    // Taille des paquets UDP (7 paquets TS de 188 octets)
constexpr int DEFAULT_TTL = 64;         // TTL par défaut pour les paquets multicast

MulticastSender::MulticastSender(const std::string& multicastAddr, int multicastPort)
    : multicastAddr_(multicastAddr), multicastPort_(multicastPort), socket_(-1),
      running_(false), packetsTransmitted_(0), currentBitrate_(0.0) {
    
    #ifdef _WIN32
    // Initialisation de Winsock sur Windows
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        throw std::runtime_error("Erreur lors de l'initialisation de Winsock");
    }
    #endif
}

void MulticastSender::start() {
    if (running_) {
        spdlog::warn("L'émetteur multicast est déjà en cours d'exécution");
        return;
    }
    
    try {
        // Création du socket UDP
        socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) {
            throw std::runtime_error("Erreur lors de la création du socket");
        }
        
        // Configuration du socket pour le multicast
        int reuse = 1;
        if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0) {
            throw std::runtime_error("Erreur lors de la configuration du socket (SO_REUSEADDR)");
        }
        
        // Configuration du TTL multicast
        int ttl = DEFAULT_TTL;
        if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl)) < 0) {
            throw std::runtime_error("Erreur lors de la configuration du TTL multicast");
        }
        
        // Préparation de l'adresse de destination
        memset(&destAddr_, 0, sizeof(destAddr_));
        destAddr_.sin_family = AF_INET;
        destAddr_.sin_addr.s_addr = inet_addr(multicastAddr_.c_str());
        destAddr_.sin_port = htons(multicastPort_);
        
        running_ = true;
        
        spdlog::info("Émetteur multicast démarré avec succès: {}:{}", multicastAddr_, multicastPort_);
        
        // Enregistrer une alerte de démarrage
        AlertManager::getInstance().addAlert(
            AlertLevel::INFO,
            "MulticastSender",
            "Émetteur multicast démarré: " + multicastAddr_ + ":" + std::to_string(multicastPort_),
            false
        );
    }
    catch (const std::exception& e) {
        // En cas d'erreur, fermer le socket s'il a été créé
        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }
        
        spdlog::error("Erreur lors du démarrage de l'émetteur multicast: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "MulticastSender",
            std::string("Erreur lors du démarrage de l'émetteur multicast: ") + e.what(),
            true
        );
        
        throw;
    }
}

void MulticastSender::stop() {
    if (!running_) {
        spdlog::warn("L'émetteur multicast n'est pas en cours d'exécution");
        return;
    }
    
    spdlog::info("Arrêt de l'émetteur multicast: {}:{}", multicastAddr_, multicastPort_);
    
    // Fermer le socket
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    
    running_ = false;
    
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "MulticastSender",
        "Émetteur multicast arrêté: " + multicastAddr_ + ":" + std::to_string(multicastPort_),
        false
    );
}

bool MulticastSender::send(const MPEGTSSegment& segment) {
    if (!running_ || socket_ < 0) {
        spdlog::error("Tentative d'envoi alors que l'émetteur multicast n'est pas démarré");
        return false;
    }
    
    try {
        // Horodatage pour le calcul du débit
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // Diviser le segment en paquets UDP de taille PACKET_SIZE
        const uint8_t* data = segment.data.data();
        size_t remaining = segment.data.size();
        size_t offset = 0;
        size_t packetCount = 0;
        
        while (remaining > 0) {
            size_t chunkSize = std::min(remaining, PACKET_SIZE);
            
            ssize_t sentBytes = sendto(socket_, 
                                       reinterpret_cast<const char*>(data + offset), 
                                       chunkSize, 
                                       0, 
                                       (struct sockaddr*)&destAddr_, 
                                       sizeof(destAddr_));
            
            if (sentBytes < 0) {
                throw std::runtime_error("Erreur lors de l'envoi des données");
            }
            
            offset += chunkSize;
            remaining -= chunkSize;
            packetCount++;
        }
        
        // Mettre à jour les statistiques
        packetsTransmitted_ += packetCount;
        
        // Calculer le débit binaire
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
        
        if (duration.count() > 0) {
            // Débit en bits par seconde (bps)
            double bitrate = (segment.data.size() * 8.0 * 1000000.0) / duration.count();
            currentBitrate_ = bitrate;
            
            // Journaliser le débit tous les N paquets (pour ne pas saturer les logs)
            if (packetsTransmitted_ % 100 == 0) {
                spdlog::debug("Débit de transmission actuel: {:.2f} Mbps", bitrate / 1000000.0);
            }
        }
        
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de l'envoi du segment: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "MulticastSender",
            std::string("Erreur lors de l'envoi du segment: ") + e.what(),
            true
        );
        
        return false;
    }
}

size_t MulticastSender::getPacketsTransmitted() const {
    return packetsTransmitted_;
}

double MulticastSender::getCurrentBitrate() const {
    return currentBitrate_;
}

bool MulticastSender::isRunning() const {
    return running_;
}

MulticastSender::~MulticastSender() {
    if (running_) {
        stop();
    }
    
    #ifdef _WIN32
    // Nettoyage de Winsock sur Windows
    WSACleanup();
    #endif
}
