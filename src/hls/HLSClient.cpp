#include "hls/HLSClient.h"
#include "hls/custom_formatters.h"
#include "alerting/AlertManager.h"
#include "spdlog/spdlog.h"

#include <fstream>
#include <sstream>
#include <regex>
#include <chrono>
#include <algorithm>

using namespace hls_to_dvb;

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


// Nouvelle méthode pour résoudre les URLs relatives
std::string HLSClient::resolveRelativeUrl(const std::string& baseUrl, const std::string& relativeUrl) {
    // Si l'URL est déjà absolue, pas besoin de résolution
    if (relativeUrl.find("http") == 0) {
        return relativeUrl;
    }
    
    // Si l'URL relative commence par /, nous devons extraire uniquement le domaine de base
    if (!relativeUrl.empty() && relativeUrl[0] == '/') {
        // Extraire le domaine de l'URL de base (http(s)://domain.com)
        std::string domain;
        std::regex domainRegex("(https?://[^/]+)");
        std::smatch match;
        if (std::regex_search(baseUrl, match, domainRegex)) {
            domain = match[1];
        } else {
            // Si nous ne pouvons pas extraire le domaine, utiliser l'URL de base complète
            domain = baseUrl;
        }
        
        return domain + relativeUrl;
    }
    
    // Sinon, extraire le chemin de base de l'URL principale
    std::string result = baseUrl;
    size_t lastSlash = result.find_last_of('/');
    if (lastSlash != std::string::npos) {
        result = result.substr(0, lastSlash + 1);
    }
    
    // Gérer les chemins relatifs comme "../"
    std::string resolvedUrl = result + relativeUrl;
    
    // Version simplifiée sans regex complexe
    // Remplacer les occurrences de "../" de manière itérative
    size_t pos = 0;
    while ((pos = resolvedUrl.find("/../")) != std::string::npos) {
        // Trouver le dernier '/' avant "/../"
        size_t prevSlash = resolvedUrl.rfind('/', pos - 1);
        if (prevSlash != std::string::npos) {
            resolvedUrl.erase(prevSlash, pos + 4 - prevSlash);
        } else {
            break;
        }
    }
    
    // Remplacer les occurrences de "./" 
    while ((pos = resolvedUrl.find("/./")) != std::string::npos) {
        resolvedUrl.replace(pos, 3, "/");
    }
    
    return resolvedUrl;
}
// Nouvelle méthode pour vérifier le format des segments
bool HLSClient::checkSegmentFormat(const std::string& segmentUrl) {
    AVFormatContext* segmentCtx = nullptr;
    bool isMpegTS = false;
    
    AVDictionary* options = createFFmpegOptions();
    
    int ret = avformat_open_input(&segmentCtx, segmentUrl.c_str(), nullptr, &options);
    if (ret >= 0) {
        ret = avformat_find_stream_info(segmentCtx, nullptr);
        if (ret >= 0) {
            // Vérifier si le format est MPEG-TS
            if (segmentCtx->iformat && 
                (strcmp(segmentCtx->iformat->name, "mpegts") == 0)) {
                isMpegTS = true;
                spdlog::info("Format du segment vérifié: MPEG-TS");
            } else if (segmentCtx->iformat) {
                spdlog::warn("Format du segment inattendu: {}", segmentCtx->iformat->name);
            }
        }
        avformat_close_input(&segmentCtx);
    } else {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Erreur lors de l'ouverture du segment: {}", errbuf);
    }
    
    av_dict_free(&options);
    return isMpegTS;
}

// Nouvelle méthode pour analyser en détail une playlist HLS
void HLSClient::dumpPlaylistInfo(const std::string& url) {
    spdlog::info("Analyse détaillée de la playlist: {}", url);
    
    AVFormatContext* ctx = nullptr;
    AVDictionary* options = createFFmpegOptions(true);  // Timeout plus long pour le debug
    
    int ret = avformat_open_input(&ctx, url.c_str(), nullptr, &options);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Erreur lors de l'ouverture de la playlist pour debug: {}", errbuf);
        av_dict_free(&options);
        return;
    }
    
    // Afficher les informations générales sur le format
    if (ctx->iformat) {
        spdlog::info("Format: {}", ctx->iformat->name);
    }
    
    // Récupérer le contenu brut de la playlist
    AVIOContext* ioCtx = ctx->pb;
    if (ioCtx) {
        int64_t pos = avio_tell(ioCtx);
        avio_seek(ioCtx, 0, SEEK_SET);
        
        size_t buffer_size = avio_size(ioCtx);
        if (buffer_size > 0) {
            std::vector<uint8_t> buffer(buffer_size + 1);
            int bytesRead = avio_read(ioCtx, buffer.data(), buffer_size);
            if (bytesRead > 0) {
                buffer[bytesRead] = 0;  // Null-terminate
                std::string content = reinterpret_cast<char*>(buffer.data());
                
                // Afficher le contenu de la playlist
                spdlog::info("Contenu de la playlist:");
                std::istringstream iss(content);
                std::string line;
                int lineNum = 1;
                
                // Détecter si c'est une master playlist
                bool isMasterPlaylist = false;
                
                while (std::getline(iss, line)) {
                    // Supprimer les retours à la ligne
                    line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
                        return c == '\r' || c == '\n'; 
                    }), line.end());
                    
                    spdlog::info("Ligne {}: {}", lineNum++, line);
                    
                    // Détecter si c'est une master playlist
                    if (line.find("#EXT-X-STREAM-INF:") != std::string::npos) {
                        isMasterPlaylist = true;
                    }
                }
                
                spdlog::info("Type de playlist détecté: {}", isMasterPlaylist ? "Master Playlist" : "Media Playlist");
                
                // Si c'est une master playlist, essayer de récupérer et analyser une des sous-playlists
                if (isMasterPlaylist) {
                    std::vector<std::string> variantUrls;
                    std::istringstream iss2(content);
                    bool inStreamInfo = false;
                    
                    while (std::getline(iss2, line)) {
                        line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
                            return c == '\r' || c == '\n'; 
                        }), line.end());
                        
                        if (line.find("#EXT-X-STREAM-INF:") != std::string::npos) {
                            inStreamInfo = true;
                        }
                        else if (inStreamInfo && !line.empty() && line[0] != '#') {
                            // C'est l'URL d'une variante
                            inStreamInfo = false;
                            std::string variantUrl = resolveRelativeUrl(url, line);
                            variantUrls.push_back(variantUrl);
                            spdlog::info("Variante trouvée: {}", variantUrl);
                        }
                    }
                    
                    // Analyser chaque variante pour vérifier les segments
                    for (const auto& variantUrl : variantUrls) {
                        spdlog::info("Analyse de la variante: {}", variantUrl);
                        AVFormatContext* variantCtx = nullptr;
                        AVDictionary* variantOptions = createFFmpegOptions();
                        
                        ret = avformat_open_input(&variantCtx, variantUrl.c_str(), nullptr, &variantOptions);
                        av_dict_free(&variantOptions);
                        
                        if (ret >= 0) {
                            // Lire la playlist de la variante
                            AVIOContext* varIoCtx = variantCtx->pb;
                            if (varIoCtx) {
                                int64_t varPos = avio_tell(varIoCtx);
                                avio_seek(varIoCtx, 0, SEEK_SET);
                                
                                size_t var_buffer_size = avio_size(varIoCtx);
                                std::vector<uint8_t> var_buffer(var_buffer_size + 1);
                                int var_bytesRead = avio_read(varIoCtx, var_buffer.data(), var_buffer_size);
                                
                                if (var_bytesRead > 0) {
                                    var_buffer[var_bytesRead] = 0;
                                    std::string varContent = reinterpret_cast<char*>(var_buffer.data());
                                    
                                    // Chercher les segments .ts dans la variante
                                    std::istringstream var_iss(varContent);
                                    std::string var_line;
                                    bool foundTsSegment = false;
                                    
                                    while (std::getline(var_iss, var_line)) {
                                        var_line.erase(std::remove_if(var_line.begin(), var_line.end(), [](char c) { 
                                            return c == '\r' || c == '\n'; 
                                        }), var_line.end());
                                        
                                        if (var_line.empty() || var_line[0] == '#') continue;
                                        
                                        if (var_line.find(".ts") != std::string::npos) {
                                            std::string segmentUrl = resolveRelativeUrl(variantUrl, var_line);
                                            spdlog::info("Segment MPEG-TS trouvé dans la variante: {}", segmentUrl);
                                            foundTsSegment = true;
                                            break;
                                        }
                                    }
                                    
                                    if (!foundTsSegment) {
                                        spdlog::warn("Aucun segment .ts trouvé dans la variante: {}", variantUrl);
                                    }
                                }
                                
                                avio_seek(varIoCtx, varPos, SEEK_SET);
                            }
                            
                            avformat_close_input(&variantCtx);
                        } else {
                            char errbuf[AV_ERROR_MAX_STRING_SIZE];
                            av_strerror(ret, errbuf, sizeof(errbuf));
                            spdlog::error("Erreur lors de l'ouverture de la variante {}: {}", variantUrl, errbuf);
                        }
                    }
                }
            }
        }
        
        // Restaurer la position
        avio_seek(ioCtx, pos, SEEK_SET);
    }
    
    // Analyser les métadonnées
    spdlog::info("Métadonnées:");
    AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_get(ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
        spdlog::info("  {} = {}", tag->key, tag->value);
    }
    
    // Libérer les ressources
    avformat_close_input(&ctx);
    av_dict_free(&options);
}

void HLSClient::start() {
    if (running_) {
        spdlog::warn("Le client HLS est déjà en cours d'exécution");
        return;
    }
    
    spdlog::info("=== HLSClient::start() - DÉBUT ===");
    spdlog::info("Démarrage du client HLS pour l'URL: {}", url_);
    
    // Vérifier la version de FFmpeg
    spdlog::info("FFmpeg version: {}", av_version_info());
    checkFFmpegSSLSupport();  // Vérifier le support SSL/TLS
    
    // S'assurer que l'URL commence par https:// ou http://
    if (url_.find("https://") != 0 && url_.find("http://") != 0) {
        url_ = "https://" + url_;
        spdlog::info("URL modifiée pour inclure le protocole HTTPS: {}", url_);
    }

    try {
        // Afficher des informations détaillées pour déboguer le flux
        dumpPlaylistInfo(url_);
        spdlog::info("=== Étape 1: Analyse de la playlist terminée ===");
        
        // Ouverture du flux pour analyse des variantes
        AVFormatContext* ctx = nullptr;
        
        AVDictionary* options = createFFmpegOptions();
        
        int ret = avformat_open_input(&ctx, url_.c_str(), nullptr, &options);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            av_dict_free(&options);
            throw std::runtime_error(std::string("Erreur lors de l'ouverture du flux HLS: ") + errbuf);
        }
        
        // Récupérer les informations sur le flux
        ret = avformat_find_stream_info(ctx, nullptr);
        if (ret < 0) {
            avformat_close_input(&ctx);
            av_dict_free(&options);
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("Erreur lors de la récupération des informations sur le flux: ") + errbuf);
        }
        
        av_dict_free(&options);
        spdlog::info("=== Étape 2: Informations sur le flux récupérées ===");
        
        // Récupérer explicitement le contenu de la playlist pour extraction des durées
        std::string playlistContent;
        if (fetchHLSManifestWithCurl(url_, playlistContent)) {
            spdlog::info("Contenu de la playlist récupéré avec curl, taille: {} octets", playlistContent.size());
            extractSegmentDurations(playlistContent);
            spdlog::info("=== Étape 3: Durées des segments extraites ===");
        } else {
            spdlog::warn("Impossible de récupérer le contenu de la playlist avec curl, tentative alternative");
            
            // Tentative alternative via FFmpeg
            AVIOContext* ioCtx = ctx->pb;
            if (ioCtx) {
                int64_t pos = avio_tell(ioCtx);
                avio_seek(ioCtx, 0, SEEK_SET);
                
                size_t buffer_size = avio_size(ioCtx);
                if (buffer_size > 0) {
                    std::vector<uint8_t> buffer(buffer_size + 1);
                    int bytesRead = avio_read(ioCtx, buffer.data(), buffer_size);
                    if (bytesRead > 0) {
                        buffer[bytesRead] = 0;  // Null-terminate
                        playlistContent = reinterpret_cast<char*>(buffer.data());
                        spdlog::info("Contenu de la playlist récupéré via FFmpeg, taille: {} octets", playlistContent.size());
                        extractSegmentDurations(playlistContent);
                        spdlog::info("=== Étape 3: Durées des segments extraites (via FFmpeg) ===");
                    }
                }
                
                // Restaurer la position
                avio_seek(ioCtx, pos, SEEK_SET);
            }
        }
        
        // Sélectionner le flux avec le plus grand débit
        bool validStream = selectHighestBitrateStream();
        spdlog::info("=== Étape 4: Sélection du flux terminée (résultat: {}) ===", validStream ? "succès" : "échec");
        
        // Libérer le contexte d'analyse
        avformat_close_input(&ctx);
        
        // Vérifier si le flux contient des segments MPEG-TS
        if (!validStream) {
            // Essayer l'acceptation forcée comme dernier recours
            spdlog::warn("Aucun flux valide détecté, tentative d'acceptation forcée");
            validStream = forceAcceptHLSStream();
            
            if (!validStream) {
                throw std::runtime_error("Aucun flux valide trouvé dans la playlist HLS");
            }
        }

        // Vérifier si c'est un flux live
        if (!isLiveStream()) {
            std::string errorMsg = "Le flux HLS n'est pas un flux Live. "
                                "Cette application ne peut traiter que les flux HLS Live.";
            
            spdlog::error(errorMsg);
            
            AlertManager::getInstance().addAlert(
                AlertLevel::ERROR,
                "HLSClient",
                errorMsg,
                true
            );
            
            throw std::runtime_error(errorMsg);
        }

        if (!streamInfo_.hasMPEGTSSegments) {
            // Essayer l'acceptation forcée comme dernier recours
            spdlog::warn("Pas de segments MPEG-TS détectés, tentative d'acceptation forcée");
            if (!forceAcceptHLSStream()) {
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
        }
        
        spdlog::info("=== Étape 5: Vérification des segments MPEG-TS terminée ===");
        
        // Vérifier les discontinuités
        bool hasDiscontinuities = checkForDiscontinuities(streamInfo_.url);
        spdlog::info("Vérification des discontinuités terminée (résultat: {})", 
                   hasDiscontinuities ? "discontinuités détectées" : "pas de discontinuité");
        
        spdlog::info("=== Étape 6: Vérification des discontinuités terminée ===");
        
        // Ouvrir le flux pour le traitement des segments
        formatContext_ = avformat_alloc_context();
        if (!formatContext_) {
            throw std::runtime_error("Impossible d'allouer le contexte de format AVFormat");
        }
        
        // Configurer les options pour le client HLS
        options = createFFmpegOptions();
        
        // Utiliser l'URL du flux de plus haut débit
        spdlog::info("Ouverture du flux pour traitement: {}", streamInfo_.url);
        ret = avformat_open_input(&formatContext_, streamInfo_.url.c_str(), nullptr, &options);
        av_dict_free(&options);
        
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("Erreur lors de l'ouverture du flux HLS: ") + errbuf);
        }
        
        // Récupérer les informations sur le flux
        spdlog::info("Récupération des informations sur le flux pour traitement");
        ret = avformat_find_stream_info(formatContext_, nullptr);
        if (ret < 0) {
            avformat_close_input(&formatContext_);
            formatContext_ = nullptr;
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("Erreur lors de la récupération des informations sur le flux: ") + errbuf);
        }
        
        spdlog::info("=== Étape 7: Ouverture du flux pour traitement terminée ===");

        // Vérification finale des informations du flux
        if (streamInfo_.width == 0 || streamInfo_.height == 0 || streamInfo_.bandwidth == 0 || streamInfo_.codecs.empty()) {
            spdlog::warn("Informations du flux incomplètes, utilisation de valeurs par défaut");
            
            if (streamInfo_.width == 0 || streamInfo_.height == 0) {
                streamInfo_.width = 1280;
                streamInfo_.height = 720;
            }
            
            if (streamInfo_.bandwidth == 0) {
                streamInfo_.bandwidth = 2000000; // 2 Mbps
            }
            
            if (streamInfo_.codecs.empty()) {
                streamInfo_.codecs = "h264,aac";
            }
        }
        
        spdlog::info("Client HLS configuré avec flux: {}x{}, {}kbps, codecs: {}",
                    streamInfo_.width, streamInfo_.height, streamInfo_.bandwidth / 1000, streamInfo_.codecs);
        
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
        
        spdlog::info("=== HLSClient::start() - FIN ===");
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
        spdlog::info("Sélection du flux HLS avec le plus grand débit");
        
        // Ouvrir le flux HLS principal
        AVFormatContext* ctx = nullptr;
        AVDictionary* options = createFFmpegOptions();
        
        int ret = avformat_open_input(&ctx, url_.c_str(), nullptr, &options);
        av_dict_free(&options);
        
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("Erreur lors de l'ouverture du flux HLS: ") + errbuf);
        }
        
        // Lire le contenu de la playlist
        std::string playlistContent;
        AVIOContext* ioCtx = ctx->pb;
        if (ioCtx) {
            int64_t pos = avio_tell(ioCtx);
            avio_seek(ioCtx, 0, SEEK_SET);
            
            size_t buffer_size = avio_size(ioCtx);
            if (buffer_size > 0) {
                std::vector<uint8_t> buffer(buffer_size + 1);
                
                int bytesRead = avio_read(ioCtx, buffer.data(), buffer_size);
                if (bytesRead > 0) {
                    buffer[bytesRead] = 0;  // Null-terminate
                    playlistContent = reinterpret_cast<char*>(buffer.data());
                    spdlog::info("Contenu de la playlist principale récupéré, taille: {} octets", playlistContent.size());
                    
                    // Journaliser les premiers caractères pour le debug
                    spdlog::debug("Début du contenu: {}", 
                                playlistContent.substr(0, std::min(static_cast<size_t>(200), playlistContent.size())));
                }
            } else {
                spdlog::warn("Taille de la playlist principale indisponible ou nulle");
            }
            
            // Restaurer la position
            avio_seek(ioCtx, pos, SEEK_SET);
        }
        
        // Vérifier si c'est une master playlist
        bool masterPlaylist = isMasterPlaylist(playlistContent);
        spdlog::info("Type de playlist détecté: {}", masterPlaylist ? "Master Playlist" : "Media Playlist");
        
        if (masterPlaylist) {
            // C'est une playlist principale avec des variantes
            spdlog::info("Analyse de la master playlist pour trouver les variantes");
            
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
            
            // Analyser la playlist HLS
            std::istringstream iss(playlistContent);
            std::string line;
            VariantInfo currentVariant;
            bool inStreamInfo = false;
            
            while (std::getline(iss, line)) {
                // Supprimer les espaces et les retours à la ligne
                line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
                    return c == '\r' || c == '\n'; 
                }), line.end());
                
                if (line.empty()) continue;
                
                spdlog::debug("Analyse de la ligne: {}", line);
                
                // Analyser les lignes EXT-X-STREAM-INF
                if (line.find("#EXT-X-STREAM-INF:") != std::string::npos) {
                    inStreamInfo = true;
                    currentVariant = VariantInfo();
                    currentVariant.bandwidth = 0;
                    currentVariant.width = 0;
                    currentVariant.height = 0;
                    currentVariant.hasMPEGTSSegments = false;
                    
                    // Extraire les attributs avec une méthode plus robuste
                    std::string bandwidthStr = extractAttributeValue(line, "BANDWIDTH");
                    if (!bandwidthStr.empty()) {
                        try {
                            currentVariant.bandwidth = std::stoi(bandwidthStr);
                            spdlog::info("Bande passante détectée: {} bps", currentVariant.bandwidth);
                        } catch (const std::exception& e) {
                            spdlog::warn("Erreur de conversion de bande passante '{}': {}", bandwidthStr, e.what());
                        }
                    }
                    
                    std::string codecs = extractAttributeValue(line, "CODECS");
                    if (!codecs.empty()) {
                        currentVariant.codecs = codecs;
                        spdlog::info("Codecs détectés: {}", codecs);
                    }
                    
                    std::string resolution = extractAttributeValue(line, "RESOLUTION");
                    if (!resolution.empty()) {
                        size_t xPos = resolution.find('x');
                        if (xPos != std::string::npos) {
                            try {
                                currentVariant.width = std::stoi(resolution.substr(0, xPos));
                                currentVariant.height = std::stoi(resolution.substr(xPos + 1));
                                spdlog::info("Résolution détectée: {}x{}", currentVariant.width, currentVariant.height);
                            } catch (const std::exception& e) {
                                spdlog::warn("Erreur de conversion de résolution '{}': {}", resolution, e.what());
                            }
                        }
                    }
                }
                else if (inStreamInfo && !line.empty() && line[0] != '#') {
                    // C'est l'URL de la variante
                    inStreamInfo = false;
                    
                    // Résoudre l'URL relative
                    currentVariant.url = resolveRelativeUrl(url_, line);
                    spdlog::info("URL de la variante: {}", currentVariant.url);
                    
                    // On va d'abord ajouter la variante sans vérifier les segments
                    variants.push_back(currentVariant);
                }
            }
            
            spdlog::info("{} variantes trouvées dans la master playlist", variants.size());
            
            if (variants.empty()) {
                spdlog::error("Aucune variante détectée dans la master playlist");
                avformat_close_input(&ctx);
                return false;
            }
            
            // Maintenant, vérifier si au moins une variante a des segments MPEG-TS
            for (auto& variant : variants) {
                spdlog::info("Vérification des segments MPEG-TS pour la variante: {}", variant.url);
                
                AVFormatContext* variantCtx = nullptr;
                AVDictionary* varOptions = createFFmpegOptions();
                
                ret = avformat_open_input(&variantCtx, variant.url.c_str(), nullptr, &varOptions);
                av_dict_free(&varOptions);
                
                if (ret >= 0) {
                    // Vérifier si la variante contient des segments MPEG-TS
                    variant.hasMPEGTSSegments = hasMPEGTSSegments(variantCtx);
                    spdlog::info("Variante {}: segments MPEG-TS = {}", 
                               variant.url, variant.hasMPEGTSSegments ? "oui" : "non");
                    
                    avformat_close_input(&variantCtx);
                    
                    if (variant.hasMPEGTSSegments) {
                        spdlog::info("Variante avec segments MPEG-TS trouvée: {}x{}, {}kbps, codecs: {}, URL: {}",
                                   variant.width, variant.height, variant.bandwidth / 1000, 
                                   variant.codecs, variant.url);
                    }
                } else {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    spdlog::warn("Impossible d'ouvrir la variante {}: {}", variant.url, errbuf);
                    
                    // En cas d'échec, on peut tenter une vérification directe du nom de fichier
                    if (variant.url.find(".ts") != std::string::npos || 
                        variant.url.find(".m3u8") != std::string::npos) {
                        spdlog::info("Variante potentiellement compatible MPEG-TS trouvée (via extension): {}", variant.url);
                        variant.hasMPEGTSSegments = true;
                    }
                }
            }
            
            // Sélectionner les variantes valides
            std::vector<VariantInfo> validVariants;
            for (const auto& v : variants) {
                if (v.hasMPEGTSSegments) {
                    validVariants.push_back(v);
                }
            }
            
            // Si aucune variante n'a de segments MPEG-TS, supposer qu'elles en ont toutes
            if (validVariants.empty()) {
                spdlog::warn("Aucune variante avec segments MPEG-TS détectée. Supposons que toutes sont compatibles.");
                validVariants = variants;
            }
            
            // Si toujours aucune variante, échec
            if (validVariants.empty()) {
                spdlog::error("Aucune variante trouvée dans le flux HLS");
                avformat_close_input(&ctx);
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
            streamInfo_.hasMPEGTSSegments = true;  // Forcer à true puisqu'elle a été sélectionnée
            
            spdlog::info("Variante sélectionnée: {}x{}, {}kbps, codecs: {}, URL: {}",
                       bestVariant.width, bestVariant.height, bestVariant.bandwidth / 1000, 
                       bestVariant.codecs, bestVariant.url);
        }
        else {
            // C'est un flux HLS sans variantes
            spdlog::info("Playlist média simple détectée (sans variantes)");
            
            streamInfo_.url = url_;
            streamInfo_.hasMPEGTSSegments = hasMPEGTSSegments(ctx);
            
            spdlog::info("Segments MPEG-TS détectés: {}", streamInfo_.hasMPEGTSSegments ? "oui" : "non");
            
            // Extraire les informations de codecs et résolution
            ret = avformat_find_stream_info(ctx, nullptr);
            if (ret >= 0) {
                spdlog::info("Nombre de flux détectés: {}", ctx->nb_streams);
                
                for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
                    AVStream* stream = ctx->streams[i];
                    
                    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        streamInfo_.width = stream->codecpar->width;
                        streamInfo_.height = stream->codecpar->height;
                        
                        spdlog::info("Flux vidéo détecté: {}x{}", streamInfo_.width, streamInfo_.height);
                        
                        if (stream->codecpar->bit_rate > 0) {
                            streamInfo_.bandwidth = stream->codecpar->bit_rate;
                            spdlog::info("Débit vidéo: {} bps", streamInfo_.bandwidth);
                        } else if (ctx->bit_rate > 0) {
                            streamInfo_.bandwidth = ctx->bit_rate;
                            spdlog::info("Débit global: {} bps", streamInfo_.bandwidth);
                        }
                        
                        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
                        if (codec) {
                            if (streamInfo_.codecs.empty()) {
                                streamInfo_.codecs = codec->name;
                            } else {
                                streamInfo_.codecs += "," + std::string(codec->name);
                            }
                            spdlog::info("Codec vidéo: {}", codec->name);
                        }
                    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
                        if (codec) {
                            if (streamInfo_.codecs.empty()) {
                                streamInfo_.codecs = codec->name;
                            } else {
                                streamInfo_.codecs += "," + std::string(codec->name);
                            }
                            spdlog::info("Codec audio: {}", codec->name);
                        }
                    }
                }
            } else {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                spdlog::warn("Impossible de récupérer les informations des flux: {}", errbuf);
            }
        }
        
        avformat_close_input(&ctx);
        
        // Vérifier et fournir des valeurs par défaut si nécessaire
        if (streamInfo_.width == 0 || streamInfo_.height == 0) {
            spdlog::warn("Résolution non détectée, utilisation de valeurs par défaut (1280x720)");
            streamInfo_.width = 1280;
            streamInfo_.height = 720;
        }
        
        if (streamInfo_.bandwidth == 0) {
            spdlog::warn("Débit non détecté, utilisation d'une valeur par défaut (2 Mbps)");
            streamInfo_.bandwidth = 2000000; // 2 Mbps
        }
        
        if (streamInfo_.codecs.empty()) {
            spdlog::warn("Codecs non détectés, utilisation d'une valeur par défaut (h264,aac)");
            streamInfo_.codecs = "h264,aac";
        } else {
            // Simplifier les noms des codecs pour l'affichage
            if (streamInfo_.codecs.find("avc1") != std::string::npos) {
                streamInfo_.codecs = "h264";
                if (streamInfo_.codecs.find("mp4a") != std::string::npos) {
                    streamInfo_.codecs += ",aac";
                }
            }
        }
        
        spdlog::info("Informations finales du flux sélectionné: {}x{}, {}kbps, codecs: {}, hasMPEGTSSegments: {}",
                   streamInfo_.width, streamInfo_.height, streamInfo_.bandwidth / 1000, 
                   streamInfo_.codecs, streamInfo_.hasMPEGTSSegments ? "oui" : "non");
        
        if (!streamInfo_.hasMPEGTSSegments) {
            spdlog::error("Le flux HLS ne contient pas de segments MPEG-TS");
            return false;
        }
        
        return true;
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


AVDictionary* HLSClient::createFFmpegOptions(bool longTimeout) {
    AVDictionary* options = nullptr;
    
    // Options de base
    av_dict_set(&options, "http_persistent", "0", 0);  // Désactiver les connexions persistantes
    
    // Timeout
    int timeout = longTimeout ? 30000000 : 15000000;  // 30s ou 15s en microsecondes
    av_dict_set(&options, "timeout", std::to_string(timeout).c_str(), 0);
    
    // Timeout spécifique pour la connexion TCP/TLS
    av_dict_set(&options, "stimeout", "10000000", 0);  // 10 secondes en microsecondes
    
    // Timeout de lecture/écriture (plus long pour gérer les réseaux lents)
    av_dict_set(&options, "rw_timeout", "15000000", 0);  // 15 secondes
    
    // Options HTTP/HTTPS spécifiques
    av_dict_set(&options, "protocol_whitelist", "file,http,https,tcp,tls,crypto", 0);
    av_dict_set(&options, "icy", "0", 0);  // Désactiver les métadonnées ICY
    
    // Force l'utilisation de TLS/SSL pour les connexions HTTPS
    av_dict_set(&options, "tls_verify", "0", 0);  // Désactiver la vérification SSL pour les tests
    av_dict_set(&options, "reconnect", "1", 0);   // Autoriser les reconnexions
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "5", 0);  // Délai max entre les reconnexions
    
    // Augmenter la taille du buffer
    av_dict_set(&options, "buffer_size", "524288", 0);  // 512KB buffer (augmenté)
    
    // Options spécifiques à macOS
    av_dict_set(&options, "verify_peer", "0", 0);  // Ignorer les erreurs de certificat
    
    // Options spécifiques HLS
    av_dict_set(&options, "hls_allow_cache", "0", 0);  // Ne pas mettre en cache les segments
    av_dict_set(&options, "live_start_index", "-1", 0);  // Dernier segment dans un stream live
    av_dict_set(&options, "hls_flags", "single_file,no_headers", 0);  // Options HLS
    av_dict_set(&options, "hls_seek_time", "0", 0);  // Commencer dès le début
    av_dict_set(&options, "hls_segment_type", "mpegts", 0);  // Forcer le type MPEG-TS
    
    return options;
}


// Fonction pour extraire une valeur d'un attribut dans une ligne de manifeste HLS
std::string HLSClient::extractAttributeValue(const std::string& line, const std::string& attributeName) {
    std::string searchStr = attributeName + "=";
    size_t pos = line.find(searchStr);
    if (pos == std::string::npos) {
        return "";
    }
    
    pos += searchStr.length();
    
    spdlog::debug("Extraction de l'attribut {}, position: {}", attributeName, pos);
    
    if (pos >= line.length()) {
        spdlog::warn("Position hors limites lors de l'extraction de {}", attributeName);
        return "";
    }
    
    // Vérifier si la valeur est entre guillemets
    if (line[pos] == '"') {
        // Valeur entre guillemets
        pos++;  // Sauter le guillemet ouvrant
        size_t endPos = line.find('"', pos);
        if (endPos == std::string::npos) {
            spdlog::warn("Guillemet fermant non trouvé pour l'attribut {}", attributeName);
            // Essayer de récupérer jusqu'à la fin de la ligne ou jusqu'à une virgule
            endPos = line.find(',', pos);
            if (endPos == std::string::npos) {
                return line.substr(pos);
            }
            return line.substr(pos, endPos - pos);
        }
        std::string value = line.substr(pos, endPos - pos);
        spdlog::debug("Valeur extraite pour {} (avec guillemets): '{}'", attributeName, value);
        return value;
    } else {
        // Valeur sans guillemets (nombre ou valeur simple)
        size_t endPos = line.find(',', pos);
        if (endPos == std::string::npos) {
            // Dernier attribut de la ligne
            std::string value = line.substr(pos);
            spdlog::debug("Valeur extraite pour {} (dernier attribut): '{}'", attributeName, value);
            return value;
        }
        std::string value = line.substr(pos, endPos - pos);
        spdlog::debug("Valeur extraite pour {}: '{}'", attributeName, value);
        return value;
    }
}


bool HLSClient::isMasterPlaylist(const std::string& content) {
    return content.find("#EXT-X-STREAM-INF:") != std::string::npos;
}

void HLSClient::checkFFmpegSSLSupport() {
    // Vérifier les protocoles supportés
    void* opaque = nullptr;
    const char* protocol_name;
    bool has_https = false;
    bool has_tls = false;
    
    while ((protocol_name = avio_enum_protocols(&opaque, 0))) {
        if (strcmp(protocol_name, "https") == 0) {
            has_https = true;
        } else if (strcmp(protocol_name, "tls") == 0) {
            has_tls = true;
        }
    }
    
    if (!has_https || !has_tls) {
        spdlog::warn("FFmpeg ne semble pas avoir été compilé avec le support HTTPS/TLS complet");
        spdlog::warn("Protocoles disponibles :");
        
        opaque = nullptr;
        while ((protocol_name = avio_enum_protocols(&opaque, 0))) {
            spdlog::warn("  {}", protocol_name);
        }
    } else {
        spdlog::info("FFmpeg supporte les protocoles HTTPS et TLS");
    }
}

bool HLSClient::fetchHLSManifestWithCurl(const std::string& url, std::string& content) {
    std::string tempFileName = "/tmp/hls_manifest_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".m3u8";
    
    // Construire la commande curl avec timeout et options SSL améliorées
    std::string command = "curl -s -k --connect-timeout 10 -m 20 -o " + tempFileName + 
                          " --retry 3 --retry-delay 2 --retry-max-time 30" +
                          " -H \"User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36\"" +
                          " \"" + url + "\"";
    
    spdlog::info("Exécution de la commande curl : {}", command);
    int result = system(command.c_str());
    
    if (result != 0) {
        spdlog::error("Échec de l'exécution de curl, code de retour: {}", result);
        return false;
    }
    
    // Lire le contenu du fichier téléchargé
    std::ifstream file(tempFileName);
    if (!file.is_open()) {
        spdlog::error("Impossible d'ouvrir le fichier téléchargé: {}", tempFileName);
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    content = buffer.str();
    
    file.close();
    
    // Supprimer le fichier temporaire
    std::remove(tempFileName.c_str());
    
    if (content.empty()) {
        spdlog::warn("Contenu de la playlist vide après téléchargement");
        return false;
    }
    
    spdlog::info("Playlist téléchargée avec succès, taille: {} octets", content.size());
    spdlog::debug("Début du contenu de la playlist: {}", 
                content.substr(0, std::min(static_cast<size_t>(200), content.size())));
    
    return true;
}


bool HLSClient::hasMPEGTSSegments(AVFormatContext* formatContext) {
    if (!formatContext) {
        spdlog::error("Contexte de format FFmpeg non initialisé");
        return false;
    }
    
    spdlog::info("Vérification de la présence de segments MPEG-TS");
    
    // Vérifier si le format est HLS
    if (formatContext->iformat && 
        (strcmp(formatContext->iformat->name, "hls") == 0 || 
         strcmp(formatContext->iformat->name, "applehttp") == 0)) {
        
        spdlog::info("Format HLS détecté: {}", formatContext->iformat->name);
        
        // Récupérer l'URL de la playlist
        std::string playlistUrl = streamInfo_.url.empty() ? url_ : streamInfo_.url;
        
        // Lire le contenu de la playlist
        AVIOContext* ioCtx = formatContext->pb;
        std::string playlistContent;
        
        if (ioCtx) {
            // Sauvegarder la position actuelle
            int64_t pos = avio_tell(ioCtx);
            avio_seek(ioCtx, 0, SEEK_SET);
            
            // Lire tout le fichier
            size_t buffer_size = avio_size(ioCtx);
            if (buffer_size > 0) {
                std::vector<uint8_t> buffer(buffer_size + 1);
                
                int bytesRead = avio_read(ioCtx, buffer.data(), buffer_size);
                if (bytesRead > 0) {
                    buffer[bytesRead] = 0;  // Null-terminate
                    playlistContent = reinterpret_cast<char*>(buffer.data());
                    spdlog::info("Contenu de la playlist lu, taille: {} octets", playlistContent.size());
                    
                    spdlog::debug("Extrait du contenu: {}", 
                                playlistContent.substr(0, std::min(static_cast<size_t>(200), playlistContent.size())));
                } else {
                    spdlog::warn("Impossible de lire le contenu de la playlist: {}", bytesRead);
                }
            } else {
                spdlog::warn("Taille de la playlist indisponible ou nulle");
            }
            
            // Restaurer la position
            avio_seek(ioCtx, pos, SEEK_SET);
        } else {
            spdlog::warn("Contexte I/O non disponible pour lire la playlist");
        }
        
        // Si la lecture directe échoue, essayer avec curl
        if (playlistContent.empty()) {
            spdlog::info("Tentative de récupération de la playlist avec curl: {}", playlistUrl);
            if (fetchHLSManifestWithCurl(playlistUrl, playlistContent)) {
                spdlog::info("Contenu de la playlist récupéré avec curl, taille: {} octets", playlistContent.size());
            } else {
                spdlog::error("Impossible de récupérer le contenu de la playlist avec curl");
            }
        }
        
        // Vérifier si c'est une master playlist
        bool isMasterPlaylist = false;
        
        if (!playlistContent.empty()) {
            std::istringstream iss(playlistContent);
            std::string line;
            
            while (std::getline(iss, line)) {
                if (line.find("#EXT-X-STREAM-INF:") != std::string::npos) {
                    isMasterPlaylist = true;
                    spdlog::info("Master playlist détectée");
                    break;
                }
            }
        } else {
            spdlog::warn("Contenu de la playlist vide, impossible de déterminer le type");
        }
        
        // Si c'est une master playlist, on doit analyser les sous-playlists
        if (isMasterPlaylist) {
            spdlog::info("Master playlist HLS détectée, analyse des variantes");
            
            std::vector<std::string> variantUrls;
            std::istringstream iss(playlistContent);
            std::string line;
            bool inStreamInfo = false;
            
            while (std::getline(iss, line)) {
                line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
                    return c == '\r' || c == '\n'; 
                }), line.end());
                
                if (line.find("#EXT-X-STREAM-INF:") != std::string::npos) {
                    inStreamInfo = true;
                }
                else if (inStreamInfo && !line.empty() && line[0] != '#') {
                    // C'est l'URL d'une variante
                    inStreamInfo = false;
                    std::string variantUrl = resolveRelativeUrl(playlistUrl, line);
                    variantUrls.push_back(variantUrl);
                    spdlog::info("Variante trouvée: {}", variantUrl);
                }
            }
            
            spdlog::info("{} variantes trouvées", variantUrls.size());
            
            // Vérifier chaque variante pour les segments MPEG-TS
            for (const auto& variantUrl : variantUrls) {
                spdlog::info("Analyse de la variante: {}", variantUrl);
                
                // Tenter d'abord avec curl pour être plus efficace
                std::string variantContent;
                if (fetchHLSManifestWithCurl(variantUrl, variantContent)) {
                    spdlog::info("Contenu de la variante récupéré avec curl, taille: {} octets", variantContent.size());
                    
                    // Chercher les segments .ts dans la variante
                    std::istringstream var_iss(variantContent);
                    std::string var_line;
                    
                    while (std::getline(var_iss, var_line)) {
                        var_line.erase(std::remove_if(var_line.begin(), var_line.end(), [](char c) { 
                            return c == '\r' || c == '\n'; 
                        }), var_line.end());
                        
                        if (var_line.empty() || var_line[0] == '#') continue;
                        
                        if (var_line.find(".ts") != std::string::npos) {
                            std::string segmentUrl = resolveRelativeUrl(variantUrl, var_line);
                            spdlog::info("Segment MPEG-TS trouvé dans la variante: {}", segmentUrl);
                            
                            // Vérifier explicitement le format du segment
                            if (checkSegmentFormat(segmentUrl)) {
                                spdlog::info("Format du segment vérifié: MPEG-TS");
                                return true;
                            } else {
                                spdlog::warn("Le segment a l'extension .ts mais n'est pas au format MPEG-TS");
                            }
                        }
                        
                        // Même sans extension .ts, essayer de vérifier si c'est un segment
                        // car certains flux HLS n'utilisent pas d'extension
                        if (!var_line.empty() && var_line[0] != '#') {
                            std::string segmentUrl = resolveRelativeUrl(variantUrl, var_line);
                            spdlog::info("Segment potentiel trouvé (sans extension .ts): {}", segmentUrl);
                            
                            // Vérifier explicitement le format du segment
                            if (checkSegmentFormat(segmentUrl)) {
                                spdlog::info("Format du segment vérifié: MPEG-TS");
                                return true;
                            }
                        }
                    }
                    
                    // Même si on n'a pas trouvé de segments .ts, vérifier s'il y a d'autres indices
                    if (variantContent.find("#EXT-X-VERSION") != std::string::npos) {
                        spdlog::info("Playlist HLS valide détectée, supposons qu'elle contient des segments MPEG-TS");
                        return true;
                    }
                } else {
                    // Si curl échoue, essayer avec FFmpeg
                    spdlog::info("Tentative d'ouverture de la variante avec FFmpeg: {}", variantUrl);
                    AVFormatContext* variantCtx = nullptr;
                    AVDictionary* varOptions = createFFmpegOptions();
                    
                    int ret = avformat_open_input(&variantCtx, variantUrl.c_str(), nullptr, &varOptions);
                    av_dict_free(&varOptions);
                    
                    if (ret >= 0) {
                        // Lire la playlist de la variante
                        AVIOContext* varIoCtx = variantCtx->pb;
                        if (varIoCtx) {
                            int64_t varPos = avio_tell(varIoCtx);
                            avio_seek(varIoCtx, 0, SEEK_SET);
                            
                            size_t var_buffer_size = avio_size(varIoCtx);
                            if (var_buffer_size > 0) {
                                std::vector<uint8_t> var_buffer(var_buffer_size + 1);
                                int var_bytesRead = avio_read(varIoCtx, var_buffer.data(), var_buffer_size);
                                
                                if (var_bytesRead > 0) {
                                    var_buffer[var_bytesRead] = 0;
                                    std::string varContent = reinterpret_cast<char*>(var_buffer.data());
                                    
                                    // Chercher les segments .ts dans la variante
                                    std::istringstream var_iss(varContent);
                                    std::string var_line;
                                    
                                    while (std::getline(var_iss, var_line)) {
                                        var_line.erase(std::remove_if(var_line.begin(), var_line.end(), [](char c) { 
                                            return c == '\r' || c == '\n'; 
                                        }), var_line.end());
                                        
                                        if (var_line.empty() || var_line[0] == '#') continue;
                                        
                                        if (var_line.find(".ts") != std::string::npos) {
                                            std::string segmentUrl = resolveRelativeUrl(variantUrl, var_line);
                                            spdlog::info("Segment MPEG-TS trouvé dans la variante: {}", segmentUrl);
                                            
                                            // Vérifier explicitement le format du segment
                                            if (checkSegmentFormat(segmentUrl)) {
                                                avformat_close_input(&variantCtx);
                                                return true;
                                            }
                                        }
                                    }
                                }
                            }
                            
                            avio_seek(varIoCtx, varPos, SEEK_SET);
                        }
                        
                        avformat_close_input(&variantCtx);
                    } else {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(ret, errbuf, sizeof(errbuf));
                        spdlog::warn("Impossible d'ouvrir la variante {}: {}", variantUrl, errbuf);
                    }
                }
            }
            
            // Si on a trouvé des variantes mais pas de segments .ts, considérer la compatibilité des codecs
            if (!variantUrls.empty()) {
                spdlog::warn("Aucun segment .ts trouvé dans les variantes, vérification des codecs");
                
                // Vérifier les codecs dans le flux principal
                bool hasCompatibleStreams = false;
                for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
                    AVStream* stream = formatContext->streams[i];
                    if (stream->codecpar->codec_id == AV_CODEC_ID_H264 ||
                        stream->codecpar->codec_id == AV_CODEC_ID_HEVC ||
                        stream->codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
                        stream->codecpar->codec_id == AV_CODEC_ID_AAC ||
                        stream->codecpar->codec_id == AV_CODEC_ID_MP3 ||
                        stream->codecpar->codec_id == AV_CODEC_ID_AC3) {
                        hasCompatibleStreams = true;
                        spdlog::info("Codec compatible détecté: {}", static_cast<int>(stream->codecpar->codec_id));
                        break;
                    }
                }
                
                if (hasCompatibleStreams) {
                    spdlog::info("Codecs compatibles détectés, supposons que c'est un flux MPEG-TS");
                    return true;
                }
                
                // Comme dernier recours, supposer que c'est compatible
                spdlog::warn("Aucun codec compatible détecté mais flux HLS valide, supposons qu'il est compatible");
                return true;
            }
            
            return false;
        }
        else {
            // Playlist media (non-master), vérifier directement les segments
            spdlog::info("Playlist média simple détectée, recherche directe de segments MPEG-TS");
            
            if (!playlistContent.empty()) {
                std::istringstream iss(playlistContent);
                std::string line;
                
                while (std::getline(iss, line)) {
                    // Supprimer les espaces et les retours à la ligne
                    line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
                        return c == '\r' || c == '\n'; 
                    }), line.end());
                    
                    if (line.empty() || line[0] == '#') continue; // Ignorer les commentaires
                    
                    // Vérifier si la ligne contient une référence à un fichier .ts
                    if (line.find(".ts") != std::string::npos) {
                        std::string segmentUrl = resolveRelativeUrl(playlistUrl, line);
                        spdlog::info("Segment MPEG-TS trouvé dans la playlist HLS: {}", segmentUrl);
                        
                        // Vérifier explicitement le format du segment
                        if (checkSegmentFormat(segmentUrl)) {
                            return true;
                        }
                    }
                    
                    // Même sans extension .ts, essayer de vérifier si c'est un segment
                    if (!line.empty() && line[0] != '#') {
                        std::string segmentUrl = resolveRelativeUrl(playlistUrl, line);
                        spdlog::info("Segment potentiel trouvé (sans extension .ts): {}", segmentUrl);
                        
                        // Vérifier explicitement le format du segment
                        if (checkSegmentFormat(segmentUrl)) {
                            return true;
                        }
                    }
                }
                
                spdlog::warn("Aucun segment .ts trouvé dans la playlist HLS");
            } else {
                spdlog::warn("Contenu de la playlist vide, impossible de vérifier les segments");
            }
        }
        
        // Si nous n'avons pas pu vérifier directement, nous utilisons l'approche actuelle
        // comme fallback, mais avec une exigence moins stricte
        bool hasCompatibleStreams = false;
        for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
            AVStream* stream = formatContext->streams[i];
            if (stream->codecpar->codec_id == AV_CODEC_ID_H264 ||
                stream->codecpar->codec_id == AV_CODEC_ID_HEVC ||
                stream->codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
                stream->codecpar->codec_id == AV_CODEC_ID_AAC ||
                stream->codecpar->codec_id == AV_CODEC_ID_MP3 ||
                stream->codecpar->codec_id == AV_CODEC_ID_AC3) {
                hasCompatibleStreams = true;
                spdlog::info("Codec compatible détecté: {}", static_cast<int>(stream->codecpar->codec_id));
                break;
            }
        }
        
        // Si aucun segment n'a été trouvé mais que des codecs compatibles sont présents,
        // on suppose que c'est un flux HLS avec des segments MPEG-TS
        if (hasCompatibleStreams) {
            spdlog::info("Aucun segment .ts trouvé, mais codecs compatibles détectés");
            return true;
        }
    } else if (formatContext->iformat) {
        spdlog::info("Format non-HLS détecté: {}", formatContext->iformat->name);
        
        // Vérifier si c'est directement un flux MPEG-TS
        if (strcmp(formatContext->iformat->name, "mpegts") == 0) {
            spdlog::info("Format MPEG-TS détecté directement");
            return true;
        }
    }
    
    spdlog::warn("Aucun segment MPEG-TS détecté");
    return false;
}

bool HLSClient::isLiveStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Vérifier si nous avons déjà déterminé que c'est un flux live
    if (streamInfo_.isLive.has_value()) {
        return streamInfo_.isLive.value();
    }
    
    spdlog::info("Vérification si le flux est Live ou VOD");
    
    try {
        // Récupérer le contenu de la playlist
        std::string playlistContent;
        if (!fetchHLSManifestWithCurl(streamInfo_.url.empty() ? url_ : streamInfo_.url, playlistContent)) {
            spdlog::error("Impossible de récupérer la playlist pour vérification Live/VOD");
            return false;
        }
        
        // Vérifier si c'est une playlist maître
        if (playlistContent.find("#EXT-X-STREAM-INF") != std::string::npos) {
            // C'est une playlist maître, nous devons analyser les variantes
            std::istringstream iss(playlistContent);
            std::string line;
            std::string variantUrl;
            bool inStreamInfo = false;
            
            while (std::getline(iss, line)) {
                line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
                    return c == '\r' || c == '\n'; 
                }), line.end());
                
                if (line.find("#EXT-X-STREAM-INF") != std::string::npos) {
                    inStreamInfo = true;
                } else if (inStreamInfo && !line.empty() && line[0] != '#') {
                    // C'est une URL de variante
                    variantUrl = resolveRelativeUrl(url_, line);
                    break;  // On prend la première variante trouvée
                }
            }
            
            if (!variantUrl.empty()) {
                // Récupérer la playlist de la variante
                if (!fetchHLSManifestWithCurl(variantUrl, playlistContent)) {
                    spdlog::error("Impossible de récupérer la playlist de variante pour vérification Live/VOD");
                    return false;
                }
            } else {
                spdlog::error("Aucune variante trouvée dans la playlist maître");
                return false;
            }
        }
        
        // Vérifier simplement si la playlist contient #EXT-X-ENDLIST (indique un VOD)
        bool hasEndlist = (playlistContent.find("#EXT-X-ENDLIST") != std::string::npos);
        bool isLive = !hasEndlist;
        
        // Afficher le résultat dans les logs
        if (isLive) {
            spdlog::info("Le flux a été détecté comme LIVE (pas de tag EXT-X-ENDLIST)");
        } else {
            spdlog::info("Le flux a été détecté comme VOD (présence du tag EXT-X-ENDLIST)");
        }
        
        // Stocker le résultat pour les appels futurs
        streamInfo_.isLive = isLive;
        
        return isLive;
    } catch (const std::exception& e) {
        spdlog::error("Erreur lors de la vérification Live/VOD: {}", e.what());
        return false;
    }
}


bool HLSClient::forceAcceptHLSStream() {
    // Cette fonction est appelée lorsque la détection standard a échoué
    // mais que nous voulons tenter de traiter le flux quand même
    
    spdlog::warn("Tentative d'acceptation forcée du flux HLS");
    
    // Essayer de déterminer si c'est une master playlist
    AVFormatContext* ctx = nullptr;
    AVDictionary* options = createFFmpegOptions();
    
    int ret = avformat_open_input(&ctx, url_.c_str(), nullptr, &options);
    av_dict_free(&options);
    
    if (ret < 0) {
        spdlog::error("Impossible d'ouvrir le flux pour l'acceptation forcée");
        return false;
    }
    
    bool isMasterPlaylist = false;
    std::string bestVariantUrl = url_; // Par défaut, utiliser l'URL principale
    int bestBandwidth = 0;
    std::string bestCodecs = "";
    int bestWidth = 0;
    int bestHeight = 0;
    
    // Récupérer le contenu de la playlist
    AVIOContext* ioCtx = ctx->pb;
    std::string playlistContent;
    
    if (ioCtx) {
        int64_t pos = avio_tell(ioCtx);
        avio_seek(ioCtx, 0, SEEK_SET);
        
        size_t buffer_size = avio_size(ioCtx);
        std::vector<uint8_t> buffer(buffer_size + 1);
        
        int bytesRead = avio_read(ioCtx, buffer.data(), buffer_size);
        if (bytesRead > 0) {
            buffer[bytesRead] = 0;
            playlistContent = reinterpret_cast<char*>(buffer.data());
            
            // Vérifier si c'est une master playlist
            std::istringstream iss(playlistContent);
            std::string line;
            
            while (std::getline(iss, line)) {
                if (line.find("#EXT-X-STREAM-INF:") != std::string::npos) {
                    isMasterPlaylist = true;
                    break;
                }
            }
            
            // Si c'est une master playlist, sélectionner la variante avec le plus grand débit
            if (isMasterPlaylist) {
                struct VariantInfo {
                    std::string url;
                    int bandwidth = 0;
                    std::string codecs;
                    int width = 0;
                    int height = 0;
                };
                
                std::vector<VariantInfo> variants;
                std::istringstream iss2(playlistContent);
                bool inStreamInfo = false;
                VariantInfo currentVariant;
                
                while (std::getline(iss2, line)) {
                    line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
                        return c == '\r' || c == '\n'; 
                    }), line.end());
                    
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
                        currentVariant.url = resolveRelativeUrl(url_, line);
                        variants.push_back(currentVariant);
                    }
                }
                
                // Sélectionner la variante avec le plus grand débit
                if (!variants.empty()) {
                    std::sort(variants.begin(), variants.end(), 
                             [](const VariantInfo& a, const VariantInfo& b) {
                                 return a.bandwidth > b.bandwidth;
                             });
                    
                    const auto& bestVariant = variants[0];
                    bestVariantUrl = bestVariant.url;
                    bestBandwidth = bestVariant.bandwidth;
                    bestCodecs = bestVariant.codecs;
                    bestWidth = bestVariant.width;
                    bestHeight = bestVariant.height;
                    
                    spdlog::info("Variante forcée avec le plus grand débit: {}x{}, {}kbps, codecs: {}, URL: {}",
                               bestWidth, bestHeight, bestBandwidth / 1000, bestCodecs, bestVariantUrl);
                }
            } else {
                // C'est une playlist média, pas une master playlist
                // Essayer d'extraire les informations du flux directement
                ret = avformat_find_stream_info(ctx, nullptr);
                if (ret >= 0) {
                    for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
                        AVStream* stream = ctx->streams[i];
                        
                        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                            bestWidth = stream->codecpar->width;
                            bestHeight = stream->codecpar->height;
                            
                            // Estimation du débit basée sur les propriétés du flux
                            if (stream->codecpar->bit_rate > 0) {
                                bestBandwidth = stream->codecpar->bit_rate;
                            } else if (ctx->bit_rate > 0) {
                                bestBandwidth = ctx->bit_rate;
                            } else {
                                // Estimation par défaut
                                bestBandwidth = 1000000; // 1 Mbps
                            }
                            
                            const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
                            if (codec) {
                                bestCodecs = codec->name;
                            }
                            
                            break;
                        }
                    }
                    
                    spdlog::info("Flux média forcé: {}x{}, {}kbps, codecs: {}", 
                               bestWidth, bestHeight, bestBandwidth / 1000, bestCodecs);
                }
            }
        }
        
        avio_seek(ioCtx, pos, SEEK_SET);
    }
    
    avformat_close_input(&ctx);
    
    // Mettre à jour les informations du flux
    streamInfo_.url = bestVariantUrl;
    streamInfo_.bandwidth = bestBandwidth;
    streamInfo_.codecs = bestCodecs;
    streamInfo_.width = bestWidth;
    streamInfo_.height = bestHeight;
    streamInfo_.hasMPEGTSSegments = true;  // Forcer l'acceptation
    
    // Vérification des valeurs d'initialisation
    if (streamInfo_.width == 0 || streamInfo_.height == 0) {
        spdlog::warn("Résolution non détectée, utilisation de valeurs par défaut (1280x720)");
        streamInfo_.width = 1280;
        streamInfo_.height = 720;
    }
    
    if (streamInfo_.bandwidth == 0) {
        spdlog::warn("Débit non détecté, utilisation d'une valeur par défaut (2 Mbps)");
        streamInfo_.bandwidth = 2000000; // 2 Mbps
    }
    
    if (streamInfo_.codecs.empty()) {
        spdlog::warn("Codecs non détectés, utilisation d'une valeur par défaut (h264/aac)");
        streamInfo_.codecs = "h264,aac";
    }
    
    spdlog::info("Flux HLS forcé comme compatible MPEG-TS: {}x{}, {}kbps, codecs: {}, URL: {}", 
               streamInfo_.width, streamInfo_.height, streamInfo_.bandwidth / 1000, 
               streamInfo_.codecs, streamInfo_.url);
    
    return true;
}


void HLSClient::extractSegmentDurations(const std::string& playlistContent) {
    std::istringstream iss(playlistContent);
    std::string line;
    double currentDuration = 0.0;
    int seqNumber = 0;
    double totalDuration = 0.0;
    int segmentCount = 0;
    
    spdlog::info("Extraction des durées de segment depuis la playlist");
    spdlog::debug("Contenu de la playlist pour extraction (premiers 500 caractères): {}", 
                playlistContent.substr(0, std::min(static_cast<size_t>(500), playlistContent.size())));
    
    while (std::getline(iss, line)) {
        // Supprimer les retours à la ligne
        line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
            return c == '\r' || c == '\n'; 
        }), line.end());
        
        spdlog::debug("Analyse de la ligne pour durée: {}", line);
        
        // Rechercher les directives EXTINF (durée)
        if (line.find("#EXTINF:") != std::string::npos) {
            // Extraire la durée avec une regex plus robuste
            std::regex duration_regex("#EXTINF:([0-9.]+)");
            std::smatch duration_match;
            
            if (std::regex_search(line, duration_match, duration_regex)) {
                try {
                    currentDuration = std::stod(duration_match[1]);
                    spdlog::info("Durée extraite avec regex: {:.2f}s", currentDuration);
                } catch (const std::exception& e) {
                    spdlog::warn("Impossible de convertir la durée '{}': {}", duration_match[1].str(), e.what());
                    currentDuration = 0.0;
                }
            } else {
                // Méthode alternative si la regex échoue
                size_t colonPos = line.find(':');
                size_t commaPos = line.find(',');
                
                if (colonPos != std::string::npos) {
                    size_t endPos = commaPos != std::string::npos ? commaPos : line.length();
                    std::string durationStr = line.substr(colonPos + 1, endPos - colonPos - 1);
                    
                    // Nettoyer la chaîne
                    durationStr.erase(std::remove_if(durationStr.begin(), durationStr.end(), 
                                                   [](char c) { return std::isspace(c); }), 
                                    durationStr.end());
                    
                    try {
                        currentDuration = std::stod(durationStr);
                        spdlog::info("Durée extraite avec méthode alternative: {:.2f}s", currentDuration);
                    } catch (const std::exception& e) {
                        spdlog::warn("Impossible de convertir la durée '{}': {}", durationStr, e.what());
                        currentDuration = 0.0;
                    }
                }
            }
        }
        // Rechercher les lignes de segments (non commentaires)
        else if (!line.empty() && line[0] != '#') {
            // C'est une ligne de segment, associer la durée au numéro de séquence
            if (currentDuration > 0.0) {
                segmentDurations_[seqNumber] = currentDuration;
                totalDuration += currentDuration;
                segmentCount++;
                
                spdlog::info("Segment {} a une durée de {:.2f}s", seqNumber, currentDuration);
                seqNumber++;
            } else {
                spdlog::warn("Segment sans durée trouvé: {}", line);
                // Associer une durée par défaut pour ne pas bloquer
                segmentDurations_[seqNumber] = 4.0;
                totalDuration += 4.0;
                segmentCount++;
                seqNumber++;
            }
            currentDuration = 0.0;
        }
    }
    
    // Calculer la durée moyenne des segments
    if (segmentCount > 0) {
        averageSegmentDuration_ = totalDuration / segmentCount;
        spdlog::info("Durée moyenne des segments: {:.2f}s (total: {:.2f}s, count: {})", 
                   averageSegmentDuration_, totalDuration, segmentCount);
    } else {
        // Valeur par défaut si aucun segment n'a été trouvé
        averageSegmentDuration_ = 4.0;
        spdlog::warn("Aucune durée de segment extraite, utilisation de la valeur par défaut: {:.2f}s", 
                   averageSegmentDuration_);
    }
    
    // Journaliser les durées de segment extraites
    spdlog::info("Extrait {} durées de segment", segmentDurations_.size());
    for (const auto& [seq, duration] : segmentDurations_) {
        spdlog::debug("  Segment {}: {:.2f}s", seq, duration);
    }
}

void HLSClient::fetchThreadFunc() {
    static int loopCount = 0;
    spdlog::info("Thread de récupération HLS démarré pour l'URL: {}", streamInfo_.url);
    
    if (!formatContext_) {
        throw std::runtime_error("Contexte de format FFmpeg non initialisé");
    }
    
    // Variables pour la gestion des discontinuités
    bool previousWasDiscontinuity = false;
    int sequenceNumber = 0;
    
    // Constante pour la taille maximale de la file d'attente
    const size_t MAX_QUEUE_SIZE = 3;  // Maximum 3 segments en file d'attente
    
    // Boucle principale de récupération des segments
    while (running_) {
        try {
            if (++loopCount % 100 == 0) {
                spdlog::info("HLSClient: fetchThreadFunc() itération {}", loopCount);
            }
            
            // Limiter la taille de la file d'attente pour éviter d'utiliser trop de mémoire
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                
                if (segmentQueue_.size() >= MAX_QUEUE_SIZE) {
                    // File d'attente suffisamment remplie, attendre un peu
                    spdlog::debug("File d'attente HLS pleine ({}/{}), attente...", 
                                segmentQueue_.size(), MAX_QUEUE_SIZE);
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
            }
            
            // Lire un paquet
            AVPacket* packet = av_packet_alloc();
            
            int ret = av_read_frame(formatContext_, packet);

            static int packetCount = 0;
            packetCount++;
            if (packetCount % 1000 == 0) {
                spdlog::info("HLS fetch thread processed {} packets so far", packetCount);
            }

            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    // Fin du flux, attendre un peu et réessayer
                    av_packet_free(&packet);
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
                    
                    av_packet_free(&packet);
                    break;
                }
            }
            
            // Vérifier si c'est un nouveau segment
            if (packet->flags & AV_PKT_FLAG_KEY && packet->pos == 0) {
                // C'est probablement le début d'un nouveau segment
                spdlog::info("Nouveau segment HLS détecté");
                
                // Accumuler les données du segment
                std::vector<uint8_t> segmentData;
                bool isDiscontinuity = previousWasDiscontinuity;
                previousWasDiscontinuity = false;
                
                // Ajouter le paquet actuel
                segmentData.insert(segmentData.end(), packet->data, packet->data + packet->size);
                
                // Lire les paquets suivants jusqu'au prochain segment
                bool endOfSegment = false;
                
                while (!endOfSegment && running_) {
                    AVPacket* nextPacket = av_packet_alloc();
                    
                    ret = av_read_frame(formatContext_, nextPacket);
                    
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
                    else if (nextPacket->flags & AV_PKT_FLAG_KEY && nextPacket->pos == 0) {
                        // Début d'un nouveau segment
                        endOfSegment = true;
                        
                        // Vérifier si c'est une discontinuité
                        if (nextPacket->flags & AV_PKT_FLAG_DISCARD) {
                            previousWasDiscontinuity = true;
                        }
                        
                        // Remettre ce paquet dans le flux
                        av_seek_frame(formatContext_, -1, nextPacket->pts, AVSEEK_FLAG_BACKWARD);
                    }
                    else {
                        // Ajouter le paquet au segment
                        segmentData.insert(segmentData.end(), nextPacket->data, nextPacket->data + nextPacket->size);
                    }
                    
                    av_packet_free(&nextPacket);
                }
                
                // Créer un segment HLS
                if (!segmentData.empty()) {
                    // Log pour le débogage
                    spdlog::info("HLSClient: Création d'un segment - taille: {} octets, séquence: {}", 
                              segmentData.size(), sequenceNumber);
                    
                    HLSSegment segment;
                    segment.data = std::move(segmentData);
                    segment.discontinuity = isDiscontinuity;
                    segment.sequenceNumber = sequenceNumber;
                    
                    // Utiliser la durée stockée ou une valeur par défaut
                    auto it = segmentDurations_.find(sequenceNumber);
                    if (it != segmentDurations_.end()) {
                        segment.duration = it->second;
                        spdlog::debug("Utilisation de la durée extraite pour le segment {}: {:.2f}s", 
                                   sequenceNumber, segment.duration);
                    } else {
                        // Si pas de durée stockée, utiliser la moyenne ou une valeur par défaut
                        segment.duration = (averageSegmentDuration_ > 0.0) ? 
                                         averageSegmentDuration_ : 4.0;
                        spdlog::debug("Utilisation de la durée moyenne pour le segment {}: {:.2f}s", 
                                   sequenceNumber, segment.duration);
                    }
                    
                    sequenceNumber++;  // Incrémenter après utilisation
                    
                    segment.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                    
                    // AJOUTER CE LOG AVANT l'ajout à la file
                    spdlog::info("HLSClient: Tentative d'ajout du segment à la file, taille: {} octets", segment.data.size());
                    
                    // Ajouter le segment à la file d'attente en respectant la taille maximale
                    {
                        std::lock_guard<std::mutex> lock(queueMutex_);
                        
                        // Limiter la taille de la file d'attente
                        while (segmentQueue_.size() >= MAX_QUEUE_SIZE) {
                            // Si la file est pleine, supprimer le segment le plus ancien
                            spdlog::warn("File d'attente pleine ({}/{}), suppression du segment le plus ancien", 
                                      segmentQueue_.size(), MAX_QUEUE_SIZE);
                            segmentQueue_.pop();
                        }
                        
                        segmentQueue_.push(segment);
                        
                        // AJOUTER CE LOG APRÈS l'ajout à la file
                        spdlog::info("HLSClient: Segment ajouté à la file, taille actuelle: {}, séquence: {}", 
                                  segmentQueue_.size(), segment.sequenceNumber);
                    }
                    
                    // Notifier les threads en attente
                    queueCondVar_.notify_all();
                    
                    spdlog::info("Segment {} ajouté à la file, taille de la file: {}, taille des données: {} octets, durée: {:.2f}s", 
                               segment.sequenceNumber, segmentQueue_.size(), segment.data.size(), segment.duration);
                } else {
                    spdlog::warn("Segment HLS vide détecté et ignoré");
                }
            }
            
            av_packet_free(&packet);
        }
        catch (const std::exception& e) {
            spdlog::error("Exception dans le thread de récupération HLS: {}", e.what());
            
            AlertManager::getInstance().addAlert(
                AlertLevel::ERROR,
                "HLSClient",
                std::string("Exception dans le thread de récupération HLS: ") + e.what(),
                true
            );
            
            // Attendre un peu avant de réessayer
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    
    spdlog::info("Thread de récupération des segments HLS terminé");
}



bool HLSClient::refreshPlaylist() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!running_) {
        spdlog::warn("Tentative de rafraîchir la playlist alors que le client HLS n'est pas en cours d'exécution");
        return false;
    }
    
    try {
        spdlog::info("Rafraîchissement de la playlist HLS: {}", streamInfo_.url);
        
        // Récupérer la playlist avec curl (plus fiable que FFmpeg pour ce cas d'usage)
        std::string playlistContent;
        if (!fetchHLSManifestWithCurl(streamInfo_.url, playlistContent)) {
            spdlog::error("Impossible de récupérer la playlist pour rafraîchissement");
            return false;
        }
        
        // Extraire les durées des segments
        if (!playlistContent.empty()) {
            extractSegmentDurations(playlistContent);
            return true;
        }
        
        return false;
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors du rafraîchissement de la playlist: {}", e.what());
        return false;
    }
}


std::optional<HLSSegment> HLSClient::getNextSegment() {
    std::unique_lock<std::mutex> lock(queueMutex_);
    
    // Ajoutez ce log pour voir si la méthode est appelée
    static int callCount = 0;
    spdlog::info("getNextSegment() appelé ({}), taille de la file: {}, thread running: {}", 
               ++callCount, segmentQueue_.size(), running_ ? "oui" : "non");
    
    static int emptyCount = 0;
    static auto lastLogTime = std::chrono::steady_clock::now();
    
    if (segmentQueue_.empty()) {
        // Ne pas spammer les logs, mais journaliser périodiquement
        auto now = std::chrono::steady_clock::now();
        if (++emptyCount % 10 == 0 || 
            std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count() >= 5) {
            spdlog::info("File d'attente des segments vide (appel #{}) - Thread running: {}, Segments traités: {}, Discontinuités: {}", 
                       emptyCount, running_ ? "oui" : "non", segmentsProcessed_, discontinuitiesDetected_.load());
            lastLogTime = now;
        }
        return std::nullopt;
    }
    
    // Réinitialiser le compteur si un segment est trouvé
    emptyCount = 0;
    lastLogTime = std::chrono::steady_clock::now();

    // Prendre simplement le premier segment disponible (FIFO)
    HLSSegment segment = segmentQueue_.front();
    segmentQueue_.pop();
    
    // Incrémenter le compteur de segments traités
    segmentsProcessed_++;
    
    // Si c'est une discontinuité, incrémenter le compteur
    if (segment.discontinuity) {
        discontinuitiesDetected_++;
    }
    
    spdlog::info("Segment {} récupéré, taille: {} octets, durée: {:.2f}s, discontinuité: {}", 
               segment.sequenceNumber, segment.data.size(), segment.duration, 
               segment.discontinuity ? "oui" : "non");
    
    return segment;
}



bool HLSClient::checkForDiscontinuities(const std::string& url) {
    try {
        spdlog::info("Vérification des discontinuités dans la playlist: {}", url);
        
        // Tentative d'ouverture de la playlist pour analyse
        std::string playlistContent;
        
        // Utiliser FFmpeg pour récupérer la playlist
        AVFormatContext* tempCtx = nullptr;
        AVIOContext* ioCtx = nullptr;
        
        AVDictionary* options = createFFmpegOptions();
        int ret = avformat_open_input(&tempCtx, url.c_str(), nullptr, &options);
        av_dict_free(&options);
        
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            spdlog::error("Impossible d'ouvrir la playlist pour vérification des discontinuités: {}", errbuf);
            
            // Essayer avec curl comme méthode alternative
            spdlog::info("Tentative de récupération de la playlist avec curl");
            if (!fetchHLSManifestWithCurl(url, playlistContent)) {
                spdlog::error("Impossible de récupérer la playlist avec curl");
                return false;
            }
        } else {
            // Obtenir le contexte I/O
            ioCtx = tempCtx->pb;
            if (!ioCtx) {
                avformat_close_input(&tempCtx);
                spdlog::error("Contexte I/O non disponible");
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
                    spdlog::info("Contenu de la playlist récupéré, taille: {} octets", playlistContent.size());
                } else {
                    spdlog::warn("Impossible de lire le contenu de la playlist: {}", bytesRead);
                }
            } else {
                spdlog::warn("Taille de la playlist non disponible ou nulle");
            }
            
            // Restaurer la position
            avio_seek(ioCtx, pos, SEEK_SET);
            
            // Fermer le contexte temporaire
            avformat_close_input(&tempCtx);
        }
        
        // Extraire les durées des segments
        if (!playlistContent.empty()) {
            extractSegmentDurations(playlistContent);
        } else {
            spdlog::error("Contenu de la playlist vide, impossible d'extraire les durées des segments");
            return false;
        }
        
        // Analyser la playlist pour trouver les marqueurs de discontinuité
        std::istringstream iss(playlistContent);
        std::string line;
        bool foundDiscontinuity = false;
        
        spdlog::info("Analyse de la playlist pour les marqueurs de discontinuité");
        
        while (std::getline(iss, line)) {
            // Normaliser la ligne
            line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
                return c == '\r' || c == '\n'; 
            }), line.end());
            
            spdlog::debug("Analyse ligne pour discontinuité: {}", line);
            
            // Vérifier s'il y a un marqueur de discontinuité
            if (line.find("#EXT-X-DISCONTINUITY") != std::string::npos ||
                line.find("#EXT-X-DISCONTINUITY-SEQUENCE") != std::string::npos) {
                foundDiscontinuity = true;
                spdlog::info("Marqueur de discontinuité trouvé dans la playlist HLS: {}", line);
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::INFO,
                    "HLSClient",
                    "Marqueur de discontinuité trouvé dans la playlist HLS",
                    false
                );
                
                // Continuer à analyser pour trouver d'autres marqueurs
            }
        }
        
        // Deuxième passe pour vérifier les discontinuités basées sur les séquences
        iss.clear();
        iss.seekg(0);
        int lastSequence = -1;
        int expectedSequence = -1;
        
        while (std::getline(iss, line)) {
            line.erase(std::remove_if(line.begin(), line.end(), [](char c) { 
                return c == '\r' || c == '\n'; 
            }), line.end());
            
            // Chercher les numéros de séquence
            if (line.find("#EXT-X-MEDIA-SEQUENCE:") != std::string::npos) {
                std::regex seq_regex("#EXT-X-MEDIA-SEQUENCE:(\\d+)");
                std::smatch seq_match;
                
                if (std::regex_search(line, seq_match, seq_regex)) {
                    int sequence = std::stoi(seq_match[1]);
                    
                    if (lastSequence != -1) {
                        expectedSequence = lastSequence + 1;
                        if (sequence != expectedSequence) {
                            spdlog::info("Discontinuité de séquence détectée: attendu {}, reçu {}", 
                                       expectedSequence, sequence);
                            foundDiscontinuity = true;
                        }
                    }
                    
                    lastSequence = sequence;
                }
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

bool HLSClient::isRunning() const {
    return running_;
}

HLSClient::~HLSClient() {
    if (running_) {
        stop();
    }
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
