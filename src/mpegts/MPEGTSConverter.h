#pragma once

#include "../hls/HLSClient.h"

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <atomic>
#include <mutex>

// Forward declarations
namespace ts {
    class TSPacket;
    class TSProcessor;
}

// Forward declaration pour DVBProcessor
class DVBProcessor;

/**
 * @struct MPEGTSSegment
 * @brief Représente un segment MPEG-TS converti
 */
struct MPEGTSSegment {
    std::vector<uint8_t> data;      ///< Données du segment MPEG-TS
    bool discontinuity;             ///< Indique si le segment marque une discontinuité
    int sequenceNumber;             ///< Numéro de séquence du segment
    double duration;                ///< Durée du segment en secondes
    int64_t timestamp;              ///< Horodatage du segment
};

/**
 * @class MPEGTSConverter
 * @brief Convertit les segments HLS en segments MPEG-TS conformes à DVB
 * 
 * Utilise TSDuck pour traiter les segments MPEG-TS et assurer leur conformité
 * avec la norme DVB, notamment pour les tables PSI/SI.
 */
class MPEGTSConverter {
public:
    /**
     * @brief Constructeur
     */
    MPEGTSConverter();
    
    /**
     * @brief Démarre le convertisseur
     */
    void start();
    
    /**
     * @brief Arrête le convertisseur
     */
    void stop();
    
    /**
     * @brief Convertit un segment HLS en segment MPEG-TS
     * @param hlsSegment Segment HLS à convertir
     * @return Segment MPEG-TS ou nullopt en cas d'erreur
     */
    std::optional<MPEGTSSegment> convert(const HLSSegment& hlsSegment);
    
    /**
     * @brief Indique si le convertisseur est en cours d'exécution
     * @return true si le convertisseur est en cours d'exécution
     */
    bool isRunning() const;
    
    /**
     * @brief Destructeur
     */
    ~MPEGTSConverter();
    
private:
    std::unique_ptr<DVBProcessor> dvbProcessor_; ///< Processeur DVB pour les tables PSI/SI
    std::atomic<bool> running_;                 ///< Indique si le convertisseur est en cours d'exécution
    mutable std::mutex mutex_;                  ///< Mutex pour les accès concurrents
    
    /**
     * @brief Traite les discontinuités dans le flux MPEG-TS
     * @param data Données MPEG-TS à traiter
     * @param discontinuity Indique si une discontinuité est présente
     * @return Données MPEG-TS traitées
     */
    std::vector<uint8_t> processDiscontinuity(const std::vector<uint8_t>& data, bool discontinuity);
    
    /**
     * @brief Met à jour les tables PSI/SI dans le flux MPEG-TS
     * @param data Données MPEG-TS à mettre à jour
     * @return Données MPEG-TS avec tables mises à jour
     */
    std::vector<uint8_t> updatePSITables(const std::vector<uint8_t>& data);
};