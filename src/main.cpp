#include <iostream>
#include <string>
#include <signal.h>
#include <thread>
#include <chrono>
#include <filesystem>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "core/config.h"
#include "core/StreamManager.h"
#include "alerting/AlertManager.h"
#include "web/WebServer.h"

using namespace hls_to_dvb;

// Pour la gestion des signaux
volatile sig_atomic_t g_running = 1;

void signalHandler(int signum) {
    spdlog::info("Signal reçu: {}", signum);
    g_running = 0;
}

// Fonction pour vérifier les capacités FFmpeg
void checkFFmpegCapabilities() {
    spdlog::info("Vérification des capacités FFmpeg:");
    
    // Vérifier la version
    spdlog::info("  Version FFmpeg: {}", avutil_version() ? av_version_info() : "Information non disponible");
    
    // Vérifier les formats supportés
    void* opaque = nullptr;
    const AVInputFormat* input_format;
    bool has_hls = false;
    bool has_applehttp = false;
    
    while ((input_format = av_demuxer_iterate(&opaque))) {
        if (input_format->name) {
            if (strcmp(input_format->name, "hls") == 0) {
                has_hls = true;
                spdlog::info("  Format HLS supporté: Oui (via démuxeur 'hls')");
            }
            else if (strcmp(input_format->name, "applehttp") == 0) {
                has_applehttp = true;
                spdlog::info("  Format HLS supporté: Oui (via démuxeur 'applehttp')");
            }
        }
    }
    
    if (!has_hls && !has_applehttp) {
        spdlog::error("  Format HLS supporté: Non - FFmpeg ne semble pas supporter le format HLS!");
        spdlog::error("  Cela peut causer des problèmes dans la récupération des segments HLS.");
    }
    
    // Vérifier les protocoles supportés
    opaque = nullptr;
    const char* protocol_name;
    bool has_http = false;
    bool has_https = false;
    
    while ((protocol_name = avio_enum_protocols(&opaque, 0))) {
        if (strcmp(protocol_name, "http") == 0) {
            has_http = true;
            spdlog::info("  Protocole HTTP supporté: Oui");
        }
        else if (strcmp(protocol_name, "https") == 0) {
            has_https = true;
            spdlog::info("  Protocole HTTPS supporté: Oui");
        }
    }
    
    if (!has_http) {
        spdlog::error("  Protocole HTTP supporté: Non - FFmpeg ne supporte pas HTTP!");
        spdlog::error("  Cela peut empêcher d'accéder aux flux HLS via HTTP.");
    }
    
    if (!has_https) {
        spdlog::error("  Protocole HTTPS supporté: Non - FFmpeg ne supporte pas HTTPS!");
        spdlog::error("  Cela peut empêcher d'accéder aux flux HLS via HTTPS.");
    }
    
    // Vérifier si FFmpeg a été compilé avec le support TLS/SSL
    #ifdef CONFIG_OPENSSL
        spdlog::info("  Support SSL/TLS: Oui (via OpenSSL)");
    #elif defined(CONFIG_GNUTLS)
        spdlog::info("  Support SSL/TLS: Oui (via GnuTLS)");
    #else
        spdlog::warn("  Support SSL/TLS: Incertain - Dépend de la compilation de FFmpeg");
    #endif
    
    spdlog::info("Fin de la vérification des capacités FFmpeg");
}

bool testHLSUrl(const std::string& url) {
    spdlog::info("Test d'accessibilité de l'URL HLS: {}", url);
    
    if (url.empty()) {
        spdlog::error("L'URL est vide!");
        return false;
    }
    
    // Vérifier que l'URL commence par http:// ou https://
    if (url.find("http://") != 0 && url.find("https://") != 0) {
        spdlog::warn("L'URL ne commence pas par http:// ou https:// - cela peut causer des problèmes");
    }
    
    AVFormatContext* ctx = nullptr;
    AVDictionary* options = nullptr;
    
    // Configurer le timeout pour ne pas bloquer trop longtemps
    av_dict_set(&options, "timeout", "5000000", 0);     // 5 secondes en microsecondes
    av_dict_set(&options, "http_persistent", "0", 0);   // Désactiver les connexions persistantes
    av_dict_set(&options, "icy", "0", 0);               // Désactiver les métadonnées ICY
    av_dict_set(&options, "reconnect", "1", 0);         // Autoriser les reconnexions
    
    // Tenter d'ouvrir l'URL
    int ret = avformat_open_input(&ctx, url.c_str(), nullptr, &options);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Impossible d'accéder à l'URL HLS {}: {}", url, errbuf);
        av_dict_free(&options);
        return false;
    }
    
    // Tenter de récupérer les informations sur le flux
    ret = avformat_find_stream_info(ctx, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Erreur lors de la récupération des informations sur le flux: {}", errbuf);
        avformat_close_input(&ctx);
        av_dict_free(&options);
        return false;
    }
    
    // Vérifier le format du flux
    spdlog::info("Format du flux: {}", ctx->iformat ? ctx->iformat->name : "inconnu");
    
    // Vérifier s'il s'agit d'un flux HLS
    bool isHLS = false;
    if (ctx->iformat && (
        strcmp(ctx->iformat->name, "hls") == 0 || 
        strcmp(ctx->iformat->name, "applehttp") == 0)) {
        isHLS = true;
        spdlog::info("Le flux est bien au format HLS");
    } else {
        spdlog::warn("Le flux ne semble pas être au format HLS!");
    }
    
    // Afficher des informations sur les flux
    spdlog::info("Nombre de flux: {}", ctx->nb_streams);
    for (unsigned int i = 0; i < ctx->nb_streams; i++) {
        AVStream* stream = ctx->streams[i];
        const char* typeName = "inconnu";
        
        switch(stream->codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO: typeName = "video"; break;
            case AVMEDIA_TYPE_AUDIO: typeName = "audio"; break;
            case AVMEDIA_TYPE_SUBTITLE: typeName = "sous-titres"; break;
            default: break;
        }
        
        // Convertir AVCodecID en entier pour éviter les problèmes de formatage
        int codecId = static_cast<int>(stream->codecpar->codec_id);
        spdlog::info("  Flux #{}: Type: {}, Codec ID: {}", i, typeName, codecId);
    }
    
    // Libérer les ressources
    avformat_close_input(&ctx);
    av_dict_free(&options);
    
    spdlog::info("URL HLS accessible: {}", url);
    return isHLS;
}

// Point d'entrée principal
int main(int argc, char* argv[]) {
    // Vérifier les arguments
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return 1;
    }
    
    std::string configFile = argv[1];
    
    // Configurer le gestionnaire de signaux
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    try {
        // S'assurer que le répertoire logs existe
        std::filesystem::create_directories("logs");
        
        // Initialiser le logger
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::info);
        
        // Créer un logger avec plusieurs sinks
        std::vector<spdlog::sink_ptr> sinks {console_sink};
        
        // Ajouter un sink de fichier si le dossier logs existe
        try {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                "logs/hls-to-dvb.log", 10 * 1024 * 1024, 5);
            file_sink->set_level(spdlog::level::debug);
            sinks.push_back(file_sink);
        } catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Erreur lors de la création du fichier de log: " << ex.what() << std::endl;
            std::cerr << "Les logs seront uniquement affichés sur la console" << std::endl;
        }
        
        auto logger = std::make_shared<spdlog::logger>("main", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::debug);
        spdlog::set_default_logger(logger);
        
        // Afficher le message de démarrage
        spdlog::info("Démarrage du convertisseur HLS vers MPEG-TS DVB");
        spdlog::info("configFile: {}", configFile);
        
        // Vérifier les capacités FFmpeg
        checkFFmpegCapabilities();

        // Charger la configuration
        Config config(configFile);
        if (!config.load()) {
            spdlog::error("Erreur lors du chargement de la configuration: {}", configFile);
            return 1;
        }
        spdlog::info("Configuration chargée avec succès");

        // Vérifier les flux configurés
        auto streamConfigs = config.getStreamConfigs();
        if (streamConfigs.empty()) {
            spdlog::error("ATTENTION: Aucun flux configuré! Vérifiez le fichier de configuration.");
            // Optionnel: Vous pouvez quitter ici si aucun flux n'est configuré
            // return 1;
        } else {
            spdlog::info("{} flux configurés dans le fichier de configuration:", streamConfigs.size());
            for (const auto& streamConfig : streamConfigs) {
                spdlog::info("  - Flux {}: {} -> {} (port {})", 
                        streamConfig.id, streamConfig.hlsInput, 
                        streamConfig.mcastOutput, streamConfig.mcastPort);
                
                if (streamConfig.hlsInput.empty()) {
                    spdlog::error("ERREUR: URL HLS vide pour le flux {}", streamConfig.id);
                    continue;
                }
                
                if (streamConfig.mcastOutput.empty() || streamConfig.mcastPort <= 0) {
                    spdlog::error("ERREUR: Configuration multicast invalide pour le flux {}", streamConfig.id);
                    continue;
                }
                
                // Tester l'accessibilité de l'URL HLS
                if (!testHLSUrl(streamConfig.hlsInput)) {
                    spdlog::error("ERREUR: URL HLS inaccessible ou invalide: {}. Le flux {} pourrait ne pas fonctionner.", 
                                streamConfig.hlsInput, streamConfig.id);
                    
                    // Ajoutez une alerte ici
                    AlertManager::getInstance().addAlert(
                        AlertLevel::ERROR,
                        "Configuration",
                        "URL HLS inaccessible ou invalide pour le flux " + streamConfig.id + ": " + streamConfig.hlsInput,
                        true
                    );
                }
            }
        }

        const auto& loggingConfig = config.getLoggingConfig();
        if (loggingConfig.level == "debug") {
            spdlog::set_level(spdlog::level::debug);
        } else if (loggingConfig.level == "info") {
            spdlog::set_level(spdlog::level::info);
        } else if (loggingConfig.level == "warning") {
            spdlog::set_level(spdlog::level::warn);
        } else if (loggingConfig.level == "error") {
            spdlog::set_level(spdlog::level::err);
        }

        // Initialiser le gestionnaire d'alertes
        const auto& alertsConfig = config.getAlertRetention();
        
        AlertManager::getInstance().setRetention(
            AlertLevel::INFO,
            alertsConfig.info
        );
        AlertManager::getInstance().setRetention(
            AlertLevel::WARNING,
            alertsConfig.warning
        );
        AlertManager::getInstance().setRetention(
            AlertLevel::ERROR, 
            alertsConfig.error
        );
        
        AlertManager::getInstance().addAlert(
            AlertLevel::INFO,
            "System",
            "Application started",
            false
        );
        
        spdlog::info("Gestionnaire d'alertes initialisé");
        
        // Créer le gestionnaire de flux qui va gérer la conversion HLS -> MPEG-TS et la diffusion multicast
        spdlog::info("Initialisation du gestionnaire de flux");
        StreamManager streamManager(&config);
        
        // Démarrer le gestionnaire de flux
        // Cette méthode va initialiser et démarrer tous les flux configurés
        // y compris la création des instances de MPEGTSConverter et MulticastSender
        spdlog::info("Démarrage du gestionnaire de flux");
        //spdlog::error("*** VÉRIFICATION: streamId={}, config existe? {} ***", 
        //    streamConfig.id, 
        //    config_->getStreamConfig(streamConfig.id) ? "OUI" : "NON");
        streamManager.start();
        spdlog::info("Gestionnaire de flux démarré avec succès");
        
        // Créer et démarrer le serveur web
        spdlog::info("Initialisation du serveur web avec le répertoire: web");
        WebServer webServer(config, streamManager, "web");

        spdlog::info("Tentative de démarrage du serveur web...");
        bool webServerStarted = webServer.start();
        spdlog::info("Résultat du démarrage du serveur web: {}", webServerStarted ? "Succès" : "Échec");

        if (!webServerStarted) {
            spdlog::error("Erreur lors du démarrage du serveur web");
            // Ne pas quitter l'application, mais continuer sans serveur web
        }
        
        // Boucle principale
        spdlog::info("Application démarrée et en cours d'exécution");
        
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        // Arrêt propre
        spdlog::info("Arrêt de l'application...");
        
        // Arrêter le serveur web
        webServer.stop();
        
        // Arrêter tous les flux
        // Cette méthode va correctement arrêter tous les MPEGTSConverter et MulticastSender
        streamManager.stop();
        
        // Ajouter une alerte de fin
        AlertManager::getInstance().addAlert(
            AlertLevel::INFO,
            "System",
            "Application stopped",
            false
        );
        
        spdlog::info("Application arrêtée");
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Exception non gérée: " << e.what() << std::endl;
        return 1;
    }
}