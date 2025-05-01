#include "core/StreamManager.h"
#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace hls_to_dvb {


StreamManager::StreamManager(Config* config)
    : config_(config), running_(false) {
}

void StreamManager::start() {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    
    if (running_) {
        spdlog::warn("Le gestionnaire de flux est déjà en cours d'exécution");
        return;
    }
    
    spdlog::info("Démarrage du gestionnaire de flux");
    running_ = true;
    
    // Démarrer tous les flux configurés
    auto streams = config_->getStreams();
    for (const auto& streamConfig : streams) {
        // Vérifier si le flux a une configuration valide
        if (!streamConfig.hlsInput.empty() && !streamConfig.multicastOutput.empty() && streamConfig.multicastPort > 0) {
            try {
                startStream(streamConfig.id);
            }
            catch (const std::exception& e) {
                spdlog::error("Erreur lors du démarrage du flux {}: {}", streamConfig.id, e.what());
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::ERROR,
                    "StreamManager",
                    "Erreur lors du démarrage du flux " + streamConfig.id + ": " + e.what(),
                    true
                );
            }
        }
    }
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
            if (pair.second.running) {
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
    // Vérifier si la configuration existe
    const StreamConfig* config = config_->getStream(streamId);
    if (!config) {
        spdlog::error("Configuration du flux non trouvée: {}", streamId);
        return false;
    }
    
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        
        // Vérifier si le flux est déjà en cours d'exécution
        auto it = streams_.find(streamId);
        if (it != streams_.end() && it->second.running) {
            spdlog::warn("Le flux {} est déjà en cours d'exécution", streamId);
            return true;
        }
        
        spdlog::info("Démarrage du flux: {}", streamId);
        
        try {
            // Créer une nouvelle instance de flux
            StreamInstance stream;
            stream.id = streamId;
            stream.config = *config;
            stream.running = false;
            
            // Créer les composants du flux
            stream.segmentBuffer = std::make_shared<SegmentBuffer>(config->bufferSize);
            stream.hlsClient = std::make_shared<HLSClient>(config->hlsInput);
            stream.mpegtsConverter = std::make_shared<MPEGTSConverter>();
            stream.multicastSender = std::make_shared<MulticastSender>(
                config->multicastOutput, 
                config->multicastPort
            );
            
            // Démarrer le client HLS pour vérifier si le flux est valide
            stream.hlsClient->start();
            
            // Vérifier si le flux est valide (contient des segments MPEG-TS)
            if (!stream.hlsClient->isValidStream()) {
                stream.hlsClient->stop();
                
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
            
            // Enregistrer l'instance
            // Puis insérer la nouvelle entrée avec emplace
            streams_.emplace(std::piecewise_construct, std::forward_as_tuple(streamId), std::forward_as_tuple(std::move(stream)));
            
            // Démarrer le thread de traitement
            stream.running = true;
            stream.processingThread = std::thread(&StreamManager::processStream, this, streamId);
            
            AlertManager::getInstance().addAlert(
                AlertLevel::INFO,
                "StreamManager",
                "Flux " + streamId + " (" + config->name + ") démarré",
                false
            );
            
            // Ajouter des informations sur le flux sélectionné
            HLSStreamInfo streamInfo = stream.hlsClient->getStreamInfo();
            
            AlertManager::getInstance().addAlert(
                AlertLevel::INFO,
                "StreamManager",
                "Flux " + streamId + " : " + std::to_string(streamInfo.width) + "x" + 
                std::to_string(streamInfo.height) + ", " + std::to_string(streamInfo.bandwidth / 1000) + 
                "kbps, codecs: " + streamInfo.codecs,
                false
            );
            
            return true;
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
}

bool StreamManager::stopStream(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    
    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        spdlog::error("Flux non trouvé: {}", streamId);
        return false;
    }
    
    StreamInstance& stream = it->second;
    
    if (!stream.running) {
        spdlog::warn("Le flux {} n'est pas en cours d'exécution", streamId);
        return true;
    }
    
    spdlog::info("Arrêt du flux: {}", streamId);
    
    // Arrêter le traitement du flux
    stream.running = false;
    
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
    
    return it->second.running;
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
    config_->setStream(updatedConfig);
    
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
        
        if (!stream->multicastSender->isRunning()) {
            stream->multicastSender->start();
        }
        
        // Boucle principale de traitement
        while (stream->running) {
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
                            // Envoyer le segment en multicast
                            stream->multicastSender->send(segmentToSend.data);
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

StreamManager::~StreamManager() {
    if (running_) {
        stop();
    }
}

} // namespace hls_to_dvb