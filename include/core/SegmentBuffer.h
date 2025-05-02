#pragma once

#include "../mpegts/MPEGTSConverter.h"

#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

/**
 * @class SegmentBuffer
 * @brief Buffer circulaire pour les segments MPEG-TS
 * 
 * Gère un tampon de segments MPEG-TS avec synchronisation pour l'accès concurrent.
 * Permet de compenser les variations de latence du réseau et d'assurer une lecture fluide.
 */
class SegmentBuffer {
public:
    /**
     * @brief Constructeur
     * @param bufferSize Taille maximale du buffer (nombre de segments)
     */
    explicit SegmentBuffer(size_t bufferSize = 3);
    
    /**
     * @brief Ajoute un segment au buffer
     * @param segment Segment à ajouter
     * @return true si le segment a été ajouté avec succès
     */
    bool pushSegment(const MPEGTSSegment& segment);
    
    /**
     * @brief Récupère le segment suivant du buffer
     * @param segment Référence pour stocker le segment récupéré
     * @param timeout Durée maximale d'attente en millisecondes (0 = pas d'attente)
     * @return true si un segment a été récupéré
     */
    bool getSegment(MPEGTSSegment& segment, int timeout = 0);
    
    /**
     * @brief Définit la taille maximale du buffer
     * @param bufferSize Nouvelle taille du buffer
     */
    void setBufferSize(size_t bufferSize);
    
    /**
     * @brief Récupère la taille maximale du buffer
     * @return Taille maximale du buffer
     */
    size_t getBufferSize() const;
    
    /**
     * @brief Récupère le nombre actuel de segments dans le buffer
     * @return Nombre de segments dans le buffer
     */
    size_t getCurrentSize() const;
    
    /**
     * @brief Vide le buffer
     */
    void clear();
    
private:
    std::deque<MPEGTSSegment> buffer_;         ///< Buffer de segments
    std::atomic<size_t> bufferSize_;            ///< Taille maximale du buffer
    mutable std::mutex mutex_;                   ///< Mutex pour l'accès concurrent
    std::condition_variable conditionVar_;      ///< Variable de condition pour l'attente
};
