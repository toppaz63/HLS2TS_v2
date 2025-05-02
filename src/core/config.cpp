#include "core/config.h"
#include <fstream>
#include <iostream>
#include <spdlog/spdlog.h>
#include <filesystem>

namespace hls_to_dvb {

Config::Config(const std::string& configPath)
    : configPath_(configPath) {
    // Initialiser avec des valeurs par défaut
}

bool Config::load() {
    return loadFromFile(configPath_);
}

bool Config::loadFromFile(const std::string& configPath) {
    try {
        std::ifstream configFile(configPath);
        if (!configFile.is_open()) {
            spdlog::error("Impossible d'ouvrir le fichier de configuration: {}", configPath);
            return false;
        }
        
        nlohmann::json json;
        configFile >> json;
        
        // Charger la configuration du serveur
        if (json.contains("server")) {
            const auto& serverJson = json["server"];
            if (serverJson.contains("address")) {
                server_.address = serverJson["address"].get<std::string>();
            }
            if (serverJson.contains("port")) {
                server_.port = serverJson["port"].get<int>();
            }
            if (serverJson.contains("workerThreads")) {
                server_.workerThreads = serverJson["workerThreads"].get<int>();
            }
        }
        
        // Charger la configuration de journalisation
        if (json.contains("logging")) {
            const auto& loggingJson = json["logging"];
            if (loggingJson.contains("level")) {
                logging_.level = loggingJson["level"].get<std::string>();
            }
            if (loggingJson.contains("console")) {
                logging_.console = loggingJson["console"].get<bool>();
            }
            if (loggingJson.contains("file")) {
                const auto& fileJson = loggingJson["file"];
                if (fileJson.contains("enabled")) {
                    logging_.file.enabled = fileJson["enabled"].get<bool>();
                }
                if (fileJson.contains("path")) {
                    logging_.file.path = fileJson["path"].get<std::string>();
                }
                if (fileJson.contains("rotationSize")) {
                    logging_.file.rotationSize = fileJson["rotationSize"].get<size_t>();
                }
                if (fileJson.contains("maxFiles")) {
                    logging_.file.maxFiles = fileJson["maxFiles"].get<int>();
                }
            }
        }
        
        // Charger la configuration des alertes
        if (json.contains("alerts")) {
            const auto& alertsJson = json["alerts"];
            if (alertsJson.contains("retention")) {
                const auto& retentionJson = alertsJson["retention"];
                if (retentionJson.contains("info")) {
                    alerts_.retention.info = retentionJson["info"].get<int>();
                }
                if (retentionJson.contains("warning")) {
                    alerts_.retention.warning = retentionJson["warning"].get<int>();
                }
                if (retentionJson.contains("error")) {
                    alerts_.retention.error = retentionJson["error"].get<int>();
                }
            }
            
            // Vous pouvez également charger la configuration des notifications ici
        }
        
        // Charger les configurations de flux
        if (json.contains("streams") && json["streams"].is_array()) {
            streams_.clear();
            streamIndexMap_.clear();
            
            for (const auto& streamJson : json["streams"]) {
                StreamConfig streamConfig;
                
                if (streamJson.contains("id")) {
                    streamConfig.id = streamJson["id"].get<std::string>();
                } else {
                    spdlog::warn("Flux sans identifiant trouvé dans la configuration, ignoré");
                    continue;
                }
                
                if (streamJson.contains("name")) {
                    streamConfig.name = streamJson["name"].get<std::string>();
                }
                
                if (streamJson.contains("hlsInput")) {
                    streamConfig.hlsInput = streamJson["hlsInput"].get<std::string>();
                }
                
                if (streamJson.contains("multicastOutput")) {
                    streamConfig.mcastOutput = streamJson["multicastOutput"].get<std::string>();
                }
                
                if (streamJson.contains("multicastPort")) {
                    streamConfig.mcastPort = streamJson["multicastPort"].get<int>();
                }
                
                if (streamJson.contains("bufferSize")) {
                    streamConfig.bufferSize = streamJson["bufferSize"].get<size_t>();
                }
                
                if (streamJson.contains("enabled")) {
                    streamConfig.enabled = streamJson["enabled"].get<bool>();
                }
                
                streamIndexMap_[streamConfig.id] = streams_.size();
                streams_.push_back(streamConfig);
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Erreur lors du chargement de la configuration: {}", e.what());
        return false;
    }
}

bool Config::loadFromString(const std::string& jsonString) {
    try {
        nlohmann::json json = nlohmann::json::parse(jsonString);
        
        // Utiliser le même code que loadFromFile, mais avec json au lieu de lire depuis un fichier
        
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Erreur lors du chargement de la configuration depuis une chaîne: {}", e.what());
        return false;
    }
}

bool Config::saveToFile(const std::string& configPath) const {
    try {
        nlohmann::json json = toJson();
        
        std::ofstream configFile(configPath);
        if (!configFile.is_open()) {
            spdlog::error("Impossible d'ouvrir le fichier pour l'écriture: {}", configPath);
            return false;
        }
        
        configFile << json.dump(4); // Indentation de 4 espaces
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Erreur lors de la sauvegarde de la configuration: {}", e.what());
        return false;
    }
}

const StreamConfig* Config::getStreamConfig(const std::string& streamId) const {
    auto it = streamIndexMap_.find(streamId);
    if (it != streamIndexMap_.end() && it->second < streams_.size()) {
        return &streams_[it->second];
    }
    return nullptr;
}

const std::vector<StreamConfig>& Config::getStreamConfigs() const {
    return streams_;
}

bool Config::updateStreamConfig(const StreamConfig& config) {
    auto it = streamIndexMap_.find(config.id);
    if (it != streamIndexMap_.end() && it->second < streams_.size()) {
        // Mettre à jour un flux existant
        streams_[it->second] = config;
    } else {
        // Ajouter un nouveau flux
        streamIndexMap_[config.id] = streams_.size();
        streams_.push_back(config);
    }
    return true;
}

bool Config::removeStreamConfig(const std::string& streamId) {
    auto it = streamIndexMap_.find(streamId);
    if (it != streamIndexMap_.end() && it->second < streams_.size()) {
        // Supprimer le flux
        streams_.erase(streams_.begin() + it->second);
        
        // Mettre à jour les indices dans la map
        streamIndexMap_.erase(it);
        for (auto& mapEntry : streamIndexMap_) {
            if (mapEntry.second > it->second) {
                mapEntry.second--;
            }
        }
        
        return true;
    }
    return false;
}

const ServerConfig& Config::getServerConfig() const {
    return server_;
}

const LoggingConfig& Config::getLoggingConfig() const {
    return logging_;
}

const AlertsConfig& Config::getAlertsConfig() const {
    return alerts_;
}

void Config::updateServerConfig(const ServerConfig& config) {
    server_ = config;
}

void Config::updateLoggingConfig(const LoggingConfig& config) {
    logging_ = config;
}

void Config::updateAlertsConfig(const AlertsConfig& config) {
    alerts_ = config;
}

const AlertsConfig::Retention& Config::getAlertRetention() const {
    return alerts_.retention;
}

nlohmann::json Config::toJson() const {
    nlohmann::json json;
    
    // Serveur
    json["server"] = {
        {"address", server_.address},
        {"port", server_.port},
        {"workerThreads", server_.workerThreads}
    };
    
    // Journalisation
    json["logging"] = {
        {"level", logging_.level},
        {"console", logging_.console},
        {"file", {
            {"enabled", logging_.file.enabled},
            {"path", logging_.file.path},
            {"rotationSize", logging_.file.rotationSize},
            {"maxFiles", logging_.file.maxFiles}
        }}
    };
    
    // Alertes
    json["alerts"] = {
        {"retention", {
            {"info", alerts_.retention.info},
            {"warning", alerts_.retention.warning},
            {"error", alerts_.retention.error}
        }}
    };
    
    // Flux
    nlohmann::json streamsJson = nlohmann::json::array();
    for (const auto& stream : streams_) {
        streamsJson.push_back({
            {"id", stream.id},
            {"name", stream.name},
            {"hlsInput", stream.hlsInput},
            {"multicastOutput", stream.mcastOutput},
            {"multicastPort", stream.mcastPort},
            {"bufferSize", stream.bufferSize},
            {"enabled", stream.enabled}
        });
    }
    json["streams"] = streamsJson;
    
    return json;
}

std::string Config::getConfigPath() const {
    return configPath_;
}

} // namespace hls_to_dvb
