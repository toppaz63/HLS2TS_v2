#pragma once

#include "../hls/HLSClient.h"

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <atomic>
#include <mutex>
#include <map>

// Forward declarations
namespace ts {
    class TSPacket;
    class TSProcessor;
    typedef std::vector<TSPacket> TSPacketVector;
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
     * @brief Traite les paquets MPEG-TS pour assurer la conformité DVB
     * @param packets Paquets à traiter
     * @param discontinuity Indique s'il y a une discontinuité
     */
    void processPackets(ts::TSPacketVector& packets, bool discontinuity);
    
    /**
     * @brief Réinitialise les compteurs de continuité
     */
    void resetContinuityCounters();
    
    std::map<uint16_t, uint8_t> continuityCounters_; ///< Compteurs de continuité par PID
    uint64_t lastPcrValue_;                        ///< Dernière valeur PCR traitée
    uint16_t pcrPid_;                              ///< PID principal des PCR
};