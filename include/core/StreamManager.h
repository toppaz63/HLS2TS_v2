#pragma once

#include "config.h"
#include "../hls/HLSClient.h"
#include "../mpegts/MPEGTSConverter.h"
#include "../multicast/MulticastSender.h"
#include "../core/SegmentBuffer.h"

// Ajouter ceci si MulticastSender est dans l'espace de noms hls_to_dvb
using hls_to_dvb::MulticastSender;

#include <unordered_map>
#include <memory>
#include <thread>
#include <mutex>
#include <optional>
#include <string>

namespace hls_to_dvb {

/**
 * @struct StreamInstance
 * @brief Représente une instance de flux en cours d'exécution
 */
struct StreamInstance {
    std::string id;                                  ///< ID du flux
    StreamConfig config;                             ///< Configuration du flux
    std::shared_ptr<HLSClient> hlsClient;            ///< Client HLS
    std::shared_ptr<MPEGTSConverter> mpegtsConverter; ///< Convertisseur MPEG-TS
    std::shared_ptr<SegmentBuffer> segmentBuffer;    ///< Buffer de segments
    std::shared_ptr<MulticastSender> multicastSender; ///< Émetteur multicast
    std::shared_ptr<std::atomic<bool>> running;      ///< État du flux (en cours d'exécution ou non)
    std::thread processingThread;                    ///< Thread de traitement du flux

    // Constructeur par défaut
    StreamInstance() : running(std::make_shared<std::atomic<bool>>(false)) {}

    // Méthode pour accéder à running de manière thread-safe
    bool isRunning() const {
        return running && running->load();
    }

    // Méthode pour modifier running de manière thread-safe
    void setRunning(bool value) {
        if (running) {
            running->store(value);
        }
    }
};

/**
 * @class StreamManager
 * @brief Gère l'ensemble des flux de l'application
 * 
 * Responsable de la création, du démarrage, de l'arrêt et de la surveillance
 * de tous les flux configurés dans l'application.
 */
class StreamManager {
public:
    /**
     * @brief Constructeur
     * @param config Pointeur vers la configuration
     */
    explicit StreamManager(Config* config);
    
    /**
     * @brief Démarre le gestionnaire de flux
     */
    void start();
    
    /**
     * @brief Arrête le gestionnaire de flux et tous les flux actifs
     */
    void stop();
    
    /**
     * @brief Démarre un flux spécifique
     * @param streamId ID du flux à démarrer
     * @return true si le flux a été démarré avec succès
     */
    bool startStream(const std::string& streamId);
    
    /**
     * @brief Arrête un flux spécifique
     * @param streamId ID du flux à arrêter
     * @return true si le flux a été arrêté avec succès
     */
    bool stopStream(const std::string& streamId);
    
    /**
     * @brief Vérifie si un flux est en cours d'exécution
     * @param streamId ID du flux à vérifier
     * @return true si le flux est en cours d'exécution
     */
    bool isStreamRunning(const std::string& streamId) const;
    
    /**
     * @brief Récupère les statistiques d'un flux
     * @param streamId ID du flux
     * @return Structure contenant les statistiques du flux
     */
    struct StreamStats {
        size_t segmentsProcessed = 0;       ///< Nombre de segments traités
        size_t discontinuitiesDetected = 0; ///< Nombre de discontinuités détectées
        size_t bufferSize = 0;              ///< Taille actuelle du buffer
        size_t bufferCapacity = 0;          ///< Capacité maximale du buffer
        size_t packetsTransmitted = 0;      ///< Nombre de paquets transmis
        double currentBitrate = 0.0;        ///< Débit actuel (bits/s)
        int width = 0;                      ///< Largeur de la vidéo
        int height = 0;                     ///< Hauteur de la vidéo
        int bandwidth = 0;                  ///< Bande passante en bits/s
        std::string codecs;                 ///< Codecs utilisés
    };
    
    /**
     * @brief Récupère les statistiques d'un flux
     * @param streamId ID du flux
     * @return Statistiques du flux ou nullopt si le flux n'existe pas
     */
    std::optional<StreamStats> getStreamStats(const std::string& streamId) const;
    
    /**
     * @brief Ajuste la taille du buffer d'un flux
     * @param streamId ID du flux
     * @param bufferSize Nouvelle taille du buffer
     * @return true si la taille a été ajustée avec succès
     */
    bool setStreamBufferSize(const std::string& streamId, size_t bufferSize);
    
    /**
     * @brief Destructeur
     */
    ~StreamManager();
    
private:
    Config* config_; ///< Pointeur vers la configuration
    
    std::unordered_map<std::string, StreamInstance> streams_; ///< Flux en cours d'exécution
    mutable std::mutex streamsMutex_; ///< Mutex pour l'accès concurrent aux flux
    
    std::atomic<bool> running_; ///< État du gestionnaire
    
    /**
     * @brief Fonction exécutée dans le thread de traitement d'un flux
     * @param streamId ID du flux à traiter
     */
    void processStream(const std::string& streamId);
};

} // namespace hls_to_dvb
