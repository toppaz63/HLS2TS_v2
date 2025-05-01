#include "Config.h"
#include "../alerting/AlertManager.h"
#include "spdlog/spdlog.h"

#include <fstream>
#include <stdexcept>
#include <random>
#include <sstream>
#include <iostream>

namespace hls_to_dvb {

using json = nlohmann::json;

std::string Config::generateUniqueId() const {
    const std::string chars = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, chars.size() - 1);
    
    std::stringstream ss;
    for (int i = 0; i < 8; i++) {
        ss << chars[dis(gen)];
    }
    return ss.str();
}

Config::Config(const std::string& configPath)
    : configPath_(configPath) {
    
    // Chargement initial de la configuration
    if (!load()) {
        std::cerr << "Impossible de charger la configuration depuis " << configPath << ". Utilisation des valeurs par défaut." << std::endl;
        spdlog::error("Impossible de charger la configuration depuis {}. Utilisation des valeurs par défaut.", configPath);
        
        // Configuration par défaut
        settings_["webServerPort"] = "8080";
        settings_["logLevel"] = "info";
        
        // Essayer de sauvegarder la configuration par défaut
        save();
    }
}

bool Config::load() {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::ifstream file(configPath_);
        if (!file.is_open()) {
            std::cerr << "Impossible d'ouvrir le fichier de configuration : " << configPath_ << std::endl;
            spdlog::error("Impossible d'ouvrir le fichier de configuration : {}", configPath_);
            return false;
        }
        
        json config;
        file >> config;
        
        // Charger les paramètres généraux
        if (config.contains("settings") && config["settings"].is_object()) {
            const auto& settings = config["settings"];
            
            for (auto it = settings.begin(); it != settings.end(); ++it) {
                settings_[it.key()] = it.value().is_string() ? 
                    it.value().get<std::string>() : it.value().dump();
            }
        }
        
        // Charger les configurations de serveur
        if (config.contains("server") && config["server"].is_object()) {
            settings_["server"] = config["server"].dump();
        }
        
        // Charger les configurations de logging
        if (config.contains("logging") && config["logging"].is_object()) {
            settings_["logging"] = config["logging"].dump();
        }
        
        // Charger les configurations d'alertes
        if (config.contains("alerts") && config["alerts"].is_object()) {
            settings_["alerts"] = config["alerts"].dump();
        }
        
        // Charger les configurations de flux
        streams_.clear();
        if (config.contains("streams") && config["streams"].is_array()) {
            for (const auto& stream : config["streams"]) {
                StreamConfig streamConfig;
                
                // Utiliser .contains() et extraction typée
                streamConfig.id = stream.contains("id") ? stream["id"].get<std::string>() : generateUniqueId();
                streamConfig.name = stream.contains("name") ? stream["name"].get<std::string>() : "Unnamed Stream";
                streamConfig.hlsInput = stream.contains("hlsInput") ? stream["hlsInput"].get<std::string>() : "";
                streamConfig.multicastOutput = stream.contains("multicastOutput") ? stream["multicastOutput"].get<std::string>() : "239.0.0.1";
                streamConfig.multicastPort = stream.contains("multicastPort") ? stream["multicastPort"].get<int>() : 1234;
                streamConfig.bufferSize = stream.contains("bufferSize") ? stream["bufferSize"].get<size_t>() : 3;
                
                streams_.push_back(streamConfig);
            }
        }
        
        spdlog::info("Configuration chargée avec succès: {} flux configurés", streams_.size());
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Erreur lors du chargement de la configuration: " << e.what() << std::endl;
        spdlog::error("Erreur lors du chargement de la configuration: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "Config",
            std::string("Erreur lors du chargement de la configuration: ") + e.what(),
            true
        );
        
        return false;
    }
}

bool Config::save() {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        
        json config;
        
        // Sauvegarder les paramètres généraux
        json settingsJson;
        for (const auto& pair : settings_) {
            if (pair.first != "server" && pair.first != "logging" && pair.first != "alerts") {
                try {
                    // Essayer de parser comme JSON, sinon conserver comme chaîne
                    settingsJson[pair.first] = json::parse(pair.second);
                } catch (...) {
                    settingsJson[pair.first] = pair.second;
                }
            }
        }
        config["settings"] = settingsJson;
        
        // Sauvegarder les configurations de serveur
        auto serverIt = settings_.find("server");
        if (serverIt != settings_.end()) {
            try {
                config["server"] = json::parse(serverIt->second);
            } catch (const std::exception& e) {
                spdlog::warn("Erreur lors du parsing de la configuration du serveur: {}", e.what());
            }
        }
        
        // Sauvegarder les configurations de logging
        auto loggingIt = settings_.find("logging");
        if (loggingIt != settings_.end()) {
            try {
                config["logging"] = json::parse(loggingIt->second);
            } catch (const std::exception& e) {
                spdlog::warn("Erreur lors du parsing de la configuration de logging: {}", e.what());
            }
        }
        
        // Sauvegarder les configurations d'alertes
        auto alertsIt = settings_.find("alerts");
        if (alertsIt != settings_.end()) {
            try {
                config["alerts"] = json::parse(alertsIt->second);
            } catch (const std::exception& e) {
                spdlog::warn("Erreur lors du parsing de la configuration des alertes: {}", e.what());
            }
        }
        
        // Sauvegarder les configurations de flux
        json streams = json::array();
        for (const auto& stream : streams_) {
            json streamJson;
            streamJson["id"] = stream.id;
            streamJson["name"] = stream.name;
            streamJson["hlsInput"] = stream.hlsInput;
            streamJson["multicastOutput"] = stream.multicastOutput;
            streamJson["multicastPort"] = stream.multicastPort;
            streamJson["bufferSize"] = stream.bufferSize;
            
            streams.push_back(streamJson);
        }
        config["streams"] = streams;
        
        // Écrire dans le fichier
        std::ofstream file(configPath_);
        if (!file.is_open()) {
            std::cerr << "Impossible d'ouvrir le fichier de configuration pour écriture : " << configPath_ << std::endl;
            spdlog::error("Impossible d'ouvrir le fichier de configuration pour écriture : {}", configPath_);
            return false;
        }
        
        file << config.dump(4); // Indentation de 4 espaces pour la lisibilité
        
        spdlog::info("Configuration sauvegardée avec succès dans {}", configPath_);
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Erreur lors de la sauvegarde de la configuration: " << e.what() << std::endl;
        spdlog::error("Erreur lors de la sauvegarde de la configuration: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "Config",
            std::string("Erreur lors de la sauvegarde de la configuration: ") + e.what(),
            true
        );
        
        return false;
    }
}

std::vector<StreamConfig> Config::getStreams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_;
}

const StreamConfig* Config::getStream(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& stream : streams_) {
        if (stream.id == id) {
            return &stream;
        }
    }
    
    return nullptr;
}

bool Config::setStream(const StreamConfig& stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Si l'ID est vide, générer un nouvel ID
    std::string id = stream.id;
    if (id.empty()) {
        id = generateUniqueId();
    }
    
    // Vérifier si le flux existe déjà
    bool found = false;
    for (auto& s : streams_) {
        if (s.id == id) {
            // Mettre à jour le flux existant
            s = stream;
            s.id = id;
            found = true;
            break;
        }
    }
    
    // Ajouter un nouveau flux si non trouvé
    if (!found) {
        StreamConfig newStream = stream;
        newStream.id = id;
        streams_.push_back(newStream);
    }
    
    // Sauvegarder la configuration mise à jour
    return save();
}

bool Config::removeStream(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Rechercher et supprimer le flux
    for (auto it = streams_.begin(); it != streams_.end(); ++it) {
        if (it->id == id) {
            streams_.erase(it);
            
            // Sauvegarder la configuration mise à jour
            return save();
        }
    }
    
    // Flux non trouvé
    return false;
}

int Config::getWebServerPort() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // D'abord, essayer de récupérer depuis "settings"
        auto webServerPortIt = settings_.find("webServerPort");
        if (webServerPortIt != settings_.end()) {
            return std::stoi(webServerPortIt->second);
        }
        
        // Ensuite, essayer de récupérer depuis la structure de server
        auto serverIt = settings_.find("server");
        if (serverIt != settings_.end()) {
            json serverConfig = json::parse(serverIt->second);
            if (serverConfig.contains("port")) {
                return serverConfig["port"].get<int>();
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Erreur lors de la récupération du port du serveur web: " << e.what() << std::endl; 
        spdlog::warn("Erreur lors de la récupération du port du serveur web: {}", e.what());
    }
    
    return 8080; // Port par défaut
}

std::string Config::getLogLevel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // D'abord, essayer de récupérer depuis "settings"
    auto logLevelIt = settings_.find("logLevel");
    if (logLevelIt != settings_.end()) {
        return logLevelIt->second;
    }
    
    // Ensuite, essayer de récupérer depuis la structure de logging
    try {
        auto loggingIt = settings_.find("logging");
        if (loggingIt != settings_.end()) {
            json loggingConfig = json::parse(loggingIt->second);
            if (loggingConfig.contains("level")) {
                return loggingConfig["level"].get<std::string>();
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Erreur lors de la récupération du niveau de log: " << e.what() << std::endl;
        spdlog::warn("Erreur lors de la récupération du niveau de log: {}", e.what());
    }
    
    return "info"; // Niveau de log par défaut
}

std::string Config::getServerAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        auto serverIt = settings_.find("server");
        if (serverIt != settings_.end()) {
            json serverConfig = json::parse(serverIt->second);
            if (serverConfig.contains("address")) {
                return serverConfig["address"].get<std::string>();
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Erreur lors de la récupération de l'adresse du serveur: " << e.what() << std::endl;
        spdlog::warn("Erreur lors de la récupération de l'adresse du serveur: {}", e.what());
    }
    
    return "0.0.0.0"; // Adresse par défaut (toutes les interfaces)
}

int Config::getServerWorkerThreads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        auto serverIt = settings_.find("server");
        if (serverIt != settings_.end()) {
            json serverConfig = json::parse(serverIt->second);
            if (serverConfig.contains("workerThreads")) {
                return serverConfig["workerThreads"].get<int>();
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Erreur lors de la récupération du nombre de threads de travail: " << e.what() << std::endl;
        spdlog::warn("Erreur lors de la récupération du nombre de threads de travail: {}", e.what());
    }
    
    return 4; // Nombre de threads par défaut
}

std::map<std::string, int> Config::getAlertRetention() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::map<std::string, int> retention = {
        {"info", 7200},     // 2 heures par défaut
        {"warning", 86400}, // 24 heures par défaut
        {"error", 604800}   // 7 jours par défaut
    };
    
    try {
        auto alertsIt = settings_.find("alerts");
        if (alertsIt != settings_.end()) {
            json alertsConfig = json::parse(alertsIt->second);
            if (alertsConfig.contains("retention")) {
                json retentionConfig = alertsConfig["retention"];
                
                if (retentionConfig.contains("info")) {
                    retention["info"] = retentionConfig["info"].get<int>();
                }
                
                if (retentionConfig.contains("warning")) {
                    retention["warning"] = retentionConfig["warning"].get<int>();
                }
                
                if (retentionConfig.contains("error")) {
                    retention["error"] = retentionConfig["error"].get<int>();
                }
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Erreur lors de la récupération des durées de rétention des alertes: " << e.what() << std::endl;
        spdlog::warn("Erreur lors de la récupération des durées de rétention des alertes: {}", e.what());
    }
    
    return retention;
}

nlohmann::json Config::getLoggingConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    json defaultConfig = {
        {"level", "info"},
        {"console", true},
        {"file", {
            {"enabled", true},
            {"path", "logs/hls-to-dvb.log"},
            {"rotationSize", 10485760},
            {"maxFiles", 5}
        }}
    };
    
    try {
        auto loggingIt = settings_.find("logging");
        if (loggingIt != settings_.end()) {
            return json::parse(loggingIt->second);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Erreur lors de la récupération de la configuration de journalisation: " << e.what() << std::endl;
        spdlog::warn("Erreur lors de la récupération de la configuration de journalisation: {}", e.what());
    }
    
    return defaultConfig;
}

} // namespace hls_to_dvb
