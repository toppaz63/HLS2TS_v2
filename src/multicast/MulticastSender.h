#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <memory>
#include <vector>
#include <chrono>

namespace hls_to_dvb {

/**
 * @brief Structure contenant les statistiques d'émission multicast
 */
struct MulticastStats {
    uint64_t packetsSent = 0;     ///< Nombre total de paquets UDP envoyés
    uint64_t bytesSent = 0;       ///< Nombre total d'octets envoyés
    
    double bitrate = 0.0;         ///< Débit moyen en bits par seconde
    double instantBitrate = 0.0;  ///< Débit instantané en bits par seconde
    
    std::chrono::system_clock::time_point lastSendTime; ///< Horodatage du dernier envoi
    
    uint64_t errors = 0;          ///< Nombre d'erreurs d'envoi
    
    // Réinitialise les statistiques
    void reset() {
        packetsSent = 0;
        bytesSent = 0;
        bitrate = 0.0;
        instantBitrate = 0.0;
        lastSendTime = std::chrono::system_clock::now();
        errors = 0;
    }
};

/**
 * @brief Classe pour diffuser des flux MPEG-TS sur un groupe multicast
 */
class MulticastSender {
public:
    /**
     * @brief Constructeur
     * @param groupAddress Adresse IP du groupe multicast
     * @param port Port UDP pour la diffusion
     * @param interface Interface réseau à utiliser (vide = interface par défaut)
     * @param ttl TTL des paquets multicast
     */
    MulticastSender(const std::string& groupAddress, int port, 
                   const std::string& interface = "", int ttl = 4);
    
    /**
     * @brief Destructeur
     */
    ~MulticastSender();
    
    /**
     * @brief Initialise le sender multicast
     * @return true si l'initialisation a réussi, false sinon
     */
    bool initialize();
    
    /**
     * @brief Démarre le thread d'envoi
     * @return true si le démarrage a réussi, false sinon
     */
    bool start();
    
    /**
     * @brief Arrête le thread d'envoi
     */
    void stop();
    
    /**
     * @brief Vérifie si le sender est en cours d'exécution
     * @return true si le sender est en cours d'exécution, false sinon
     */
    bool isRunning() const;
    
    /**
     * @brief Envoie des données sur le groupe multicast
     * @param data Données à envoyer
     * @return true si l'envoi a réussi, false sinon
     */
    bool send(const std::vector<uint8_t>& data);
    
    /**
     * @brief Configure le débit du flux
     * @param bitrateKbps Débit en kilobits par seconde (0 = pas de limitation)
     */
    void setBitrate(uint32_t bitrateKbps);
    
    /**
     * @brief Récupère les statistiques du sender
     * @return Structure contenant les statistiques
     */
    const MulticastStats& getStats() const;
    
    /**
     * @brief Récupère l'adresse du groupe multicast
     * @return Adresse du groupe multicast
     */
    std::string getGroupAddress() const;
    
    /**
     * @brief Récupère le port multicast
     * @return Port multicast
     */
    int getPort() const;
    
private:
    std::string groupAddress_;
    int port_;
    std::string interface_;
    int ttl_;
    
    std::atomic<bool> running_;
    std::atomic<uint32_t> bitrateKbps_;
    
    int socket_;
    std::thread senderThread_;
    
    std::mutex queueMutex_;
    std::condition_variable queueCond_;
    std::queue<std::vector<uint8_t>> dataQueue_;
    
    MulticastStats stats_;
    
    void senderLoop();
    bool createSocket();
    void closeSocket();
};

} // namespace hls_to_dvb
