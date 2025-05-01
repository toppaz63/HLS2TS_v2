#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <chrono>
#include <atomic>
#include <string>

namespace hls_to_dvb {

/**
 * @brief Structure représentant un segment MPEG-TS traité
 */
struct MPEGTSSegment {
    std::vector<uint8_t> data;           ///< Données du segment MPEG-TS
    bool hasDiscontinuity;               ///< Indique si le segment contient une discontinuité
    uint64_t sequenceNumber;             ///< Numéro de séquence du segment
    std::chrono::milliseconds duration;  ///< Durée du segment
    
    MPEGTSSegment() : hasDiscontinuity(false), sequenceNumber(0), duration(0) {}
    
    MPEGTSSegment(const std::vector<uint8_t>& _data, bool _hasDiscontinuity, 
                  uint64_t _sequenceNumber, std::chrono::milliseconds _duration) 
        : data(_data), hasDiscontinuity(_hasDiscontinuity), 
          sequenceNumber(_sequenceNumber), duration(_duration) {}
};

/**
 * @brief Classe gérant un buffer de segments pour retarder le flux de sortie
 * 
 * Cette classe permet de mettre en buffer un nombre configurable de segments avant
 * de les envoyer vers la sortie, ce qui permet de gérer les interruptions réseau
 * ou du flux HLS source.
 */
class SegmentBuffer {
public:
    /**
     * @brief Constructeur
     * @param bufferSize Taille du buffer en nombre de segments
     * @param name Nom du buffer pour l'identification dans les logs
     */
    SegmentBuffer(size_t bufferSize, const std::string& name = "default");
    
    /**
     * @brief Ajoute un segment au buffer
     * @param segment Segment à ajouter
     * @return true si le segment a été ajouté, false si le buffer est plein
     */
    bool pushSegment(const MPEGTSSegment& segment);
    
    /**
     * @brief Récupère le prochain segment disponible
     * @param segment Référence vers où stocker le segment récupéré
     * @param waitTimeoutMs Temps d'attente maximum en millisecondes (0 = non bloquant)
     * @return true si un segment a été récupéré, false si timeout ou buffer vide
     */
    bool getSegment(MPEGTSSegment& segment, uint32_t waitTimeoutMs = 0);
    
    /**
     * @brief Récupère la taille configurée du buffer
     * @return Taille du buffer en nombre de segments
     */
    size_t getBufferSize() const;
    
    /**
     * @brief Modifie la taille du buffer
     * @param newSize Nouvelle taille du buffer en nombre de segments
     */
    void setBufferSize(size_t newSize);
    
    /**
     * @brief Récupère le nombre actuel de segments dans le buffer
     * @return Nombre de segments actuellement dans le buffer
     */
    size_t getCurrentSize() const;
    
    /**
     * @brief Vide le buffer
     */
    void clear();
    
    /**
     * @brief Indique si le buffer est actuellement en sous-charge
     * @param thresholdPercent Pourcentage de remplissage minimal souhaité (0-100)
     * @return true si le buffer est en sous-charge, false sinon
     */
    bool isUnderflow(float thresholdPercent = 30.0f) const;
    
    /**
     * @brief Récupère le nom du buffer
     * @return Nom du buffer
     */
    std::string getName() const;
    
private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<MPEGTSSegment> segments_;
    std::atomic<size_t> bufferSize_;
    std::string name_;
    
    // Statistiques
    std::chrono::steady_clock::time_point lastGetTime_;
    std::chrono::steady_clock::time_point lastPushTime_;
};

} // namespace hls_to_dvb
