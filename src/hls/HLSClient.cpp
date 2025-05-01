#include "HLSClient.h"
#include "../alerting/AlertManager.h"
#include "spdlog/spdlog.h"

#include <fstream>
#include <sstream>
#include <regex>
#include <chrono>
#include <algorithm>

// Initialisation globale de FFmpeg (une seule fois)
static struct FFmpegInit {
    FFmpegInit() {
        av_log_set_level(AV_LOG_WARNING);
        avformat_network_init();
    }
    ~FFmpegInit() {
        avformat_network_deinit();
    }
} ffmpegInit;

HLSClient::HLSClient(const std::string& url)
    : url_(url), formatContext_(nullptr), running_(false),
      segmentsProcessed_(0), discontinuitiesDetected_(0) {
    
    // Initialiser les informations du flux
    streamInfo_.url = url;
    streamInfo_.bandwidth = 0;
    streamInfo_.codecs = "";
    streamInfo_.width = 0;
    streamInfo_.height = 0;
    streamInfo_.hasMPEGTSSegments = false;
}

void HLSClient::start() {
    if (running_) {
        spdlog::warn("Le client HLS est déjà en cours d'exécution");
        return;
    }
    
    spdlog::info("Démarrage du client HLS pour l'URL: {}", url_);
    
    try {
        // Ouverture du flux pour analyse des variantes
        AVFormatContext* ctx = nullptr;
        
        AVDictionary* options = nullptr;
        av_dict_set(&options, "http_persistent", "0", 0);  // Désactiver les connexions persistantes
        av_dict_set(&options, "timeout", "10000000", 0);   // Timeout en microsecondes (10s)
        
        int ret = avformat_open_input(&ctx, url_.c_str(), nullptr, &options);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("Erreur lors de l'ouverture du flux HLS: ") + errbuf);
        }
        
        // Récupérer les informations sur le flux
        ret = avformat_find_stream_info(ctx, nullptr);
        if (ret < 0) {
            avformat_close_input(&ctx);
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("Erreur lors de la récupération des informations sur le flux: ") + errbuf);
        }
        
        // Sélectionner le flux avec le plus grand débit
        bool validStream = selectHighestBitrateStream();
        
        // Libérer le contexte d'analyse
        avformat_close_input(&ctx);
        
        // Vérifier si le flux contient des segments MPEG-TS
        if (!validStream) {
            throw std::runtime_error("Aucun flux valide trouvé dans la playlist HLS");
        }
        
        if (!streamInfo_.hasMPEGTSSegments) {
            std::string errorMsg = "Le flux HLS ne contient pas de segments MPEG-TS. "
                                  "Cette application ne peut traiter que les flux HLS avec des segments MPEG-TS.";
            
            spdlog::error(errorMsg);
            
            AlertManager::getInstance().addAlert(
                AlertLevel::ERROR,
                "HLSClient",
                errorMsg,
                true
            );
            
            throw std::runtime_error(errorMsg);
        }
        
        // Ouvrir le flux pour le traitement des segments
        formatContext_ = avformat_alloc_context();
        if (!formatContext_) {
            throw std::runtime_error("Impossible d'allouer le contexte de format AVFormat");
        }
        
        // Configurer les options pour le client HLS
        options = nullptr;
        av_dict_set(&options, "http_persistent", "0", 0);  // Désactiver les connexions persistantes
        av_dict_set(&options, "timeout", "10000000", 0);   // Timeout en microsecondes (10s)
        
        // Utiliser l'URL du flux de plus haut débit
        ret = avformat_open_input(&formatContext_, streamInfo_.url.c_str(), nullptr, &options);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("Erreur lors de l'ouverture du flux HLS: ") + errbuf);
        }
        
        // Récupérer les informations sur le flux
        ret = avformat_find_stream_info(formatContext_, nullptr);
        if (ret < 0) {
            avformat_close_input(&formatContext_);
            formatContext_ = nullptr;
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("Erreur lors de la récupération des informations sur le flux: ") + errbuf);
        }
        
        // Vérifier les discontinuités
        checkForDiscontinuities(streamInfo_.url);
        
        // Démarrer le thread de récupération des segments
        running_ = true;
        fetchThread_ = std::thread(&HLSClient::fetchThreadFunc, this);
        
        spdlog::info("Client HLS démarré avec succès. Flux sélectionné: {}x{}, {}kbps, codecs: {}",
                   streamInfo_.width, streamInfo_.height, streamInfo_.bandwidth / 1000, streamInfo_.codecs);
        
        AlertManager::getInstance().addAlert(
            AlertLevel::INFO,
            "HLSClient",
            "Client HLS démarré. Flux sélectionné: " + std::to_string(streamInfo_.width) + "x" + 
            std::to_string(streamInfo_.height) + ", " + std::to_string(streamInfo_.bandwidth / 1000) + 
            "kbps, codecs: " + streamInfo_.codecs,
            false
        );
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors du démarrage du client HLS: {}", e.what());
        
        if (formatContext_) {
            avformat_close_input(&formatContext_);
            formatContext_ = nullptr;
        }
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "HLSClient",
            std::string("Erreur lors du démarrage du client HLS: ") + e.what(),
            true
        );
        
        throw;
    }
}

bool HLSClient::selectHighestBitrateStream() {
    try {
        // Ouvrir le flux HLS principal
        AVFormatContext* ctx = nullptr;
        
        AVDictionary* options = nullptr;
        av_dict_set(&options, "http_persistent", "0", 0);
        av_dict_set(&options, "timeout", "10000000", 0);
        
        int ret = avformat_open_input(&ctx, url_.c_str(), nullptr, &options);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("Erreur lors de l'ouverture du flux HLS: ") + errbuf);
        }
        
        // Vérifier si c'est un flux HLS avec des variantes
        if (ctx->nb_streams == 0) {
            // C'est probablement une playlist principale avec des variantes
            
            // Structure pour stocker les informations sur les variantes
            struct VariantInfo {
                std::string url;
                int bandwidth;
                std::string codecs;
                int width;
                int height;
                bool hasMPEGTSSegments;
            };
            
            std::vector<VariantInfo> variants;
            
            // Lire la playlist principale pour extraire les variantes
            uint8_t* buffer = nullptr;
            size_t buffer_size = 0;
            
            AVIOContext* ioCtx = ctx->pb;
            if (ioCtx) {
                // Sauvegarder la position actuelle
                int64_t pos = avio_tell(ioCtx);
                avio_seek(ioCtx, 0, SEEK_SET);
                
                // Lire tout le fichier
                buffer_size = avio_size(ioCtx);
                buffer = (uint8_t*)av_malloc(buffer_size + 1);
                if (buffer) {
                    int bytesRead = avio_read(ioCtx, buffer, buffer_size);
                    if (bytesRead > 0) {
                        buffer[bytesRead] = 0;  // Null-terminate
                        
                        // Analyser la playlist HLS
                        std::istringstream iss(reinterpret_cast<char*>(buffer));
                        std::string line;
                        VariantInfo currentVariant;
                        bool inStreamInfo = false;
                        
                        while (std::getline(iss, line)) {
                            // Supprimer les espaces et les retours à la ligne
                            line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
                                return c == '\r' || c == '\n'; 
                            }), line.end());
                            
                            if (line.empty()) continue;
                            
                            // Analyser les lignes EXT-X-STREAM-INF
                            if (line.find("#EXT-X-STREAM-INF:") != std::string::npos) {
                                inStreamInfo = true;
                                currentVariant = VariantInfo();
                                
                                // Extraire la bande passante
                                std::regex bandwidth_regex("BANDWIDTH=(\\d+)");
                                std::smatch bandwidth_match;
                                if (std::regex_search(line, bandwidth_match, bandwidth_regex)) {
                                    currentVariant.bandwidth = std::stoi(bandwidth_match[1]);
                                }
                                
                                // Extraire les codecs
                                std::regex codecs_regex("CODECS=\"([^\"]*)\"");
                                std::smatch codecs_match;
                                if (std::regex_search(line, codecs_match, codecs_regex)) {
                                    currentVariant.codecs = codecs_match[1];
                                }
                                
                                // Extraire la résolution
                                std::regex resolution_regex("RESOLUTION=(\\d+)x(\\d+)");
                                std::smatch resolution_match;
                                if (std::regex_search(line, resolution_match, resolution_regex)) {
                                    currentVariant.width = std::stoi(resolution_match[1]);
                                    currentVariant.height = std::stoi(resolution_match[2]);
                                }
                            }
                            else if (inStreamInfo && !line.empty() && line[0] != '#') {
                                // C'est l'URL de la variante
                                inStreamInfo = false;
                                
                                // Résoudre l'URL relative si nécessaire
                                if (line.find("http") == 0) {
                                    currentVariant.url = line;
                                } else {
                                    // Extraire le chemin de base de l'URL principale
                                    std::string baseUrl = url_;
                                    size_t lastSlash = baseUrl.find_last_of('/');
                                    if (lastSlash != std::string::npos) {
                                        baseUrl = baseUrl.substr(0, lastSlash + 1);
                                    }
                                    currentVariant.url = baseUrl + line;
                                }
                                
                                // Vérifier si la variante contient des segments MPEG-TS
                                AVFormatContext* variantCtx = nullptr;
                                ret = avformat_open_input(&variantCtx, currentVariant.url.c_str(), nullptr, &options);
                                if (ret >= 0) {
                                    currentVariant.hasMPEGTSSegments = hasMPEGTSSegments(variantCtx);
                                    avformat_close_input(&variantCtx);
                                }
                                
                                variants.push_back(currentVariant);
                            }
                        }
                    }
                    av_free(buffer);
                }
                
                // Restaurer la position
                avio_seek(ioCtx, pos, SEEK_SET);
            }
            
            // Fermer le contexte
            avformat_close_input(&ctx);
            
            // Sélectionner la variante avec le plus grand débit parmi celles qui ont des segments MPEG-TS
            std::vector<VariantInfo> validVariants;
            for (const auto& v : variants) {
                if (v.hasMPEGTSSegments) {
                    validVariants.push_back(v);
                }
            }
            
            if (validVariants.empty()) {
                spdlog::error("Aucune variante avec segments MPEG-TS trouvée dans le flux HLS");
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::ERROR,
                    "HLSClient",
                    "Aucune variante avec segments MPEG-TS trouvée dans le flux HLS. "
                    "Cette application ne peut traiter que les flux HLS avec des segments MPEG-TS.",
                    true
                );
                
                return false;
            }
            
            // Trier par bande passante décroissante
            std::sort(validVariants.begin(), validVariants.end(), 
                     [](const VariantInfo& a, const VariantInfo& b) {
                         return a.bandwidth > b.bandwidth;
                     });
            
            // Sélectionner la variante avec le plus grand débit
            const auto& bestVariant = validVariants[0];
            
            streamInfo_.url = bestVariant.url;
            streamInfo_.bandwidth = bestVariant.bandwidth;
            streamInfo_.codecs = bestVariant.codecs;
            streamInfo_.width = bestVariant.width;
            streamInfo_.height = bestVariant.height;
            streamInfo_.hasMPEGTSSegments = bestVariant.hasMPEGTSSegments;
            
            spdlog::info("Sélection de la variante avec le plus grand débit: {}x{}, {}kbps, codecs: {}",
                       bestVariant.width, bestVariant.height, bestVariant.bandwidth / 1000, bestVariant.codecs);
            
            return true;
        }
        else {
            // C'est un flux HLS sans variantes
            streamInfo_.url = url_;
            streamInfo_.hasMPEGTSSegments = hasMPEGTSSegments(ctx);
            
            // Extraire les informations de codecs et résolution
            for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
                AVStream* stream = ctx->streams[i];
                
                if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    streamInfo_.width = stream->codecpar->width;
                    streamInfo_.height = stream->codecpar->height;
                    
                    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
                    if (codec) {
                        streamInfo_.codecs = codec->name;
                    }
                    
                    break;
                }
            }
            
            avformat_close_input(&ctx);
            
            if (!streamInfo_.hasMPEGTSSegments) {
                spdlog::error("Le flux HLS ne contient pas de segments MPEG-TS");
                return false;
            }
            
            return true;
        }
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de la sélection du flux HLS: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "HLSClient",
            std::string("Erreur lors de la sélection du flux HLS: ") + e.what(),
            true
        );
        
        return false;
    }
}

bool HLSClient::hasMPEGTSSegments(AVFormatContext* formatContext) {
    if (!formatContext) return false;
    
    // Vérifier si le format est HLS
    if (formatContext->iformat && 
        (strcmp(formatContext->iformat->name, "hls") == 0 || 
         strcmp(formatContext->iformat->name, "applehttp") == 0)) {
        
        // Essayer de déterminer si les segments sont en MPEG-TS
        
        // Méthode 1: Vérifier le nom du format du premier segment
        // Cela nécessite d'ouvrir un segment
        // Pour simplifier, on vérifie si le codec_id des flux est compatible avec MPEG-TS
        
        bool hasCompatibleStreams = false;
        
        for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
            AVStream* stream = formatContext->streams[i];
            
            // MPEG-TS supporte généralement ces types de codecs
            if (stream->codecpar->codec_id == AV_CODEC_ID_H264 ||
                stream->codecpar->codec_id == AV_CODEC_ID_HEVC ||
                stream->codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
                stream->codecpar->codec_id == AV_CODEC_ID_AAC ||
                stream->codecpar->codec_id == AV_CODEC_ID_MP3 ||
                stream->codecpar->codec_id == AV_CODEC_ID_AC3) {
                
                hasCompatibleStreams = true;
                break;
            }
        }
        
        // Méthode 2: Vérifier les métadonnées
        AVDictionaryEntry* tag = av_dict_get(formatContext->metadata, "variant_bitrate", NULL, 0);
        bool hasHLSMetadata = (tag != NULL);
        
        // Si nous avons des flux compatibles et des métadonnées HLS, supposer que c'est du MPEG-TS
        // Cette heuristique n'est pas parfaite, mais c'est un bon indicateur
        return hasCompatibleStreams && hasHLSMetadata;
    }
    
    return false;
}

void HLSClient::stop() {
    if (!running_) {
        spdlog::warn("Le client HLS n'est pas en cours d'exécution");
        return;
    }
    
    spdlog::info("Arrêt du client HLS");
    
    // Arrêter le thread de récupération
    running_ = false;
    queueCondVar_.notify_all();
    
    if (fetchThread_.joinable()) {
        fetchThread_.join();
    }
    
    // Fermer le flux FFmpeg
    if (formatContext_) {
        avformat_close_input(&formatContext_);
        formatContext_ = nullptr;
    }
    
    // Vider la file d'attente des segments
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!segmentQueue_.empty()) {
            segmentQueue_.pop();
        }
    }
    
    spdlog::info("Client HLS arrêté");
    
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "HLSClient",
        "Client HLS arrêté",
        false
    );
}

void HLSClient::fetchThreadFunc() {
    try {
        spdlog::info("Thread de récupération des segments HLS démarré");
        
        if (!formatContext_) {
            throw std::runtime_error("Contexte de format FFmpeg non initialisé");
        }
        
        // Variables pour la gestion des discontinuités
        bool previousWasDiscontinuity = false;
        int sequenceNumber = 0;
        
        // Boucle principale de récupération des segments
        while (running_) {
            // Lire un paquet
            AVPacket packet;
            av_init_packet(&packet);
            packet.data = nullptr;
            packet.size = 0;
            
            int ret = av_read_frame(formatContext_, &packet);
            
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    // Fin du flux, attendre un peu et réessayer
                    av_packet_unref(&packet);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                else {
                    // Erreur de lecture
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    spdlog::error("Erreur lors de la lecture du flux HLS: {}", errbuf);
                    
                    AlertManager::getInstance().addAlert(
                        AlertLevel::ERROR,
                        "HLSClient",
                        std::string("Erreur lors de la lecture du flux HLS: ") + errbuf,
                        true
                    );
                    
                    av_packet_unref(&packet);
                    break;
                }
            }
            
            // Vérifier si c'est un nouveau segment
            if (packet.flags & AV_PKT_FLAG_KEY && packet.pos == 0) {
                // C'est probablement le début d'un nouveau segment
                
                // Accumuler les données du segment
                std::vector<uint8_t> segmentData;
                bool isDiscontinuity = previousWasDiscontinuity;
                previousWasDiscontinuity = false;
                
                // Ajouter le paquet actuel
                segmentData.insert(segmentData.end(), packet.data, packet.data + packet.size);
                
                // Lire les paquets suivants jusqu'au prochain segment
                bool endOfSegment = false;
                
                while (!endOfSegment && running_) {
                    AVPacket nextPacket;
                    av_init_packet(&nextPacket);
                    nextPacket.data = nullptr;
                    nextPacket.size = 0;
                    
                    ret = av_read_frame(formatContext_, &nextPacket);
                    
                    if (ret < 0) {
                        // Fin du flux ou erreur
                        if (ret == AVERROR_EOF) {
                            // Dernier segment
                            endOfSegment = true;
                        }
                        else {
                            // Erreur de lecture
                            char errbuf[AV_ERROR_MAX_STRING_SIZE];
                            av_strerror(ret, errbuf, sizeof(errbuf));
                            spdlog::error("Erreur lors de la lecture du flux HLS: {}", errbuf);
                            
                            AlertManager::getInstance().addAlert(
                                AlertLevel::ERROR,
                                "HLSClient",
                                std::string("Erreur lors de la lecture du flux HLS: ") + errbuf,
                                true
                            );
                            
                            endOfSegment = true;
                        }
                    }
                    else if (nextPacket.flags & AV_PKT_FLAG_KEY && nextPacket.pos == 0) {
                        // Début d'un nouveau segment
                        endOfSegment = true;
                        
                        // Vérifier si c'est une discontinuité
                        if (nextPacket.flags & AV_PKT_FLAG_DISCARD) {
                            previousWasDiscontinuity = true;
                        }
                        
                        // Remettre ce paquet dans le flux
                        av_seek_frame(formatContext_, -1, nextPacket.pts, AVSEEK_FLAG_BACKWARD);
                    }
                    else {
                        // Ajouter le paquet au segment
                        segmentData.insert(segmentData.end(), nextPacket.data, nextPacket.data + nextPacket.size);
                    }
                    
                    av_packet_unref(&nextPacket);
                }
                
                // Créer un segment HLS
                if (!segmentData.empty()) {
                    HLSSegment segment;
                    segment.data = std::move(segmentData);
                    segment.discontinuity = isDiscontinuity;
                    segment.sequenceNumber = sequenceNumber++;
                    segment.duration = 0.0;  // À déterminer
                    segment.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                    
                    // Ajouter le segment à la file d'attente
                    {
                        std::lock_guard<std::mutex> lock(queueMutex_);
                        segmentQueue_.push(segment);
                    }
                    
                    // Notifier les threads en attente
                    queueCondVar_.notify_one();
                    
                    // Mettre à jour les compteurs
                    segmentsProcessed_++;
                    
                    if (isDiscontinuity) {
                        discontinuitiesDetected_++;
                        spdlog::info("Discontinuité détectée dans le segment {}", segment.sequenceNumber);
                    }

 spdlog::debug("Segment {} récupéré, taille: {} octets", segment.sequenceNumber, segment.data.size());
                }
            }
            
            av_packet_unref(&packet);
        }
    }
    catch (const std::exception& e) {
        spdlog::error("Exception dans le thread de récupération HLS: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "HLSClient",
            std::string("Exception dans le thread de récupération HLS: ") + e.what(),
            true
        );
    }
    
    spdlog::info("Thread de récupération des segments HLS terminé");
}

std::optional<HLSSegment> HLSClient::getNextSegment() {
    std::unique_lock<std::mutex> lock(queueMutex_);
    
    if (segmentQueue_.empty()) {
        return std::nullopt;
    }
    
    // Récupérer le segment du début de la file
    HLSSegment segment = segmentQueue_.front();
    segmentQueue_.pop();
    
    return segment;
}

bool HLSClient::checkForDiscontinuities(const std::string& url) {
    try {
        // Tentative d'ouverture de la playlist pour analyse
        std::string playlistContent;
        
        // Utiliser FFmpeg pour récupérer la playlist
        AVFormatContext* tempCtx = nullptr;
        AVIOContext* ioCtx = nullptr;
        
        int ret = avformat_open_input(&tempCtx, url.c_str(), nullptr, nullptr);
        if (ret < 0) {
            return false;  // Impossible d'ouvrir la playlist
        }
        
        // Obtenir le contexte I/O
        ioCtx = tempCtx->pb;
        if (!ioCtx) {
            avformat_close_input(&tempCtx);
            return false;
        }
        
        // Sauvegarder la position actuelle
        int64_t pos = avio_tell(ioCtx);
        avio_seek(ioCtx, 0, SEEK_SET);
        
        // Lire la playlist
        size_t size = avio_size(ioCtx);
        if (size > 0) {
            std::vector<char> buffer(size + 1);
            int bytesRead = avio_read(ioCtx, (unsigned char*)buffer.data(), size);
            if (bytesRead > 0) {
                buffer[bytesRead] = 0;  // Null-terminate
                playlistContent = buffer.data();
            }
        }
        
        // Restaurer la position
        avio_seek(ioCtx, pos, SEEK_SET);
        
        // Fermer le contexte temporaire
        avformat_close_input(&tempCtx);
        
        // Analyser la playlist pour trouver les marqueurs de discontinuité
        std::istringstream iss(playlistContent);
        std::string line;
        bool foundDiscontinuity = false;
        
        while (std::getline(iss, line)) {
            // Vérifier s'il y a un marqueur de discontinuité
            if (line.find("#EXT-X-DISCONTINUITY") != std::string::npos) {
                foundDiscontinuity = true;
                spdlog::info("Marqueur de discontinuité trouvé dans la playlist HLS");
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::INFO,
                    "HLSClient",
                    "Marqueur de discontinuité trouvé dans la playlist HLS",
                    false
                );
                
                break;
            }
        }
        
        return foundDiscontinuity;
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de la vérification des discontinuités: {}", e.what());
        return false;
    }
}

size_t HLSClient::getSegmentsProcessed() const {
    return segmentsProcessed_;
}

size_t HLSClient::getDiscontinuitiesDetected() const {
    return discontinuitiesDetected_;
}

HLSStreamInfo HLSClient::getStreamInfo() const {
    return streamInfo_;
}

bool HLSClient::isValidStream() const {
    return streamInfo_.hasMPEGTSSegments;
}

HLSClient::~HLSClient() {
    if (running_) {
        stop();
    }
}