#include "core/StreamManager.h"
#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <set> 

// Ajouter ces inclusions pour les fonctions réseau
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <process.h>  // Pour _getpid()
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>  // Pour getpid()
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
    try {
        static std::map<std::string, int> callCounts;
        int currentCall = ++callCounts[streamId];
        spdlog::info("********************************  startStream({}) appelé - appel #{} pour ce flux", streamId, currentCall);

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
        tempStream.setRunning(false);  // Initialement à false jusqu'à ce que tout soit prêt
        
        try {
            // Création et initialisation des composants
            spdlog::info("Création du SegmentBuffer pour {}", streamId);
            tempStream.segmentBuffer = std::make_shared<SegmentBuffer>(config->bufferSize);
            
            spdlog::info("Création du HLSClient pour {} avec URL: {}", streamId, config->hlsInput);
            tempStream.hlsClient = std::make_shared<HLSClient>(config->hlsInput);
            
            spdlog::info("Création du MPEGTSConverter pour {}", streamId);
            tempStream.mpegtsConverter = std::make_shared<MPEGTSConverter>();
            
            spdlog::info("Création du MulticastSender pour le flux {} avec adresse {} et port {}", 
                        streamId, config->mcastOutput, config->mcastPort);
            
            tempStream.multicastSender = std::make_shared<MulticastSender>(
                config->mcastOutput, 
                config->mcastPort,
                config->mcastInterface,
                4
            );
            
            // NOUVEAU: Création du moniteur de qualité
            spdlog::info("Création du TSQualityMonitor pour {}", streamId);
            tempStream.qualityMonitor = std::make_shared<TSQualityMonitor>();
            
            // Vérifier que tous les composants ont été créés correctement
            if (!tempStream.hlsClient || !tempStream.mpegtsConverter || 
                !tempStream.multicastSender || !tempStream.segmentBuffer || 
                !tempStream.qualityMonitor) {
                
                spdlog::error("Un ou plusieurs composants n'ont pas pu être créés pour le flux {}", streamId);
                return false;
            }
            
            // DÉMARRAGE: Démarrer tous les composants AVANT de créer le thread
            
            // 1. Initialiser le MulticastSender
            spdlog::info("Initialisation du MulticastSender pour {}", streamId);
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
            
            // 2. Démarrer le HLSClient et vérifier s'il fonctionne correctement
            spdlog::info("Démarrage du HLSClient pour {}", streamId);
            tempStream.hlsClient->start();
            
            // Vérifier si c'est un flux live et valide
            if (!tempStream.hlsClient->isLiveStream()) {
                tempStream.hlsClient->stop();
                
                spdlog::error("Le flux HLS {} n'est pas un flux Live", streamId);
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::ERROR,
                    "StreamManager",
                    "Le flux HLS " + streamId + " (" + config->name + ") n'est pas un flux Live. "
                    "Cette application ne peut traiter que les flux HLS Live.",
                    true
                );
                
                return false;
            }

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
            
            // 3. Démarrer le MPEGTSConverter
            spdlog::info("Démarrage du MPEGTSConverter pour {}", streamId);
            tempStream.mpegtsConverter->start();
            
            if (!tempStream.mpegtsConverter->isRunning()) {
                spdlog::error("Le MPEGTSConverter n'a pas démarré correctement pour le flux {}", streamId);
                tempStream.hlsClient->stop();
                return false;
            }
            
            // 4. Démarrer le MulticastSender
            spdlog::info("Démarrage du MulticastSender pour le flux {}", streamId);
            if (!tempStream.multicastSender->start()) {
                spdlog::error("Échec du démarrage du sender multicast pour le flux {}", streamId);
                tempStream.hlsClient->stop();
                tempStream.mpegtsConverter->stop();
                return false;
            }
            
            // Test de validation direct: envoyer un paquet de test pour vérifier la chaîne complète
            spdlog::info("Test direct d'envoi multicast pour le flux {}", streamId);
            bool testSent = false;
            
            // Récupérer un segment de test pour vérifier la chaîne complète
            auto testSegment = tempStream.hlsClient->getNextSegment();
            if (testSegment) {
                auto convertedSegment = tempStream.mpegtsConverter->convert(*testSegment);
                if (convertedSegment) {
                    // Analyser la qualité du segment
                    tempStream.qualityMonitor->analyze(convertedSegment->data);
                    
                    // Essayer d'envoyer le segment
                    testSent = tempStream.multicastSender->send(convertedSegment->data, false);
                    spdlog::info("Test d'envoi direct: {}", testSent ? "Réussi" : "Échoué");
                }
            }
            
            if (!testSent) {
                // Fallback: envoyer un paquet de test synthétique
                std::vector<uint8_t> testData(188, 0xFF);
                std::memcpy(testData.data(), "TEST_DIRECT_MULTICAST", 21);
                testSent = tempStream.multicastSender->send(testData, false);
                spdlog::info("Test d'envoi de secours: {}", testSent ? "Réussi" : "Échoué");
                
                if (!testSent) {
                    spdlog::error("Tous les tests d'envoi ont échoué, arrêt du démarrage du flux");
                    tempStream.hlsClient->stop();
                    tempStream.mpegtsConverter->stop();
                    tempStream.multicastSender->stop();
                    return false;
                }
            }
            
            // Maintenant que tous les composants fonctionnent,
            // verrouiller le mutex pour ajouter le flux à la liste des streams_
            {
                std::lock_guard<std::mutex> lock(streamsMutex_);
                spdlog::info("Mutex verrouillé pour le flux {}", streamId);
                
                // Vérifier si le flux est déjà en cours d'exécution
                auto it = streams_.find(streamId);
                if (it != streams_.end() && it->second.isRunning()) {
                    spdlog::warn("Le flux {} est déjà en cours d'exécution", streamId);
                    return true;
                }
                
                // Marquer la stream comme en cours d'exécution AVANT de lancer le thread
                tempStream.setRunning(true);
                
                // Ajouter le flux à la map
                auto result = streams_.emplace(streamId, std::move(tempStream));
                
                // Démarrer le thread de traitement
                auto& savedStream = result.first->second;
                try {
                    savedStream.processingThread = std::thread(&StreamManager::processStream, this, streamId);
                    spdlog::info("*** THREAD CRÉÉ AVEC SUCCÈS ***");
                } catch (const std::exception& e) {
                    spdlog::error("Erreur lors de la création du thread de traitement pour le flux {}: {}", streamId, e.what());
                    // Mettre à jour l'état
                    savedStream.setRunning(false);
                    savedStream.hlsClient->stop();
                    savedStream.mpegtsConverter->stop();
                    savedStream.multicastSender->stop();
                    return false;
                }
                
                spdlog::info("Mutex déverrouillé pour le flux {}", streamId);
            }
            
            // Récupérer les informations sur le flux et générer les alertes
            HLSStreamInfo streamInfo = tempStream.hlsClient->getStreamInfo();
            
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
            
            spdlog::info("startStream({}) terminé avec succès - appel #{} pour ce flux", streamId, currentCall);
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
    } catch (const std::exception& e) {
        spdlog::error("*** EXCEPTION NON GÉRÉE DANS startStream({}): {} ***", streamId, e.what());
        return false;
    } catch (...) {
        spdlog::error("*** EXCEPTION INCONNUE DANS startStream({}) ***", streamId);
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
    // Afficher l'ID du thread pour le débogage
    std::thread::id threadId = std::this_thread::get_id();
    std::stringstream ss;
    ss << threadId;
    std::string threadIdStr = ss.str();
    spdlog::info("*** [THREAD: {}] Démarrage du thread de traitement pour {} ***", threadIdStr, streamId);
    
    // Variables pour le monitoring de la santé du flux
    auto lastSuccessfulCycleTime = std::chrono::steady_clock::now();
    bool healthCheckPassed = true;
    int consecutiveErrorCount = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;
    
    // Récupérer l'instance du flux
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
    
    // Vérifier que tous les composants sont présents
    if (!stream || !stream->hlsClient || !stream->mpegtsConverter || 
        !stream->multicastSender || !stream->segmentBuffer || !stream->qualityMonitor) {
        
        spdlog::error("Composants non initialisés pour le flux: {}", streamId);
        spdlog::error("hlsClient: {}, mpegtsConverter: {}, multicastSender: {}, segmentBuffer: {}, qualityMonitor: {}",
                   stream ? (stream->hlsClient ? "OK" : "NULL") : "STREAM NULL",
                   stream ? (stream->mpegtsConverter ? "OK" : "NULL") : "STREAM NULL",
                   stream ? (stream->multicastSender ? "OK" : "NULL") : "STREAM NULL",
                   stream ? (stream->segmentBuffer ? "OK" : "NULL") : "STREAM NULL",
                   stream ? (stream->qualityMonitor ? "OK" : "NULL") : "STREAM NULL");
        return;
    }
    
    // Vérifier que tous les composants sont démarrés (ils devraient l'être selon la logique de startStream)
    if (!stream->hlsClient->isRunning() || !stream->mpegtsConverter->isRunning() || 
        !stream->multicastSender->isRunning()) {
        spdlog::error("Un ou plusieurs composants ne sont pas démarrés pour le flux: {}", streamId);
        spdlog::error("hlsClient: {}, mpegtsConverter: {}, multicastSender: {}",
                   stream->hlsClient->isRunning() ? "Running" : "Stopped",
                   stream->mpegtsConverter->isRunning() ? "Running" : "Stopped",
                   stream->multicastSender->isRunning() ? "Running" : "Stopped");
        
        // On ne retourne pas ici, mais on log l'erreur et on tente de continuer
        // Les composants auraient dû être démarrés dans startStream
    }
    
    #ifdef _WIN32
        spdlog::info("Thread de traitement du flux {} démarré avec PID={}, thread ID: {:x}", 
                   streamId, _getpid(), std::hash<std::thread::id>{}(std::this_thread::get_id()));
    #else
        spdlog::info("Thread de traitement du flux {} démarré avec PID={}, thread ID: {:x}", 
                   streamId, getpid(), std::hash<std::thread::id>{}(std::this_thread::get_id()));
    #endif

    try {
        // Variables pour le contrôle temporel
        auto lastSegmentTime = std::chrono::steady_clock::now();
        auto lastPlaylistRefreshTime = std::chrono::steady_clock::now();
        auto lastCheckTime = std::chrono::steady_clock::now();
        auto lastQualityCheckTime = std::chrono::steady_clock::now();
        auto sendStartTime = std::chrono::steady_clock::now();
        auto lastHealthCheckTime = std::chrono::steady_clock::now();
        
        double lastSegmentDuration = 0.0;
        int lastProcessedSequenceNumber = -1;
        int emptyCount = 0;
        int retryCount = 0;
        bool segmentInProgress = false;
        bool waitingForNewSegment = false;
        
        // Paramètres adaptés aux flux HLS live
        const int MAX_RETRIES_BEFORE_ACTION = 10;
        const int MAX_RETRIES_BEFORE_RESTART = 30;
        const int PLAYLIST_REFRESH_INTERVAL_SEC = 10;
        const int QUALITY_CHECK_INTERVAL_SEC = 30;
        const int FORCED_CHECK_INTERVAL_SEC = 5;
        const int HEALTH_CHECK_INTERVAL_SEC = 20;
        
        spdlog::info("Démarrage de la boucle principale pour le flux {}", streamId);
        
        // Boucle principale de traitement
        while (stream->isRunning()) {
            try {
                auto currentTime = std::chrono::steady_clock::now();
                
 // Vérification périodique de l'état de santé du flux
                auto elapsedSinceHealthCheck = std::chrono::duration_cast<std::chrono::seconds>(
                    currentTime - lastHealthCheckTime).count();
                
                if (elapsedSinceHealthCheck >= HEALTH_CHECK_INTERVAL_SEC) {
                    lastHealthCheckTime = currentTime;
                    
                    // Vérifier si tous les composants sont toujours opérationnels
                    bool allComponentsRunning = stream->hlsClient->isRunning() &&
                                              stream->mpegtsConverter->isRunning() &&
                                              stream->multicastSender->isRunning();
                    
                    if (!allComponentsRunning) {
                        spdlog::warn("Un ou plusieurs composants ne sont plus en cours d'exécution pour le flux {}", streamId);
                        spdlog::warn("État des composants - HLSClient: {}, MPEGTSConverter: {}, MulticastSender: {}",
                                  stream->hlsClient->isRunning() ? "OK" : "Arrêté",
                                  stream->mpegtsConverter->isRunning() ? "OK" : "Arrêté",
                                  stream->multicastSender->isRunning() ? "OK" : "Arrêté");
                        
                        // Tentative de redémarrage des composants arrêtés
                        if (!stream->hlsClient->isRunning()) {
                            spdlog::info("Tentative de redémarrage du HLSClient pour le flux {}", streamId);
                            stream->hlsClient->start();
                        }
                        
                        if (!stream->mpegtsConverter->isRunning()) {
                            spdlog::info("Tentative de redémarrage du MPEGTSConverter pour le flux {}", streamId);
                            stream->mpegtsConverter->start();
                        }
                        
                        if (!stream->multicastSender->isRunning()) {
                            spdlog::info("Tentative de redémarrage du MulticastSender pour le flux {}", streamId);
                            stream->multicastSender->start();
                        }
                    }
                    
                    // Vérifier le temps écoulé depuis le dernier traitement réussi
                    auto timeSinceSuccess = std::chrono::duration_cast<std::chrono::seconds>(
                        currentTime - lastSuccessfulCycleTime).count();
                    
                    if (timeSinceSuccess > 60 && healthCheckPassed) {
                        spdlog::warn("Aucun segment traité avec succès depuis {} secondes pour le flux {}", 
                                  timeSinceSuccess, streamId);
                        healthCheckPassed = false;
                        
                        // Déclencher un rafraîchissement immédiat
                        lastPlaylistRefreshTime = std::chrono::steady_clock::now() - std::chrono::seconds(PLAYLIST_REFRESH_INTERVAL_SEC + 1);
                    }
                }
                
                // Vérification périodique de la qualité du flux
                auto elapsedSinceQualityCheck = std::chrono::duration_cast<std::chrono::seconds>(
                    currentTime - lastQualityCheckTime).count();
                
                if (elapsedSinceQualityCheck >= QUALITY_CHECK_INTERVAL_SEC) {
                    lastQualityCheckTime = currentTime;
                    
                    // Récupérer et analyser les statistiques de qualité
                    const auto& stats = stream->qualityMonitor->getStats();
                    
                    spdlog::info("Statistiques de qualité pour le flux {}: PCR discontinuités={}, CC erreurs={}, PCR jitter={}ms, Débit={}kbps",
                               streamId, stats.pcrDiscontinuities, stats.continuityErrors, stats.pcrJitter,
                               stats.bitrateBps / 1000);
                    
                    // Vérifier la conformité DVB
                    bool isDVBCompliant = stream->qualityMonitor->isDVBCompliant(true);
                    
                    if (!isDVBCompliant) {
                        spdlog::warn("Le flux {} n'est pas entièrement conforme aux spécifications DVB", streamId);
                        
                        AlertManager::getInstance().addAlert(
                            AlertLevel::WARNING,
                            "StreamManager",
                            "Problèmes de conformité DVB détectés dans le flux " + streamId + 
                            ". Vérifiez les logs pour plus de détails.",
                            false
                        );
                    }
                }
                
                // Vérifier si le client HLS est toujours actif
                if (!stream->hlsClient->isRunning()) {
                    spdlog::error("HLSClient n'est plus en cours d'exécution, tentative de redémarrage");
                    // Tenter de redémarrer le client HLS
                    stream->hlsClient->start();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    
                    // Incrémenter le compteur d'erreurs consécutives
                    consecutiveErrorCount++;
                    
                    if (consecutiveErrorCount > MAX_CONSECUTIVE_ERRORS) {
                        spdlog::error("Trop d'erreurs consécutives ({}/{}), réinitialisation complète du flux",
                                   consecutiveErrorCount, MAX_CONSECUTIVE_ERRORS);
                        
                        // Réinitialiser complètement le flux
                        // (Cette logique sera implémentée dans la méthode resetStream)
                        resetStream(streamId);
                        consecutiveErrorCount = 0;
                    }
                    
                    continue;
                }
                
                // Si nous sommes en train de traiter un segment, attendre que sa durée soit écoulée
                if (segmentInProgress) {
                    auto elapsedSinceStartSec = std::chrono::duration_cast<std::chrono::milliseconds>(
                        currentTime - sendStartTime).count() / 1000.0;
                    
                    if (elapsedSinceStartSec < lastSegmentDuration) {
                        // Attendre un court moment pour être réactif tout en respectant le timing
                        double remainingSec = lastSegmentDuration - elapsedSinceStartSec;
                        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(remainingSec * 1000 * 0.2)));
                        continue;
                    }
                    
                    // Le segment est terminé
                    segmentInProgress = false;
                    spdlog::debug("Fin de la diffusion du segment {}", lastProcessedSequenceNumber);
                }
                
                // Calculer le temps écoulé depuis le dernier segment
                auto elapsedSinceLastSegmentSec = std::chrono::duration_cast<std::chrono::seconds>(
                    currentTime - lastSegmentTime).count();
                
                // Calculer le temps d'attente optimal
                double waitTimeSec = lastSegmentDuration > 0.0 ? lastSegmentDuration * 0.5 : 2.0;
                
                // Vérifier s'il est temps de rafraîchir la playlist
                auto elapsedSinceRefreshSec = std::chrono::duration_cast<std::chrono::seconds>(
                    currentTime - lastPlaylistRefreshTime).count();

                if (elapsedSinceRefreshSec >= PLAYLIST_REFRESH_INTERVAL_SEC && waitingForNewSegment) {
                    spdlog::info("Rafraîchissement de la playlist HLS après {}s sans nouveaux segments",
                            elapsedSinceRefreshSec);
                    
                    try {
                        bool refreshed = stream->hlsClient->refreshPlaylist();
                        lastPlaylistRefreshTime = currentTime;
                        
                        if (refreshed) {
                            spdlog::info("Playlist rafraîchie avec succès");
                            retryCount = 0;
                        }
                    } catch (const std::exception& e) {
                        spdlog::error("Erreur lors du rafraîchissement de la playlist: {}", e.what());
                        lastPlaylistRefreshTime = currentTime;
                        consecutiveErrorCount++;
                    }
                }
                
                // Vérifier s'il faut traiter une vérification forcée
                auto timeElapsedSinceLastCheck = std::chrono::duration_cast<std::chrono::seconds>(
                    currentTime - lastCheckTime).count();
                
                if (timeElapsedSinceLastCheck >= FORCED_CHECK_INTERVAL_SEC) {
                    spdlog::debug("Vérification forcée après {} secondes", timeElapsedSinceLastCheck);
                    lastCheckTime = currentTime;
                    
                    // Forcer une récupération de segment
                    auto hlsSegment = stream->hlsClient->getNextSegment();
                    
                    if (hlsSegment) {
                        spdlog::debug("VÉRIFICATION FORCÉE: Segment récupéré, seq: {}, taille: {} octets", 
                                  hlsSegment->sequenceNumber, hlsSegment->data.size());
                        
                        // Réinitialiser les compteurs d'état
                        waitingForNewSegment = false;
                        retryCount = 0;
                        emptyCount = 0;
                        consecutiveErrorCount = 0;
                        
                        // Mise à jour du temps et de la durée
                        lastSegmentTime = currentTime;
                        lastSegmentDuration = hlsSegment->duration > 0.0 ? hlsSegment->duration : 4.0;
                        lastProcessedSequenceNumber = hlsSegment->sequenceNumber;
                        
                        // Convertir et traiter le segment
                        processSegment(stream, *hlsSegment, segmentInProgress, sendStartTime);
                        
                        // Mettre à jour l'horodatage du dernier cycle réussi
                        lastSuccessfulCycleTime = currentTime;
                        healthCheckPassed = true;
                    } else {
                        spdlog::debug("VÉRIFICATION FORCÉE: Aucun segment disponible");
                    }
                    
                    continue;
                }
                
                // Vérifier s'il faut tenter de récupérer un nouveau segment
                bool shouldCheckForSegment = 
                    !segmentInProgress && (
                    lastSegmentDuration == 0.0 || 
                    elapsedSinceLastSegmentSec >= waitTimeSec);
                
                if (shouldCheckForSegment) {
                    spdlog::debug("Tentative de récupération d'un segment, waitTime={}s, elapsed={}s", 
                        waitTimeSec, elapsedSinceLastSegmentSec);
                    
                    lastCheckTime = currentTime;
                    
                    // Récupérer un segment HLS
                    auto hlsSegment = stream->hlsClient->getNextSegment();
                    
                    if (hlsSegment) {
                        // Segment disponible
                        waitingForNewSegment = false;
                        retryCount = 0;
                        emptyCount = 0;
                        consecutiveErrorCount = 0;
                        
                        spdlog::info("Segment récupéré: Flux: {}, Durée: {}s, Séquence: {}, Taille: {} octets, Discontinuité: {}", 
                            streamId, hlsSegment->duration, hlsSegment->sequenceNumber, 
                            hlsSegment->data.size(), hlsSegment->discontinuity ? "oui" : "non");
                        
                        // Vérifier que ce n'est pas un segment déjà traité
                        if (hlsSegment->sequenceNumber != lastProcessedSequenceNumber || hlsSegment->discontinuity) {
                            // Mise à jour du temps et de la durée
                            lastSegmentTime = currentTime;
                            lastSegmentDuration = hlsSegment->duration > 0.0 ? hlsSegment->duration : 4.0;
                            lastProcessedSequenceNumber = hlsSegment->sequenceNumber;
                            
                            // Traiter le segment
                            bool success = processSegment(stream, *hlsSegment, segmentInProgress, sendStartTime);
                            
                            // Mettre à jour l'état du flux
                            if (success) {
                                lastSuccessfulCycleTime = currentTime;
                                healthCheckPassed = true;
                            }
                        } else {
                            spdlog::debug("Segment {} déjà traité, ignoré", hlsSegment->sequenceNumber);
                        }
                    }
                    else {
                        // Aucun segment disponible
                        emptyCount++;
                        
                        // Après plusieurs tentatives, passer en mode attente
                        if (emptyCount > 5 && !waitingForNewSegment) {
                            waitingForNewSegment = true;
                            lastPlaylistRefreshTime = currentTime;
                            spdlog::info("Passage en mode attente pour nouveaux segments");
                        }
                        
                        if (emptyCount % 50 == 0) {
                            spdlog::info("Aucun segment HLS disponible (x{}), attente...", emptyCount);
                        }
                        
                        // Vérifier si nous sommes bloqués trop longtemps
                        retryCount++;
                        
                        if (retryCount == MAX_RETRIES_BEFORE_ACTION) {
                            spdlog::warn("Plusieurs vérifications sans segment ({}).", retryCount);
                        }
                        else if (retryCount >= MAX_RETRIES_BEFORE_RESTART) {
                            spdlog::warn("Trop de tentatives sans obtenir de segment ({}), redémarrage du client HLS", retryCount);
                            
                            try {
                                // Récupérer l'URL avant d'arrêter le client
                                std::string currentUrl = stream->hlsClient->getStreamInfo().url;
                                
                                // Arrêter et redémarrer le client
                                stream->hlsClient->stop();
                                std::this_thread::sleep_for(std::chrono::seconds(2));
                                
                                stream->hlsClient = std::make_shared<HLSClient>(currentUrl);
                                stream->hlsClient->start();
                                
                                // Réinitialiser les compteurs
                                retryCount = 0;
                                emptyCount = 0;
                                waitingForNewSegment = false;
                                
                                spdlog::info("Client HLS redémarré avec succès");
                            } catch (const std::exception& e) {
                                spdlog::error("Échec du redémarrage du client HLS: {}", e.what());
                                consecutiveErrorCount++;
                            }
                        }
                        
                        // Attente adaptée
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                }
                else {
                    // Pas encore temps de récupérer un nouveau segment
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
                
                // Incrémenter le compteur d'erreurs consécutives
                consecutiveErrorCount++;
                
                // Si trop d'erreurs consécutives, tenter une réinitialisation complète
                if (consecutiveErrorCount > MAX_CONSECUTIVE_ERRORS) {
                    spdlog::error("Trop d'erreurs consécutives ({}/{}), tentative de réinitialisation du flux",
                               consecutiveErrorCount, MAX_CONSECUTIVE_ERRORS);
                    
                    resetStream(streamId);
                    consecutiveErrorCount = 0;
                }
                
                // Attendre avant de réessayer
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        
        spdlog::info("Sortie normale de la boucle pour le flux: {}", streamId);
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
    
    spdlog::info("Fin du thread de traitement pour le flux: {}", streamId);
}                


bool StreamManager::processSegment(StreamInstance* stream, const HLSSegment& hlsSegment, 
                                 bool& segmentInProgress, std::chrono::steady_clock::time_point& sendStartTime) {
    if (!stream || !stream->mpegtsConverter || !stream->multicastSender || !stream->segmentBuffer) {
        spdlog::error("Composants non initialisés pour le traitement du segment");
        return false;
    }
    
    // Convertir le segment en MPEG-TS
    auto mpegtsSegment = stream->mpegtsConverter->convert(hlsSegment);
    if (!mpegtsSegment) {
        spdlog::error("Échec de conversion du segment HLS en MPEG-TS, séquence: {}", hlsSegment.sequenceNumber);
        return false;
    }
    
    spdlog::info("Segment {} converti en MPEG-TS, taille: {} octets", 
               mpegtsSegment->sequenceNumber, mpegtsSegment->data.size());
    
    // Analyser la qualité du segment
    if (stream->qualityMonitor) {
        auto stats = stream->qualityMonitor->analyze(mpegtsSegment->data);
        
        // Log seulement en cas de problème
        if (stats.pcrDiscontinuities > 0 || stats.continuityErrors > 0 || stats.pcrJitter > 0.5) {
            spdlog::warn("Problèmes détectés dans le segment {}: PCR discontinuités={}, CC erreurs={}, PCR jitter={}ms",
                      mpegtsSegment->sequenceNumber, stats.pcrDiscontinuities, stats.continuityErrors, stats.pcrJitter);
        }
    }
    
    // Ajouter le segment au buffer
    stream->segmentBuffer->pushSegment(*mpegtsSegment);
    spdlog::debug("Segment {} ajouté au buffer, taille du buffer: {}/{}", 
                mpegtsSegment->sequenceNumber, 
                stream->segmentBuffer->getCurrentSize(),
                stream->segmentBuffer->getBufferSize());
    
    // Récupérer un segment du buffer pour l'envoi
    MPEGTSSegment segmentToSend;
    if (!stream->segmentBuffer->getSegment(segmentToSend)) {
        spdlog::warn("Impossible de récupérer un segment du buffer pour l'envoi");
        return false;
    }
    
    spdlog::info("Segment {} récupéré du buffer, taille: {} octets, prêt pour envoi multicast",
               segmentToSend.sequenceNumber, segmentToSend.data.size());
    
    // Vérifier les données
    if (segmentToSend.data.empty()) {
        spdlog::error("Segment {} vide, ignoré pour l'envoi multicast", segmentToSend.sequenceNumber);
        return false;
    }
    
    // Vérifier le MulticastSender
    if (!stream->multicastSender->isRunning()) {
        spdlog::warn("MulticastSender non opérationnel, tentative de redémarrage");
        if (!stream->multicastSender->start()) {
            spdlog::error("Impossible de redémarrer le MulticastSender");
            return false;
        }
    }
    
    // Envoyer le segment en multicast
    spdlog::info("Tentative d'envoi du segment {} en multicast ({} octets, discontinuité: {})",
               segmentToSend.sequenceNumber, segmentToSend.data.size(), 
               segmentToSend.discontinuity ? "oui" : "non");
    
    bool sendResult = stream->multicastSender->send(segmentToSend.data, segmentToSend.discontinuity);
    
    if (!sendResult) {
        spdlog::error("Échec d'envoi du segment {} multicast", segmentToSend.sequenceNumber);
        return false;
    }
    
    spdlog::info("Segment {} envoyé avec succès en multicast", segmentToSend.sequenceNumber);
    sendStartTime = std::chrono::steady_clock::now();
    segmentInProgress = true;
    
    return true;
}

bool StreamManager::resetStream(const std::string& streamId) {
    spdlog::info("Réinitialisation complète du flux {}", streamId);
    
    // Récupérer la configuration du flux
    const StreamConfig* config = config_->getStreamConfig(streamId);
    if (!config) {
        spdlog::error("Configuration du flux non trouvée pour la réinitialisation: {}", streamId);
        return false;
    }
    
    // Accéder au flux existant
    StreamInstance* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        auto it = streams_.find(streamId);
        if (it == streams_.end()) {
            spdlog::error("Flux non trouvé pour la réinitialisation: {}", streamId);
            return false;
        }
        stream = &it->second;
    }
    
    // Arrêter les composants existants
    try {
        spdlog::info("Arrêt des composants existants pour le flux {}", streamId);
        
        if (stream->hlsClient) {
            stream->hlsClient->stop();
        }
        
        if (stream->mpegtsConverter) {
            stream->mpegtsConverter->stop();
        }
        
        if (stream->multicastSender) {
            stream->multicastSender->stop();
        }
        
        // Attendre un peu pour s'assurer que tout est arrêté
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Récréer et redémarrer les composants
        spdlog::info("Recréation et redémarrage des composants pour le flux {}", streamId);
        
        // Conserver le buffer existant si possible
        if (!stream->segmentBuffer) {
            stream->segmentBuffer = std::make_shared<SegmentBuffer>(config->bufferSize);
        } else {
            stream->segmentBuffer->clear();
        }
        
        // Créer un nouveau client HLS
        stream->hlsClient = std::make_shared<HLSClient>(config->hlsInput);
        stream->hlsClient->start();
        
        // Vérifier si c'est un flux live valide
        if (!stream->hlsClient->isLiveStream() || !stream->hlsClient->isValidStream()) {
            spdlog::error("Le flux HLS n'est pas valide après réinitialisation");
            
            AlertManager::getInstance().addAlert(
                AlertLevel::ERROR,
                "StreamManager",
                "Échec de la réinitialisation du flux " + streamId + " : flux HLS non valide",
                true
            );
            
            return false;
        }
        
        // Recréer le convertisseur
        stream->mpegtsConverter = std::make_shared<MPEGTSConverter>();
        stream->mpegtsConverter->start();
        
        // Réinitialiser le MulticastSender
        stream->multicastSender = std::make_shared<MulticastSender>(
            config->mcastOutput, 
            config->mcastPort,
            config->mcastInterface,
            4
        );
        
        if (!stream->multicastSender->initialize() || !stream->multicastSender->start()) {
            spdlog::error("Échec de l'initialisation du MulticastSender après réinitialisation");
            
            AlertManager::getInstance().addAlert(
                AlertLevel::ERROR,
                "StreamManager",
                "Échec de la réinitialisation du flux " + streamId + " : problème avec le MulticastSender",
                true
            );
            
            return false;
        }
        
        // Réinitialiser le moniteur de qualité
        if (!stream->qualityMonitor) {
            stream->qualityMonitor = std::make_shared<TSQualityMonitor>();
        } else {
            stream->qualityMonitor->reset();
        }
        
        spdlog::info("Flux {} réinitialisé avec succès", streamId);
        
        AlertManager::getInstance().addAlert(
            AlertLevel::INFO,
            "StreamManager",
            "Flux " + streamId + " réinitialisé avec succès après détection de problèmes",
            false
        );
        
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la réinitialisation du flux {}: {}", streamId, e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "StreamManager",
            "Exception lors de la réinitialisation du flux " + streamId + ": " + e.what(),
            true
        );
        
        return false;
    }
}

// Fonction de test direct pour vérifier chaque composant
void StreamManager::testDirectDataFlow(const std::string& streamId) {
    spdlog::info("===== TEST DIRECT DU FLUX DE DONNÉES POUR {} =====", streamId);
    
    StreamInstance* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        auto it = streams_.find(streamId);
        if (it == streams_.end()) {
            spdlog::error("TEST: Flux non trouvé: {}", streamId);
            return;
        }
        stream = &it->second;
    }
    
    // 1. Vérifier l'état du HLSClient
    if (!stream->hlsClient) {
        spdlog::error("TEST: HLSClient non initialisé");
        return;
    }
    
    spdlog::info("TEST: HLSClient état: {}", stream->hlsClient->isRunning() ? "en cours d'exécution" : "arrêté");
    
    // 2. Tenter de récupérer un segment directement
    auto hlsSegment = stream->hlsClient->getNextSegment();
    if (hlsSegment) {
        spdlog::info("TEST: Segment HLS récupéré: seq={}, taille={} octets", 
                  hlsSegment->sequenceNumber, hlsSegment->data.size());
        
        // 3. Tester la conversion en MPEG-TS
        if (!stream->mpegtsConverter) {
            spdlog::error("TEST: MPEGTSConverter non initialisé");
            return;
        }
        
        auto mpegtsSegment = stream->mpegtsConverter->convert(*hlsSegment);
        if (mpegtsSegment) {
            spdlog::info("TEST: Segment converti en MPEG-TS: seq={}, taille={} octets", 
                      mpegtsSegment->sequenceNumber, mpegtsSegment->data.size());
            
            // 4. Tester le buffer de segments
            if (!stream->segmentBuffer) {
                spdlog::error("TEST: SegmentBuffer non initialisé");
                return;
            }
            
            stream->segmentBuffer->pushSegment(*mpegtsSegment);
            spdlog::info("TEST: Segment ajouté au buffer, taille du buffer: {}", 
                      stream->segmentBuffer->getCurrentSize());
            
            // 5. Récupérer du buffer et tester l'envoi multicast
            MPEGTSSegment segmentFromBuffer;
            if (stream->segmentBuffer->getSegment(segmentFromBuffer)) {
                spdlog::info("TEST: Segment récupéré du buffer: seq={}, taille={} octets", 
                          segmentFromBuffer.sequenceNumber, segmentFromBuffer.data.size());
                
                // 6. Tester l'envoi multicast
                if (!stream->multicastSender) {
                    spdlog::error("TEST: MulticastSender non initialisé");
                    return;
                }
                
                bool sendResult = stream->multicastSender->send(segmentFromBuffer.data, segmentFromBuffer.discontinuity);
                spdlog::info("TEST: Résultat de l'envoi multicast: {}", sendResult ? "réussi" : "échec");
            } else {
                spdlog::error("TEST: Impossible de récupérer le segment du buffer");
            }
        } else {
            spdlog::error("TEST: Échec de la conversion en MPEG-TS");
        }
    } else {
        spdlog::warn("TEST: Aucun segment HLS disponible");
        
        // Fabriquer un segment de test artificiel pour tester le reste de la chaîne
        spdlog::info("TEST: Création d'un segment de test artificiel");
        
        HLSSegment testSegment;
        testSegment.data.resize(188 * 100, 0xFF); // 100 paquets MPEG-TS
        testSegment.sequenceNumber = 9999;
        testSegment.duration = 4.0;
        testSegment.discontinuity = false;
        testSegment.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        // Tester le reste de la chaîne avec ce segment artificiel...
        auto mpegtsSegment = stream->mpegtsConverter->convert(testSegment);
        if (mpegtsSegment) {
            spdlog::info("TEST: Segment artificiel converti en MPEG-TS: taille={} octets", 
                      mpegtsSegment->data.size());
            
            // Suite des tests...
            stream->segmentBuffer->pushSegment(*mpegtsSegment);
            
            MPEGTSSegment segmentFromBuffer;
            if (stream->segmentBuffer->getSegment(segmentFromBuffer)) {
                spdlog::info("TEST: Segment artificiel récupéré du buffer: taille={} octets", 
                          segmentFromBuffer.data.size());
                
                bool sendResult = stream->multicastSender->send(segmentFromBuffer.data, segmentFromBuffer.discontinuity);
                spdlog::info("TEST: Résultat de l'envoi multicast du segment artificiel: {}", 
                          sendResult ? "réussi" : "échec");
            }
        }
    }
    
    spdlog::info("===== FIN DU TEST DIRECT =====");
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
