#pragma once

#include <string>
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <optional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

/**
 * @struct HLSStreamInfo
 * @brief Informations sur un flux HLS
 */
struct HLSStreamInfo {
    std::string url;            ///< URL du flux
    int bandwidth;              ///< Bande passante en bits/s
    std::string codecs;         ///< Codecs utilisés
    int width;                  ///< Largeur de la vidéo
    int height;                 ///< Hauteur de la vidéo
    bool hasMPEGTSSegments;     ///< Indique si le flux contient des segments MPEG-TS
};

/**
 * @struct HLSSegment
 * @brief Représente un segment HLS récupéré et ses métadonnées
 */
struct HLSSegment {
    std::vector<uint8_t> data;  ///< Données brutes du segment
    bool discontinuity;         ///< Indique si le segment marque une discontinuité
    int sequenceNumber;         ///< Numéro de séquence du segment
    double duration;            ///< Durée du segment en secondes
    int64_t timestamp;          ///< Horodatage du segment
};

/**
 * @class HLSClient
 * @brief Client HLS pour récupérer et analyser les flux HLS
 * 
 * Utilise FFmpeg pour récupérer les segments HLS et détecter les discontinuités.
 */
class HLSClient {
public:
    /**
     * @brief Constructeur
     * @param url URL du flux HLS à récupérer
     */
    explicit HLSClient(const std::string& url);
    
    /**
     * @brief Démarre le client HLS
     */
    void start();
    
    /**
     * @brief Arrête le client HLS
     */
    void stop();
    
    /**
     * @brief Récupère le prochain segment disponible
     * @return Segment HLS ou nullopt si aucun segment disponible
     */
    std::optional<HLSSegment> getNextSegment();
    
    /**
     * @brief Récupère le nombre de segments traités
     */
    size_t getSegmentsProcessed() const;
    
    /**
     * @brief Récupère le nombre de discontinuités détectées
     */
    size_t getDiscontinuitiesDetected() const;
    
    /**
     * @brief Récupère les informations sur le flux sélectionné
     * @return Informations sur le flux HLS
     */
    HLSStreamInfo getStreamInfo() const;
    
    /**
     * @brief Vérifie si le flux est valide (contient des segments MPEG-TS)
     * @return true si le flux est valide
     */
    bool isValidStream() const;
    
    /**
     * @brief Destructeur
     */
    ~HLSClient();
    
private:
    std::string url_;                    ///< URL du flux HLS
    AVFormatContext* formatContext_;     ///< Contexte FFmpeg pour le format
    HLSStreamInfo streamInfo_;           ///< Informations sur le flux sélectionné
    
    std::thread fetchThread_;            ///< Thread de récupération des segments
    std::atomic<bool> running_;          ///< Indique si le client est en cours d'exécution
    
    std::queue<HLSSegment> segmentQueue_; ///< File d'attente des segments récupérés
    std::mutex queueMutex_;              ///< Mutex pour l'accès à la file d'attente
    std::condition_variable queueCondVar_; ///< Variable de condition pour la synchronisation
    
    std::atomic<size_t> segmentsProcessed_;       ///< Compteur de segments traités
    std::atomic<size_t> discontinuitiesDetected_; ///< Compteur de discontinuités détectées
    
    /**
     * @brief Sélectionne le flux avec le plus grand débit
     * @return true si un flux valide a été sélectionné
     */
    bool selectHighestBitrateStream();
    
    /**
     * @brief Vérifie si le flux contient des segments MPEG-TS
     * @param formatContext Contexte du format
     * @return true si le flux contient des segments MPEG-TS
     */
    bool hasMPEGTSSegments(AVFormatContext* formatContext);
    
    /**
     * @brief Fonction principale du thread de récupération
     */
    void fetchThreadFunc();
    
    /**
     * @brief Analyse la playlist HLS pour détecter les discontinuités
     * @param url URL de la playlist à analyser
     * @return true si des discontinuités ont été détectées
     */
    bool checkForDiscontinuities(const std::string& url);
};
