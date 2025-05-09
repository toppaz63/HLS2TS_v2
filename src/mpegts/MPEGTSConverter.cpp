#include "mpegts/MPEGTSConverter.h"
#include "mpegts/DVBProcessor.h"
#include "alerting/AlertManager.h"  
#include "spdlog/spdlog.h"

// Utilisation de TSDuck pour manipuler les paquets MPEG-TS
#include <tsduck/tsduck.h>

using namespace hls_to_dvb;

MPEGTSConverter::MPEGTSConverter()
    : running_(false), lastPcrValue_(0), pcrPid_(0x1FFF) {
    
    // Initialiser les compteurs de continuité
    resetContinuityCounters();
}

void MPEGTSConverter::resetContinuityCountersInternal() {
    //std::lock_guard<std::mutex> lock(mutex_);
    continuityCounters_.clear();
    spdlog::info("**** MPEGTSConverter::resetContinuityCountersInternal() **** Réinitialisation des compteurs de continuité");
}

void MPEGTSConverter::resetContinuityCounters() {
    std::lock_guard<std::mutex> lock(mutex_);
    resetContinuityCountersInternal();
    spdlog::info("**** MPEGTSConverter::resetContinuityCounters() **** Réinitialisation des compteurs de continuité");
}

void MPEGTSConverter::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (running_) {
        spdlog::warn("Le convertisseur MPEG-TS est déjà en cours d'exécution");
        return;
    }
    
    spdlog::info("**** MPEGTSConverter::start() **** Démarrage du convertisseur MPEG-TS");
    
    try {
        // Initialiser le processeur DVB
        dvbProcessor_ = std::make_unique<DVBProcessor>();
        dvbProcessor_->initialize();
        spdlog::info("**** MPEGTSConverter::start() **** DVBProcessor initialisé avec succès");
        
        // Réinitialiser les variables d'état
        lastPcrValue_ = 0;
        pcrPid_ = 0x1FFF; // Valeur invalide par défaut
        spdlog::info("**** MPEGTSConverter::start() **** Réinitialisation des variables d'état");
        resetContinuityCountersInternal();
        spdlog::info("**** MPEGTSConverter::start() **** Réinitialisation des compteurs de continuité");
        
        running_ = true;
        
        AlertManager::getInstance().addAlert(
            AlertLevel::INFO,
            "MPEGTSConverter",
            "Convertisseur MPEG-TS démarré",
            false
        );
        
        spdlog::info("**** MPEGTSConverter::start() **** Convertisseur MPEG-TS démarré avec succès");
    }
    catch (const ts::Exception& e) {
        spdlog::error("Erreur TSDuck lors du démarrage du convertisseur MPEG-TS: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "MPEGTSConverter",
            std::string("Erreur TSDuck lors du démarrage du convertisseur MPEG-TS: ") + e.what(),
            true
        );
        
        throw;
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors du démarrage du convertisseur MPEG-TS: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "MPEGTSConverter",
            std::string("Erreur lors du démarrage du convertisseur MPEG-TS: ") + e.what(),
            true
        );
        
        throw;
    }
}

void MPEGTSConverter::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!running_) {
        spdlog::warn("Le convertisseur MPEG-TS n'est pas en cours d'exécution");
        return;
    }
    
    spdlog::info("Arrêt du convertisseur MPEG-TS");
    
    // Libérer les ressources
    if (dvbProcessor_) {
        dvbProcessor_->cleanup();
        dvbProcessor_.reset();
    }
    
    running_ = false;
    
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "MPEGTSConverter",
        "Convertisseur MPEG-TS arrêté",
        false
    );
}

std::optional<MPEGTSSegment> MPEGTSConverter::convert(const HLSSegment& hlsSegment) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!running_ || !dvbProcessor_) {
        spdlog::error("Le convertisseur MPEG-TS n'est pas démarré");
        return std::nullopt;
    }
    
    try {
        spdlog::debug("Conversion du segment HLS {} en MPEG-TS (discontinuité: {})",
                    hlsSegment.sequenceNumber, hlsSegment.discontinuity ? "oui" : "non");
        
        // Si le segment HLS est déjà en MPEG-TS (ce qui est généralement le cas),
        // nous devons traiter le flux MPEG-TS pour assurer sa conformité DVB
        
        // Extraction des paquets MPEG-TS
        ts::TSPacketVector packets;
        
        if (hlsSegment.data.size() % ts::PKT_SIZE != 0) {
            spdlog::warn("Taille de données non multiple de la taille d'un paquet TS: {}", hlsSegment.data.size());
            
            // Si la taille est trop petite pour contenir même un seul paquet TS
            if (hlsSegment.data.size() < ts::PKT_SIZE) {
                spdlog::error("Segment trop petit pour être traité: {} octets (minimum: {} octets)", 
                            hlsSegment.data.size(), ts::PKT_SIZE);
                
                hls_to_dvb::AlertManager::getInstance().addAlert(
                    hls_to_dvb::AlertLevel::ERROR,
                    "MPEGTSConverter",
                    "Segment trop petit pour être traité: " + std::to_string(hlsSegment.data.size()) + 
                    " octets (minimum: " + std::to_string(ts::PKT_SIZE) + " octets)",
                    false
                );
                
                return std::nullopt;
            }
            
            // Tronquer aux paquets complets
            size_t validSize = (hlsSegment.data.size() / ts::PKT_SIZE) * ts::PKT_SIZE;
            size_t truncatedBytes = hlsSegment.data.size() - validSize;
            
            spdlog::info("Troncature du segment de {} octets à {} octets (suppression de {} octets de rembourrage)", 
                    hlsSegment.data.size(), validSize, truncatedBytes);
            
            // Créer une copie tronquée des données
            std::vector<uint8_t> truncatedData(hlsSegment.data.begin(), hlsSegment.data.begin() + validSize);
            
            // Charger manuellement les paquets depuis les données tronquées
            size_t packetCount = truncatedData.size() / ts::PKT_SIZE;
            packets.reserve(packetCount);
            
            for (size_t i = 0; i < packetCount; ++i) {
                ts::TSPacket packet;
                std::memcpy(packet.b, truncatedData.data() + (i * ts::PKT_SIZE), ts::PKT_SIZE);
                packets.push_back(packet);
            }
        }
        else {
            // Charger manuellement les paquets depuis les données originales
            size_t packetCount = hlsSegment.data.size() / ts::PKT_SIZE;
            packets.reserve(packetCount);
            
            for (size_t i = 0; i < packetCount; ++i) {
                ts::TSPacket packet;
                std::memcpy(packet.b, hlsSegment.data.data() + (i * ts::PKT_SIZE), ts::PKT_SIZE);
                packets.push_back(packet);
            }
        }
        
        // Vérifier si les paquets sont valides
        if (packets.empty()) {
            spdlog::error("Aucun paquet MPEG-TS valide trouvé dans le segment HLS {}", 
                        hlsSegment.sequenceNumber);
            
            AlertManager::getInstance().addAlert(
                AlertLevel::ERROR,
                "MPEGTSConverter",
                "Aucun paquet MPEG-TS valide trouvé dans le segment HLS " + 
                std::to_string(hlsSegment.sequenceNumber),
                true
            );
            
            return std::nullopt;
        }
        
        // Appliquer les compteurs de continuité et gérer les PCR
        processPackets(packets, hlsSegment.discontinuity);
        
        // Convertir les paquets en vecteur d'octets pour le traitement
        std::vector<uint8_t> tsData;
        tsData.reserve(packets.size() * ts::PKT_SIZE);
        
        for (const auto& packet : packets) {
            const uint8_t* packetData = packet.b;
            tsData.insert(tsData.end(), packetData, packetData + ts::PKT_SIZE);
        }
        
        // Mettre à jour les tables PSI/SI avec indication de discontinuité
        std::vector<uint8_t> finalData = dvbProcessor_->updatePSITables(tsData, hlsSegment.discontinuity);
        
        // Créer le segment MPEG-TS de sortie
        MPEGTSSegment mpegtsSegment;
        mpegtsSegment.data = finalData;
        mpegtsSegment.discontinuity = hlsSegment.discontinuity;
        mpegtsSegment.sequenceNumber = hlsSegment.sequenceNumber;
        mpegtsSegment.duration = hlsSegment.duration;
        mpegtsSegment.timestamp = hlsSegment.timestamp;
        
        // Journaliser le succès
        spdlog::debug("Segment MPEG-TS {} généré avec succès, taille: {} octets",
                    mpegtsSegment.sequenceNumber, mpegtsSegment.data.size());
        
        return mpegtsSegment;
    }
    catch (const ts::Exception& e) {
        spdlog::error("Exception TSDuck lors de la conversion MPEG-TS: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "MPEGTSConverter",
            std::string("Exception TSDuck lors de la conversion MPEG-TS: ") + e.what(),
            true
        );
        
        return std::nullopt;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la conversion MPEG-TS: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "MPEGTSConverter",
            std::string("Exception lors de la conversion MPEG-TS: ") + e.what(),
            true
        );
        
        return std::nullopt;
    }
}

void MPEGTSConverter::processPackets(ts::TSPacketVector& packets, bool discontinuity) {
    try {
        // Structure pour suivre les PID et les PCR
        bool firstPcrFound = false;
        std::map<uint16_t, bool> pidHasDiscontinuity;
        
        // Si c'est une discontinuité, réinitialiser l'état PCR
        if (discontinuity) {
            spdlog::info("Discontinuité détectée, préparation au traitement des PCR et compteurs de continuité");
            firstPcrFound = false;
            
            // Marquer tous les PID comme ayant une discontinuité
            for (const auto& pair : continuityCounters_) {
                pidHasDiscontinuity[pair.first] = true;
            }
        }
        
        // Premier passage pour identifier le PID PCR principal si nécessaire
        if (pcrPid_ == 0x1FFF) {
            for (const auto& packet : packets) {
                if (packet.hasPCR()) {
                    pcrPid_ = packet.getPID();
                    spdlog::info("PID PCR principal détecté: 0x{:04X}", pcrPid_);
                    break;
                }
            }
        }
        
        // Deuxième passage pour traiter les paquets
        for (auto& packet : packets) {
            uint16_t pid = packet.getPID();
            
            // Vérifier si c'est un paquet nul (PID = 0x1FFF)
            bool isNullPacket = (pid == 0x1FFF);
            bool hasAdaptationField = packet.hasAF();
            
            // Appliquer le compteur de continuité
            if (!isNullPacket && !hasAdaptationField) {
                // Si c'est la première fois qu'on voit ce PID ou s'il y a une discontinuité
                if (continuityCounters_.find(pid) == continuityCounters_.end() || 
                    (discontinuity && pidHasDiscontinuity[pid])) {
                    // Initialiser le compteur ou le réinitialiser après discontinuité
                    continuityCounters_[pid] = 0;
                    pidHasDiscontinuity[pid] = false;
                } else {
                    // Incrémenter le compteur normalement
                    continuityCounters_[pid] = (continuityCounters_[pid] + 1) & 0x0F;
                }
                
                // Définir le compteur dans le paquet
                packet.setCC(continuityCounters_[pid]);
            }
            
            // Traiter les PCR
            if (packet.hasPCR()) {
                uint64_t currentPcr = packet.getPCR();
                
                // Si c'est une discontinuité et premier PCR rencontré
                if (discontinuity && !firstPcrFound) {
                    // Marquer le paquet avec l'indicateur de discontinuité
                    packet.setDiscontinuityIndicator(true);
                    firstPcrFound = true;
                    
                    // Enregistrer cette valeur comme nouvelle base PCR
                    lastPcrValue_ = currentPcr;
                    
                    spdlog::info("Discontinuité PCR appliquée sur le PID 0x{:04X}, PCR: {}", pid, currentPcr);
                } 
                else if (discontinuity && firstPcrFound && pid == pcrPid_) {
                    // Si c'est toujours une discontinuité mais pas le premier PCR,
                    // ajustement basé sur la nouvelle base PCR
                    uint64_t expectedPcr = lastPcrValue_ + 27000000 * 0.04; // 40ms d'incrément typique
                    
                    // Mise à jour de la dernière valeur PCR connue
                    lastPcrValue_ = currentPcr;
                    
                    spdlog::debug("PCR suivant dans la discontinuité, PID 0x{:04X}, PCR: {}, attendu: {}", 
                               pid, currentPcr, expectedPcr);
                }
                else {
                    // PCR normal, vérifier qu'il est cohérent
                    if (lastPcrValue_ > 0 && currentPcr < lastPcrValue_) {
                        spdlog::warn("PCR non monotone détecté: {} -> {}", lastPcrValue_, currentPcr);
                    }
                    
                    // Mise à jour de la dernière valeur PCR
                    lastPcrValue_ = currentPcr;
                }
            }
        }
    }
    catch (const ts::Exception& e) {
        spdlog::error("Exception TSDuck lors du traitement des paquets: {}", e.what());
        throw;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors du traitement des paquets: {}", e.what());
        throw;
    }
}

bool MPEGTSConverter::isRunning() const {
    return running_;
}

MPEGTSConverter::~MPEGTSConverter() {
    if (running_) {
        stop();
    }
}
