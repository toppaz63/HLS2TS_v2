#include "core/StreamManager.h"
#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <set> 

// Ajouter ces inclusions pour les fonctions réseau
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

namespace hls_to_dvb {


StreamManager::StreamManager(Config* config)
    : config_(config), running_(false) {
}

void StreamManager::start() {
    static int startCallCount = 0;
    int currentStartCall = ++startCallCount;
    spdlog::info("start() appelé (appel #{}) - début", currentStartCall);
    
    // Récupérer les configurations des flux
    auto streamConfigs = config_->getStreamConfigs();
    
    // Vérifier s'il y a des doublons et les journaliser
    std::set<std::string> uniqueIds;
    for (const auto& config : streamConfigs) {
        if (!uniqueIds.insert(config.id).second) {
            spdlog::warn("ID de flux en double détecté : {}", config.id);
        }
    }
    
    // Définir l'état running_
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        
        if (running_) {
            spdlog::warn("Le gestionnaire de flux est déjà en cours d'exécution");
            return;
        }
        
        running_ = true;
    }
    
    spdlog::info("Démarrage du gestionnaire de flux");
    
    // Utiliser un ensemble pour suivre les flux déjà démarrés
    std::set<std::string> startedStreams;
    
    // Démarrer les flux sans tenir le mutex principal
    for (const auto& streamConfig : streamConfigs) {
        // Vérifier si ce flux a déjà été démarré
        if (startedStreams.find(streamConfig.id) != startedStreams.end()) {
            spdlog::warn("Tentative de démarrage multiple du flux {}, ignorée", streamConfig.id);
            continue;
        }
        
        if (!streamConfig.hlsInput.empty() && !streamConfig.mcastOutput.empty() && streamConfig.mcastPort > 0) {
            try {
                spdlog::info("Tentative de démarrage du flux {}", streamConfig.id);
                bool success = startStream(streamConfig.id);
                
                if (success) {
                    startedStreams.insert(streamConfig.id);
                    spdlog::info("Flux {} démarré avec succès", streamConfig.id);
                } else {
                    spdlog::warn("Échec du démarrage du flux {}", streamConfig.id);
                }
            }
            catch (const std::exception& e) {
                spdlog::error("Exception lors du démarrage du flux {}: {}", streamConfig.id, e.what());
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::ERROR,
                    "StreamManager",
                    "Erreur lors du démarrage du flux " + streamConfig.id + ": " + e.what(),
                    true
                );
            }
        }
    }
    spdlog::info("start() appelé (appel #{}) - fin", currentStartCall);
    spdlog::info("Tous les flux ont été traités, {} flux démarrés", startedStreams.size());
}


void StreamManager::stop() {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    
    if (!running_) {
        spdlog::warn("Le gestionnaire de flux n'est pas en cours d'exécution");
        return;
    }
    
    spdlog::info("Arrêt du gestionnaire de flux");
    
    // Arrêter tous les flux en cours d'exécution
    for (auto& pair : streams_) {
        try {
            if (pair.second.isRunning()) {
                stopStream(pair.first);
            }
        }
        catch (const std::exception& e) {
            spdlog::error("Erreur lors de l'arrêt du flux {}: {}", pair.first, e.what());
        }
    }
    
    running_ = false;
    
    spdlog::info("Tous les flux ont été arrêtés");
}

bool StreamManager::startStream(const std::string& streamId) {
    static std::map<std::string, int> callCounts;
    int currentCall = ++callCounts[streamId];
    spdlog::info("startStream({}) appelé - appel #{} pour ce flux", streamId, currentCall);

    // Vérifier si la configuration existe
    const StreamConfig* config = config_->getStreamConfig(streamId);
    if (!config) {
        spdlog::error("Configuration du flux non trouvée: {}", streamId);
        return false;
    }

    // Valider l'adresse multicast avant de faire quoi que ce soit d'autre
    if (!isValidMulticastAddress(config->mcastOutput)) {
        spdlog::error("Adresse multicast invalide: {}", config->mcastOutput);
        return false;
    }
    
    // Créer tous les composants en dehors du mutex
    spdlog::info("Démarrage du flux: {}", streamId);
    
    // Créer une nouvelle instance de flux temporaire
    StreamInstance tempStream;
    spdlog::info("Création d'une nouvelle instance de flux pour {}", streamId);
    tempStream.id = streamId;
    tempStream.config = *config;
    tempStream.setRunning(false);
    
    try {
        // Créer les composants du flux en dehors du mutex
        spdlog::info("Création du SegmentBuffer pour {}", streamId);
        tempStream.segmentBuffer = std::make_shared<SegmentBuffer>(config->bufferSize);
        spdlog::info("SegmentBuffer créé pour {}", streamId);

        spdlog::info("Création du HLSClient pour {} avec URL: {}", streamId, config->hlsInput);
        tempStream.hlsClient = std::make_shared<HLSClient>(config->hlsInput);
        spdlog::info("HLSClient créé pour {}", streamId);

        spdlog::info("Création du MPEGTSConverter pour {}", streamId);
        tempStream.mpegtsConverter = std::make_shared<MPEGTSConverter>();
        spdlog::info("MPEGTSConverter créé pour {}", streamId);
        
        // Création du MulticastSender avec journalisation des paramètres
        spdlog::info("Création du MulticastSender pour le flux {} avec adresse {} et port {}", 
                     streamId, config->mcastOutput, config->mcastPort);
        
        tempStream.multicastSender = std::make_shared<MulticastSender>(
            config->mcastOutput, 
            config->mcastPort,
            config->mcastInterface,
            4
        );
        
        // Initialiser le MulticastSender
        if (!tempStream.multicastSender->initialize()) {
            spdlog::error("Échec de l'initialisation du sender multicast pour le flux {}", streamId);
            
            AlertManager::getInstance().addAlert(
                AlertLevel::ERROR,
                "StreamManager",
                "Échec de l'initialisation du sender multicast pour le flux " + streamId + 
                ". Vérifiez l'adresse multicast " + config->mcastOutput,
                true
            );
            
            return false;
        }
        
        // Démarrer le client HLS pour vérifier si le flux est valide
        tempStream.hlsClient->start();
        
        // Vérifier si le flux est valide (contient des segments MPEG-TS)
        if (!tempStream.hlsClient->isValidStream()) {
            tempStream.hlsClient->stop();
            
            spdlog::error("Le flux HLS {} ne contient pas de segments MPEG-TS", streamId);
            
            AlertManager::getInstance().addAlert(
                AlertLevel::ERROR,
                "StreamManager",
                "Le flux HLS " + streamId + " (" + config->name + ") ne contient pas de segments MPEG-TS. "
                "Cette application ne peut traiter que les flux HLS avec des segments MPEG-TS.",
                true
            );
            
            return false;
        }
        
        // Maintenant que tous les composants sont prêts, verrouiller le mutex
        // pour mettre à jour la liste des flux
        bool success = false;
        HLSStreamInfo streamInfo;
        
        {
            // Utiliser lock_guard pour garantir le déverrouillage en cas d'exception
            std::lock_guard<std::mutex> lock(streamsMutex_);
            spdlog::info("Mutex verrouillé pour le flux {}", streamId);
            
            // Vérifier si le flux est déjà en cours d'exécution
            auto it = streams_.find(streamId);
            if (it != streams_.end() && it->second.isRunning()) {
                spdlog::warn("Le flux {} est déjà en cours d'exécution", streamId);
                return true;
            }
            
            // Créer une nouvelle entrée pour ce flux
            tempStream.setRunning(true);
            auto result = streams_.emplace(streamId, std::move(tempStream));
            
            // Démarrer le thread de traitement
            auto& savedStream = result.first->second;
            savedStream.processingThread = std::thread(&StreamManager::processStream, this, streamId);
            
            // Récupérer les informations sur le flux sélectionné
            streamInfo = savedStream.hlsClient->getStreamInfo();
            success = true;
            
            spdlog::info("Mutex déverrouillé pour le flux {}", streamId);
        }
        
        // Générer les alertes après avoir déverrouillé le mutex
        if (success) {
            AlertManager::getInstance().addAlert(
                AlertLevel::INFO,
                "StreamManager",
                "Flux " + streamId + " (" + config->name + ") démarré",
                false
            );
            
            AlertManager::getInstance().addAlert(
                AlertLevel::INFO,
                "StreamManager",
                "Flux " + streamId + " : " + std::to_string(streamInfo.width) + "x" + 
                std::to_string(streamInfo.height) + ", " + std::to_string(streamInfo.bandwidth / 1000) + 
                "kbps, codecs: " + streamInfo.codecs,
                false
            );
        }
        spdlog::info("startStream({}) terminé - appel #{} pour ce flux", streamId, currentCall);
        return success;
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de la création du flux {}: {}", streamId, e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "StreamManager",
            "Erreur lors de la création du flux " + streamId + ": " + e.what(),
            true
        );
        
        return false;
    }
}



bool StreamManager::stopStream(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    
    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        spdlog::error("Flux non trouvé: {}", streamId);
        return false;
    }
    
    StreamInstance& stream = it->second;
    
    if (!stream.isRunning()) {
        spdlog::warn("Le flux {} n'est pas en cours d'exécution", streamId);
        return true;
    }
    
    spdlog::info("Arrêt du flux: {}", streamId);
    
    // Arrêter le traitement du flux
    stream.setRunning(false);
    
    // Attendre la fin du thread de traitement
    if (stream.processingThread.joinable()) {
        stream.processingThread.join();
    }
    
    // Arrêter les composants
    if (stream.multicastSender) {
        stream.multicastSender->stop();
    }
    
    if (stream.mpegtsConverter) {
        stream.mpegtsConverter->stop();
    }
    
    if (stream.hlsClient) {
        stream.hlsClient->stop();
    }
    
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "StreamManager",
        "Flux " + streamId + " (" + stream.config.name + ") arrêté",
        false
    );
    
    return true;
}

bool StreamManager::isStreamRunning(const std::string& streamId) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    
    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        return false;
    }
    
    return it->second.isRunning();
}

std::optional<StreamManager::StreamStats> StreamManager::getStreamStats(const std::string& streamId) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    
    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        return std::nullopt;
    }
    
    const StreamInstance& stream = it->second;
    
    StreamStats stats;
    
    if (stream.hlsClient) {
        stats.segmentsProcessed = stream.hlsClient->getSegmentsProcessed();
        stats.discontinuitiesDetected = stream.hlsClient->getDiscontinuitiesDetected();
        
        // Ajouter les informations sur le flux
        HLSStreamInfo streamInfo = stream.hlsClient->getStreamInfo();
        stats.width = streamInfo.width;
        stats.height = streamInfo.height;
        stats.bandwidth = streamInfo.bandwidth;
        stats.codecs = streamInfo.codecs;
    }
    
    if (stream.segmentBuffer) {
        stats.bufferSize = stream.segmentBuffer->getCurrentSize();
        stats.bufferCapacity = stream.segmentBuffer->getBufferSize();
    }
    
    if (stream.multicastSender) {
        const MulticastStats& multicastStats = stream.multicastSender->getStats();
        stats.packetsTransmitted = multicastStats.packetsSent;
        stats.currentBitrate = multicastStats.instantBitrate;
    }
    
    return stats;
}

bool StreamManager::setStreamBufferSize(const std::string& streamId, size_t bufferSize) {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    
    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        spdlog::error("Flux non trouvé pour l'ajustement du buffer: {}", streamId);
        return false;
    }
    
    StreamInstance& stream = it->second;
    
    if (!stream.segmentBuffer) {
        spdlog::error("Buffer non initialisé pour le flux: {}", streamId);
        return false;
    }
    
    // Mettre à jour la taille du buffer
    stream.segmentBuffer->setBufferSize(bufferSize);
    
    // Mettre à jour la configuration pour la persistance
    StreamConfig updatedConfig = stream.config;
    updatedConfig.bufferSize = bufferSize;
    config_->updateStreamConfig(updatedConfig);
    
    spdlog::info("Taille du buffer ajustée pour le flux {}: {}", streamId, bufferSize);
    
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "StreamManager",
        "Taille du buffer ajustée pour le flux " + streamId + ": " + std::to_string(bufferSize),
        false
    );
    
    return true;
}

void StreamManager::processStream(const std::string& streamId) {
    StreamInstance* stream = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        auto it = streams_.find(streamId);
        if (it == streams_.end()) {
            spdlog::error("Flux non trouvé pour le traitement: {}", streamId);
            return;
        }
        stream = &it->second;
    }
    
    if (!stream || !stream->hlsClient || !stream->mpegtsConverter || 
        !stream->multicastSender || !stream->segmentBuffer) {
        spdlog::error("Composants non initialisés pour le flux: {}", streamId);
        return;
    }
    
    spdlog::info("Démarrage du thread de traitement pour le flux: {}", streamId);
    
    try {
        // Démarrer les composants si ce n'est pas déjà fait
        if (!stream->mpegtsConverter->isRunning()) {
            stream->mpegtsConverter->start();
        }
        
        // *** MODIFICATION IMPORTANTE : Vérifier le résultat de start() ***
        if (!stream->multicastSender->isRunning()) {
            spdlog::info("Démarrage du MulticastSender pour le flux {}", streamId);
            if (!stream->multicastSender->start()) {
                spdlog::error("Échec du démarrage du sender multicast pour le flux {}", streamId);
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::ERROR,
                    "StreamManager",
                    "Échec du démarrage du sender multicast pour le flux " + streamId + 
                    ". Vérifiez que l'initialisation a été effectuée correctement.",
                    true
                );
                
                // Marquer le flux comme arrêté pour sortir de la boucle
                stream->setRunning(false);
                return;
            }
        }
        
        // Boucle principale de traitement
        while (stream->isRunning()) {
            try {
                // Récupérer un segment HLS
                auto hlsSegment = stream->hlsClient->getNextSegment();
                
                if (hlsSegment) {
                    // Convertir le segment en MPEG-TS
                    auto mpegtsSegment = stream->mpegtsConverter->convert(*hlsSegment);
                    
                    if (mpegtsSegment) {
                        // Ajouter le segment au buffer
                        stream->segmentBuffer->pushSegment(*mpegtsSegment);
                        
                        // Essayer de récupérer un segment du buffer pour l'envoi
                        MPEGTSSegment segmentToSend;
                        if (stream->segmentBuffer->getSegment(segmentToSend)) {
                            // Vérifier que le sender est toujours opérationnel
                            if (!stream->multicastSender->isRunning()) {
                                spdlog::warn("MulticastSender non opérationnel, tentative de redémarrage");
                                if (!stream->multicastSender->start()) {
                                    spdlog::error("Impossible de redémarrer le MulticastSender");
                                    throw std::runtime_error("Échec de redémarrage du MulticastSender");
                                }
                            }
                            
                            // Envoyer le segment en multicast
                            if (!stream->multicastSender->send(segmentToSend.data)) {
                                spdlog::error("Échec d'envoi du segment multicast");
                            }
                        }
                    }
                }
                else {
                    // Aucun segment HLS disponible, attendre un peu
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            catch (const std::exception& e) {
                spdlog::error("Erreur lors du traitement du flux {}: {}", streamId, e.what());
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::ERROR,
                    "StreamManager",
                    "Erreur lors du traitement du flux " + streamId + ": " + e.what(),
                    true
                );
                
                // Attendre un peu avant de réessayer
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        
        spdlog::info("Arrêt du thread de traitement pour le flux: {}", streamId);
    }
    catch (const std::exception& e) {
        spdlog::error("Exception dans le thread de traitement du flux {}: {}", streamId, e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "StreamManager",
            "Exception dans le thread de traitement du flux " + streamId + ": " + e.what(),
            true
        );
    }
}

bool StreamManager::isValidMulticastAddress(const std::string& address) {
    struct in_addr addr;
    spdlog::info("Validation de l'adresse multicast: {}", address);
    if (inet_pton(AF_INET, address.c_str(), &addr) != 1) {
        spdlog::error("Format d'adresse IP invalide: {}", address);
        return false;
    }
    
    // Vérifier que c'est bien une adresse multicast (224.0.0.0 à 239.255.255.255)
    uint32_t addrValue = ntohl(addr.s_addr);
    if ((addrValue & 0xF0000000) != 0xE0000000) {
        spdlog::error("L'adresse {} n'est pas une adresse multicast valide", address);
        return false;
    }
    
    spdlog::info("Adresse multicast valide: {}", address);
    return true;
}

StreamManager::~StreamManager() {
    if (running_) {
        stop();
    }
}

} // namespace hls_to_dvb
