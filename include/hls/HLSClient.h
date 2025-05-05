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
#include <regex>

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

    /**
     * @brief Résout une URL relative par rapport à une URL de base
     * @param baseUrl URL de base
     * @param relativeUrl URL relative
     * @return URL absolue résolue
     */
    std::string resolveRelativeUrl(const std::string& baseUrl, const std::string& relativeUrl);
    
    /**
     * @brief Vérifie le format d'un segment HLS
     * @param segmentUrl URL du segment à vérifier
     * @return true si le segment est en format MPEG-TS
     */
    bool checkSegmentFormat(const std::string& segmentUrl);

    /**
    * @brief Force l'acceptation d'un flux HLS même si les segments MPEG-TS ne sont pas détectés
    * @return true si le flux peut être traité
    */
    bool forceAcceptHLSStream();

    /**
     * @brief Affiche des informations détaillées sur une playlist HLS
     * @param url URL de la playlist à analyser
     */
    void dumpPlaylistInfo(const std::string& url);

    /**
    * @brief Extrait la valeur d'un attribut dans une ligne de manifeste HLS
    * @param line Ligne du manifeste contenant l'attribut
    * @param attributeName Nom de l'attribut à extraire
    * @return Valeur de l'attribut ou chaîne vide si non trouvé
    */
    std::string extractAttributeValue(const std::string& line, const std::string& attributeName);

    /**
    * @brief Détermine si une playlist est une master playlist
    * @param content Contenu de la playlist
    * @return true si c'est une master playlist, false sinon
    */
    bool isMasterPlaylist(const std::string& content);

    /**
    * @brief Vérifie si FFmpeg supporte les protocoles SSL/TLS
    */
    void checkFFmpegSSLSupport();

    /**
    * @brief Télécharge un manifeste HLS en utilisant curl comme alternative
    * @param url URL du manifeste HLS
    * @param content Référence pour stocker le contenu récupéré
    * @return true si réussi, false sinon
    */
    bool fetchHLSManifestWithCurl(const std::string& url, std::string& content);

    /**
    * @brief Crée un dictionnaire d'options FFmpeg avec les paramètres appropriés
    * @param longTimeout Utiliser un timeout plus long si true
    * @return Dictionnaire d'options FFmpeg
    */
    AVDictionary* createFFmpegOptions(bool longTimeout = false);
};