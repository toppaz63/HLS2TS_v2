#include "mpegts/MPEGTSConverter.h"
#include "mpegts/DVBProcessor.h"
#include "alerting/AlertManager.h"
#include "spdlog/spdlog.h"


// Utilisation de TSDuck pour manipuler les paquets MPEG-TS
// Note: Cette implémentation suppose que les bibliothèques TSDuck sont disponibles
#include <tsduck/tsduck.h>

using namespace hls_to_dvb;

MPEGTSConverter::MPEGTSConverter()
    : running_(false) {
    
    // Le processeur DVB sera initialisé dans la méthode start()
}

void MPEGTSConverter::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (running_) {
        spdlog::warn("Le convertisseur MPEG-TS est déjà en cours d'exécution");
        return;
    }
    
    spdlog::info("Démarrage du convertisseur MPEG-TS");
    
    try {
        // Initialiser le processeur DVB
        dvbProcessor_ = std::make_unique<DVBProcessor>();
        dvbProcessor_->initialize();
        
        running_ = true;
        
        hls_to_dvb::AlertManager::getInstance().addAlert(
            hls_to_dvb::AlertLevel::INFO,
            "MPEGTSConverter",
            "Convertisseur MPEG-TS démarré",
            false
        );
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors du démarrage du convertisseur MPEG-TS: {}", e.what());
        
        hls_to_dvb::AlertManager::getInstance().addAlert(
            hls_to_dvb::AlertLevel::ERROR,
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
    
    hls_to_dvb::AlertManager::getInstance().addAlert(
        hls_to_dvb::AlertLevel::INFO,
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
        
        // Utilisation d'une méthode alternative compatible avec TSDuck 3.40
        // Au lieu d'utiliser FromBytes qui n'existe pas, nous allons construire manuellement les paquets
        if (hlsSegment.data.size() % ts::PKT_SIZE != 0) {
            spdlog::error("Taille de données non multiple de la taille d'un paquet TS: {}", hlsSegment.data.size());
            
            hls_to_dvb::AlertManager::getInstance().addAlert(
                hls_to_dvb::AlertLevel::ERROR,
                "MPEGTSConverter",
                "Taille de données non multiple de la taille d'un paquet TS: " + 
                std::to_string(hlsSegment.data.size()),
                true
            );
            
            return std::nullopt;
        }
        
        size_t packetCount = hlsSegment.data.size() / ts::PKT_SIZE;
        for (size_t i = 0; i < packetCount; ++i) {
            ts::TSPacket packet;
            const uint8_t* packetData = hlsSegment.data.data() + (i * ts::PKT_SIZE);
            std::memcpy(packet.b, packetData, ts::PKT_SIZE);
            packets.push_back(packet);
        }
        
        // Vérifier si les paquets sont valides
        if (packets.empty()) {
            spdlog::error("Aucun paquet MPEG-TS valide trouvé dans le segment HLS {}", 
                        hlsSegment.sequenceNumber);
            
            hls_to_dvb::AlertManager::getInstance().addAlert(
                hls_to_dvb::AlertLevel::ERROR,
                "MPEGTSConverter",
                "Aucun paquet MPEG-TS valide trouvé dans le segment HLS " + 
                std::to_string(hlsSegment.sequenceNumber),
                true
            );
            
            return std::nullopt;
        }
        
        // Convertir les paquets en vecteur d'octets pour le traitement
        std::vector<uint8_t> tsData;
        tsData.reserve(packets.size() * ts::PKT_SIZE);
        
        for (const auto& packet : packets) {
            const uint8_t* packetData = packet.b;
            tsData.insert(tsData.end(), packetData, packetData + ts::PKT_SIZE);
        }
        
        // Traiter les discontinuités si nécessaire
        std::vector<uint8_t> discontinuityProcessed;
        if (hlsSegment.discontinuity) {
            discontinuityProcessed = processDiscontinuity(tsData, true);
        } else {
            discontinuityProcessed = tsData;
        }
        
        // Mettre à jour les tables PSI/SI
        std::vector<uint8_t> finalData = updatePSITables(discontinuityProcessed);
        
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
        
        hls_to_dvb::AlertManager::getInstance().addAlert(
            hls_to_dvb::AlertLevel::ERROR,
            "MPEGTSConverter",
            std::string("Exception TSDuck lors de la conversion MPEG-TS: ") + e.what(),
            true
        );
        
        return std::nullopt;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la conversion MPEG-TS: {}", e.what());
        
        hls_to_dvb::AlertManager::getInstance().addAlert(
            hls_to_dvb::AlertLevel::ERROR,
            "MPEGTSConverter",
            std::string("Exception lors de la conversion MPEG-TS: ") + e.what(),
            true
        );
        
        return std::nullopt;
    }
}

std::vector<uint8_t> MPEGTSConverter::processDiscontinuity(const std::vector<uint8_t>& data, bool discontinuity) {
    if (!discontinuity || data.empty()) {
        return data;
    }
    
    spdlog::info("Traitement d'une discontinuité dans le flux MPEG-TS");
    
    try {
        // Convertir le vecteur d'octets en paquets MPEG-TS
        ts::TSPacketVector packets;
        size_t packetCount = data.size() / ts::PKT_SIZE;
        
        // Journaliser plus d'informations
        spdlog::debug("Nombre de paquets: {}", packetCount);
        
        // Charger les paquets
        for (size_t i = 0; i < packetCount; ++i) {
            ts::TSPacket packet;
            std::memcpy(packet.b, &data[i * ts::PKT_SIZE], ts::PKT_SIZE);
            packets.push_back(packet);
        }
        
        // Traiter chaque paquet
        bool foundPCR = false;
        for (auto& packet : packets) {
            try {
                // Vérifier si le paquet a un PCR de manière sécurisée
                if (packet.hasPCR()) {
                    packet.setDiscontinuityIndicator(true);
                    foundPCR = true;
                    
                    // Journaliser le PID et le PCR
                    uint16_t pid = packet.getPID();
                    uint64_t pcr = packet.getPCR();
                    spdlog::debug("Marqueur de discontinuité défini sur le paquet PID: 0x{:X}, PCR: {}", pid, pcr);
                }
            }
            catch (const std::exception& e) {
                spdlog::error("Erreur lors du traitement d'un paquet: {}", e.what());
            }
        }
        
        if (!foundPCR) {
            spdlog::warn("Aucun paquet avec PCR trouvé dans le segment");
        }
        
        // Convertir les paquets en vecteur d'octets
        std::vector<uint8_t> result;
        result.reserve(packets.size() * ts::PKT_SIZE);
        
        for (const auto& packet : packets) {
            const uint8_t* packetData = packet.b;
            result.insert(result.end(), packetData, packetData + ts::PKT_SIZE);
        }
        
        return result;
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors du traitement de la discontinuité: {}", e.what());
        
        // En cas d'erreur, retourner les données d'origine
        return data;
    }
}

std::vector<uint8_t> MPEGTSConverter::updatePSITables(const std::vector<uint8_t>& data) {
    if (data.empty() || !dvbProcessor_) {
        return data;
    }
    
    try {
        // Utiliser le processeur DVB pour mettre à jour les tables PSI/SI
        return dvbProcessor_->updatePSITables(data);
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de la mise à jour des tables PSI/SI: {}", e.what());
        
        // En cas d'erreur, retourner les données d'origine
        return data;
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
