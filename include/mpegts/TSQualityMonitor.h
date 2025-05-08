#pragma once

#include <vector>
#include <map>
#include <memory>
#include <tsduck/tsduck.h>

namespace hls_to_dvb {

/**
 * @struct TSQualityStats
 * @brief Statistiques sur la qualité d'un flux MPEG-TS
 */
struct TSQualityStats {
    int pcrDiscontinuities = 0;     ///< Nombre de discontinuités PCR
    int continuityErrors = 0;       ///< Nombre d'erreurs de continuité
    double pcrJitter = 0.0;         ///< Jitter PCR en millisecondes
    uint32_t totalPcrCount = 0;     ///< Nombre total de PCR analysés
    uint64_t lastPcrValue = 0;      ///< Dernière valeur PCR observée
    uint64_t firstPcrValue = 0;     ///< Première valeur PCR observée
    std::map<uint16_t, uint8_t> lastCCValues; ///< Dernier compteur de continuité par PID
    int bitrateBps = 0;             ///< Débit instantané en bits par seconde
    uint64_t totalBytes = 0;        ///< Nombre total d'octets traités
    
    void reset() {
        pcrDiscontinuities = 0;
        continuityErrors = 0;
        pcrJitter = 0.0;
        totalPcrCount = 0;
        lastPcrValue = 0;
        firstPcrValue = 0;
        lastCCValues.clear();
        bitrateBps = 0;
        totalBytes = 0;
    }
};

struct TableStatus {
    bool detected = false;
    int64_t lastDetectedTime = 0;
    uint8_t version = 0;
    // Ajoutez d'autres champs si nécessaire
};

// Définir la structure TableInfo
struct TableInfo {
    bool detected = false;
    int64_t lastDetectedTime = 0;
    uint8_t version = 0;
    // Ajoutez d'autres champs si nécessaire
};


// Définition de la classe MyTableHandler
class MyTableHandler : public ts::TableHandlerInterface
{
public:
    // Modifié pour correspondre au type réel de votre variable tables
    MyTableHandler(std::map<uint16_t, TableInfo>& tbls) : tables(tbls) {}
    
    virtual void handleTable(ts::SectionDemux& demux, const ts::BinaryTable& table) override {
        auto it = tables.find(table.sourcePID());
        if (it != tables.end()) {
            it->second.detected = true;
            it->second.lastDetectedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            it->second.version = table.version();
        }
    }
    
private:
    std::map<uint16_t, TableInfo>& tables;
};

/**
 * @class TSQualityMonitor
 * @brief Classe pour surveiller la qualité d'un flux MPEG-TS
 */
class TSQualityMonitor {
public:
    /**
     * @brief Constructeur
     */
    TSQualityMonitor();
    
    /**
     * @brief Réinitialise les statistiques
     */
    void reset();
    
    /**
     * @brief Analyse un segment de données MPEG-TS
     * @param tsData Données MPEG-TS à analyser
     * @return Statistiques mises à jour
     */
    TSQualityStats analyze(const std::vector<uint8_t>& tsData);
    
    /**
     * @brief Récupère les statistiques actuelles
     * @return Statistiques actuelles
     */
    const TSQualityStats& getStats() const;
    
    /**
     * @brief Vérifie si le flux est conforme aux spécifications DVB
     * @param detailedLog Si true, génère des logs détaillés des problèmes
     * @return true si le flux est conforme aux spécifications DVB
     */
    bool isDVBCompliant(bool detailedLog = false) const;
    
private:
    TSQualityStats stats_;
    std::unique_ptr<ts::PCRAnalyzer> pcrAnalyzer_;
    std::map<uint16_t, uint8_t> expectedCC_; // PID -> CC attendu
    
    // Horodatage de la dernière analyse
    std::chrono::steady_clock::time_point lastAnalysisTime_;
    
    // Taille du dernier segment analysé
    size_t lastSegmentSize_;
    
    // Vérifier la présence et la validité des tables PSI/SI
    bool checkPSITables(const ts::TSPacketVector& packets, bool detailedLog);
    
    // Vérifier la fréquence de répétition des tables PSI/SI
    bool checkTableRepetitionRates(bool detailedLog) const;
};

} // namespace hls_to_dvb