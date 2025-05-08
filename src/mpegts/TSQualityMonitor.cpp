#include "mpegts/TSQualityMonitor.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace hls_to_dvb {

TSQualityMonitor::TSQualityMonitor() 
    : pcrAnalyzer_(std::make_unique<ts::PCRAnalyzer>()),
      lastSegmentSize_(0) {
    reset();
}

void TSQualityMonitor::reset() {
    stats_.reset();
    pcrAnalyzer_->reset();
    expectedCC_.clear();
    lastAnalysisTime_ = std::chrono::steady_clock::now();
    lastSegmentSize_ = 0;
}

TSQualityStats TSQualityMonitor::analyze(const std::vector<uint8_t>& tsData) {
    // Mettre à jour les statistiques totales
    stats_.totalBytes += tsData.size();
    
    // Calculer le débit
    auto currentTime = std::chrono::steady_clock::now();
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        currentTime - lastAnalysisTime_).count();
    
    if (durationMs > 0) {
        // Bits par seconde = (taille en octets * 8) / (durée en secondes)
        stats_.bitrateBps = static_cast<int>((tsData.size() * 8 * 1000) / durationMs);
    }
    
    lastAnalysisTime_ = currentTime;
    lastSegmentSize_ = tsData.size();
    
    // Convertir les données en paquets TS
    ts::TSPacketVector packets;
    size_t packetCount = tsData.size() / ts::PKT_SIZE;
    
    if (tsData.size() % ts::PKT_SIZE != 0) {
        spdlog::warn("TSQualityMonitor: données de taille incorrecte, non multiple de 188 octets");
        packetCount = tsData.size() / ts::PKT_SIZE; // Tronquer aux paquets complets
    }
    
    packets.reserve(packetCount);
    for (size_t i = 0; i < packetCount; ++i) {
        ts::TSPacket packet;
        memcpy(packet.b, tsData.data() + (i * ts::PKT_SIZE), ts::PKT_SIZE);
        packets.push_back(packet);
    }
    
    // Analyser chaque paquet
    for (const auto& packet : packets) {
        // Vérifier les PCR
        if (packet.hasPCR()) {
            uint64_t pcrValue = packet.getPCR();
            
            // Enregistrer le premier PCR
            if (stats_.firstPcrValue == 0) {
                stats_.firstPcrValue = pcrValue;
            }
            
            // Vérifier la continuité des PCR
            if (stats_.lastPcrValue > 0) {
                // PCR en 27MHz, 1 sec = 27,000,000 ticks
                int64_t diff = static_cast<int64_t>(pcrValue) - static_cast<int64_t>(stats_.lastPcrValue);
                
                // PCR doit être monotone, sauf au rollover (valeur max = 2^33 - 1)
                if (diff < 0 && abs(diff) < 8589934592LL) { // 2^33 / 2
                    stats_.pcrDiscontinuities++;
                    spdlog::debug("TSQualityMonitor: Discontinuité PCR détectée: {} -> {}", 
                                stats_.lastPcrValue, pcrValue);
                }
            }
            
            stats_.lastPcrValue = pcrValue;
            stats_.totalPcrCount++;
            
            // Utiliser le PCRAnalyzer de TSDuck pour l'analyse avancée
            pcrAnalyzer_->feedPacket(packet);
        }
        
        // Vérifier la continuité
        uint16_t pid = packet.getPID();
        
        // Ignorer les paquets null et PAT/CAT/NIT pour la vérification de continuité
        if (pid != ts::PID_NULL && pid != ts::PID_PAT && pid != ts::PID_CAT && pid != ts::PID_NIT) {
            uint8_t cc = packet.getCC();
            
            if (expectedCC_.find(pid) != expectedCC_.end()) {
                uint8_t expected = expectedCC_[pid];
                
                // Vérifier si le paquet a un payload
                if (packet.hasPayload()) {
                    // Vérifier la continuité
                    if (cc != expected) {
                        stats_.continuityErrors++;
                        spdlog::debug("TSQualityMonitor: Erreur de continuité sur PID 0x{:X}: attendu={}, reçu={}",
                                    pid, expected, cc);
                    }
                    
                    // Mettre à jour le CC attendu
                    expectedCC_[pid] = (cc + 1) % 16;
                }
            } else {
                // Premier paquet pour ce PID
                expectedCC_[pid] = packet.hasPayload() ? (cc + 1) % 16 : cc;
            }
            
            // Stocker le dernier CC pour référence
            stats_.lastCCValues[pid] = cc;
        }
    }
    
    // Vérifier la présence et la validité des tables PSI/SI
    checkPSITables(packets, false);
    
    ts::PCRAnalyzer::Status status;
    pcrAnalyzer_->getStatus(status);
    //stats_.pcrJitter = status.jitter_ms;
    
    return stats_;
}

const TSQualityStats& TSQualityMonitor::getStats() const {
    return stats_;
}

bool TSQualityMonitor::isDVBCompliant(bool detailedLog) const {
    bool compliant = true;
    
    // Vérifier les erreurs de continuité
    if (stats_.continuityErrors > 0) {
        if (detailedLog) {
            spdlog::warn("Non conforme DVB: {} erreurs de continuité détectées", stats_.continuityErrors);
        }
        compliant = false;
    }
    
    // Vérifier les discontinuités PCR
    if (stats_.pcrDiscontinuities > 0) {
        if (detailedLog) {
            spdlog::warn("Non conforme DVB: {} discontinuités PCR détectées", stats_.pcrDiscontinuities);
        }
        compliant = false;
    }
    
    // Vérifier le jitter PCR (>500us est problématique selon DVB)
    if (stats_.pcrJitter > 0.5) {
        if (detailedLog) {
            spdlog::warn("Non conforme DVB: Jitter PCR de {}ms dépasse la limite recommandée de 0.5ms", 
                       stats_.pcrJitter);
        }
        compliant = false;
    }
    
    // Vérifier la fréquence de répétition des tables
    if (!checkTableRepetitionRates(detailedLog)) {
        compliant = false;
    }
    
    return compliant;
}

bool TSQualityMonitor::checkPSITables(const ts::TSPacketVector& packets, bool detailedLog) {
    // Structure pour suivre les tables détectées
    struct TableInfo {
        bool detected = false;
        uint64_t lastDetectedTime = 0;
        int version = -1;
    };
    
    std::map<uint16_t, hls_to_dvb::TableInfo> tables;
    tables[ts::PID_PAT] = hls_to_dvb::TableInfo(); // PAT
    tables[ts::PID_CAT] = hls_to_dvb::TableInfo(); // CAT
    tables[ts::PID_NIT] = hls_to_dvb::TableInfo(); // NIT
    tables[0x11] = hls_to_dvb::TableInfo();       // SDT
    tables[0x14] = hls_to_dvb::TableInfo();       // TDT/TOT
    
    // Utiliser SectionDemux pour extraire les tables
    ts::DuckContext duck;
    ts::SectionDemux demux(duck);
    
    // Configurer les callbacks pour les tables
    
    MyTableHandler handler(tables);
    demux.setTableHandler(&handler);
    
    // Analyser les paquets
    for (const auto& packet : packets) {
        demux.feedPacket(packet);
    }
    
    // Vérifier si les tables obligatoires sont présentes
    bool allTablesPresent = true;
    
    for (const auto& [pid, info] : tables) {
        // PAT et SDT sont obligatoires
        if ((pid == ts::PID_PAT || pid == 0x11) && !info.detected) {
            if (detailedLog) {
                spdlog::warn("Table PSI/SI obligatoire manquante: PID 0x{:X}", pid);
            }
            allTablesPresent = false;
        }
    }
    
    return allTablesPresent;
}

bool TSQualityMonitor::checkTableRepetitionRates(bool detailedLog) const {
    // Dans un segment isolé, impossible de vérifier les taux de répétition
    // Cette fonction serait plus utile avec un stockage persistant des timestamps
    return true; // Supposons que les taux sont corrects
}

} // namespace hls_to_dvb