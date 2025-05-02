
#include "mpegts/DVBProcessor.h"
#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <cstring>
#include <iostream>
// Utilisation de TSDuck pour manipuler les tables DVB
#include <tsduck/tsduck.h>

using namespace hls_to_dvb;

DVBProcessor::DVBProcessor() 
    : versionPAT_(0), versionSDT_(0), versionEIT_(0), versionNIT_(0) {
}

void DVBProcessor::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // Créer les tables PSI de base avec des valeurs par défaut
        pat_ = std::make_unique<ts::PAT>();
        pat_->ts_id = 1;
        pat_->version = versionPAT_;
        pat_->is_current = true;
        
        sdt_ = std::make_unique<ts::SDT>();
        sdt_->ts_id = 1;
        sdt_->onetw_id = 1; // Nom de propriété correct dans cette version de TSDuck
        sdt_->version = versionSDT_;
        sdt_->is_current = true;
        
        nit_ = std::make_unique<ts::NIT>();
        nit_->network_id = 1;
        nit_->version = versionNIT_;
        nit_->is_current = true;
        
        eit_ = std::make_unique<ts::EIT>();
        eit_->service_id = 0;
        eit_->ts_id = 1;
        eit_->onetw_id = 1; // Nom de propriété correct dans cette version de TSDuck
        eit_->version = versionEIT_;
        eit_->is_current = true;
        
        spdlog::info("DVBProcessor initialisé avec succès");
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de l'initialisation du DVBProcessor: {}", e.what());
        
        hls_to_dvb::AlertManager::getInstance().addAlert(
            hls_to_dvb::AlertLevel::ERROR,
            "DVBProcessor",
            std::string("Erreur lors de l'initialisation: ") + e.what(),
            true
        );
        
        throw;
    }
}

void DVBProcessor::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Libérer les ressources
    pat_.reset();
    sdt_.reset();
    eit_.reset();
    nit_.reset();
    pmts_.clear();
    services_.clear();
}

std::vector<uint8_t> DVBProcessor::updatePSITables(const std::vector<uint8_t>& data) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Si les données sont vides, retourner directement
        if (data.empty() || !pat_ || !sdt_) {
            return data;
        }
        
        // Vérifier que la taille est un multiple de TS_PACKET_SIZE
        if (data.size() % ts::PKT_SIZE != 0) {
            spdlog::warn("Taille de données non multiple de 188 octets: {}", data.size());
            return data;
        }
        
        // Analyser les PID dans le flux
        auto pids = analyzePIDs(data);
        
        // Générer les tables PSI/SI
        std::map<uint16_t, std::vector<uint8_t>> tables;
        
        // Générer la PAT (PID 0x0000)
        auto patData = generatePAT();
        if (!patData.empty()) {
            tables[0x0000] = patData;
        }
        
        // Générer la SDT (PID 0x0011)
        auto sdtData = generateSDT();
        if (!sdtData.empty()) {
            tables[0x0011] = sdtData;
        }
        
        // Générer la NIT (PID 0x0010)
        auto nitData = generateNIT();
        if (!nitData.empty()) {
            tables[0x0010] = nitData;
        }
        
        // Générer les PMT pour chaque service
        for (const auto& [serviceId, service] : services_) {
            auto pmtData = generatePMT(serviceId);
            if (!pmtData.empty()) {
                tables[service.pmtPid] = pmtData;
            }
        }
        
        // Insérer les tables dans le flux
        return insertTables(data, tables);
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de la mise à jour des tables PSI/SI: {}", e.what());
        
        hls_to_dvb::AlertManager::getInstance().addAlert(
            hls_to_dvb::AlertLevel::ERROR,
            "DVBProcessor",
            std::string("Erreur lors de la mise à jour des tables PSI/SI: ") + e.what(),
            true
        );
        
        return data;
    }
}

void DVBProcessor::setService(const DVBService& service) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Stocker le service
    services_[service.serviceId] = service;
    
    // Créer ou mettre à jour la PMT
    auto it = pmts_.find(service.serviceId);
    if (it == pmts_.end()) {
        pmts_[service.serviceId] = std::make_unique<ts::PMT>();
        versionPMT_[service.serviceId] = 0;
    }
    
    // Configurer la PMT
    ts::PMT* pmt = pmts_[service.serviceId].get();
    pmt->service_id = service.serviceId;
    pmt->version = versionPMT_[service.serviceId];
    pmt->is_current = true;
    
    // Nettoyer les streams existants
    pmt->streams.clear();
    
    // Ajouter les composants en utilisant l'API correcte de TSDuck 3.40
    for (const auto& [pid, streamType] : service.components) {
        auto& stream = pmt->streams[pid]; // Crée une nouvelle entrée dans la map
        stream.stream_type = streamType;
        // Note: Dans TSDuck 3.40, 'pid' est la clé de la map, pas un membre de Stream
    }
    
    spdlog::info("Service configuré: ID={}, PMT PID=0x{:X}, {} composants", 
               service.serviceId, service.pmtPid, service.components.size());
}

bool DVBProcessor::removeService(uint16_t serviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Vérifier si le service existe
    auto it = services_.find(serviceId);
    if (it == services_.end()) {
        return false;
    }
    
    // Supprimer le service
    services_.erase(it);
    
    // Supprimer la PMT associée
    pmts_.erase(serviceId);
    versionPMT_.erase(serviceId);
    
    spdlog::info("Service supprimé: ID={}", serviceId);
    
    return true;
}

std::vector<DVBService> DVBProcessor::getServices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<DVBService> result;
    result.reserve(services_.size());
    
    for (const auto& [id, service] : services_) {
        result.push_back(service);
    }
    
    return result;
}

std::vector<uint8_t> DVBProcessor::generatePAT() {
    if (!pat_) return {};
    
    try {
        // Mettre à jour la PAT
        pat_->version = versionPAT_;
        pat_->pmts.clear();
        
        // Ajouter tous les services à la PAT
        for (const auto& [serviceId, service] : services_) {
            pat_->pmts[serviceId] = service.pmtPid;
        }
        
        // Incrémenter la version pour la prochaine fois
        versionPAT_ = (versionPAT_ + 1) % 32;
        
        // Créer une instance DuckContext pour la sérialisation
        ts::DuckContext duck;
        
        // Sérialiser la PAT en BinaryTable
        ts::BinaryTable binTable;
        if (!pat_->serialize(duck, binTable)) {
            spdlog::error("Erreur lors de la sérialisation de la PAT");
            return {};
        }
        
        std::vector<uint8_t> result;
        
        // Parcourir les sections de la table binaire
        for (size_t i = 0; i < binTable.sectionCount(); ++i) {
            // Récupérer la section (qui est un SectionPtr, donc un shared_ptr<Section>)
            ts::SectionPtr sectionPtr = binTable.sectionAt(i);
            if (!sectionPtr) {
                continue;
            }
            
            // Créer un paquet à partir de la section
            std::vector<ts::TSPacket> sectionPackets;
            
            // Utiliser le constructeur de paquet avec PID
            ts::TSPacket pkt;
            pkt.init(0x00); // PID = 0x00 pour la PAT
            
            // Récupérer les données de la section
            const uint8_t* sectionData = sectionPtr->content();
            size_t remainingSize = sectionPtr->size();
            bool first = true;
            
            while (remainingSize > 0) {
                // Calculer l'espace disponible dans le paquet
                size_t headerSize = first ? 4 : 1; // 4 octets pour le premier paquet, 1 pour les suivants
                size_t payloadSize = std::min(remainingSize, size_t(ts::PKT_SIZE - headerSize));
                
                // Initialiser le paquet
                pkt.init(0x00);
                pkt.setPUSI(first);
                
                // Si c'est le premier paquet, ajouter un pointeur de champ
                if (first) {
                    pkt.b[4] = 0; // pointer_field = 0 (la section commence immédiatement)
                    std::memcpy(pkt.b + 5, sectionData, payloadSize);
                    first = false;
                } else {
                    std::memcpy(pkt.b + 4, sectionData, payloadSize);
                }
                
                // Mettre à jour les compteurs
                sectionData += payloadSize;
                remainingSize -= payloadSize;
                
                // Ajouter le paquet au résultat
                result.insert(result.end(), pkt.b, pkt.b + ts::PKT_SIZE);
            }
        }
        
        return result;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la génération de la PAT: {}", e.what());
        return {};
    }
}

std::vector<uint8_t> DVBProcessor::generatePMT(uint16_t serviceId) {
    // Vérifier si le service existe
    auto serviceIt = services_.find(serviceId);
    if (serviceIt == services_.end()) {
        return {};
    }
    
    // Vérifier si la PMT existe
    auto pmtIt = pmts_.find(serviceId);
    if (pmtIt == pmts_.end()) {
        return {};
    }
    
    const auto& service = serviceIt->second;
    ts::PMT* pmt = pmtIt->second.get();
    
    try {
        // Mettre à jour la PMT
        pmt->service_id = serviceId;
        pmt->version = versionPMT_[serviceId];
        pmt->is_current = true;
        
        // Mettre à jour les streams avec une approche compatible TSDuck 3.40
        pmt->streams.clear();
        for (const auto& [pid, streamType] : service.components) {
            auto& stream = pmt->streams[pid]; // Crée une nouvelle entrée dans la map
            stream.stream_type = streamType;
        }
        
        // Incrémenter la version pour la prochaine fois
        versionPMT_[serviceId] = (versionPMT_[serviceId] + 1) % 32;
        
        // Créer une instance DuckContext pour la sérialisation
        ts::DuckContext duck;
        
        // Sérialiser la PMT en BinaryTable
        ts::BinaryTable binTable;
        if (!pmt->serialize(duck, binTable)) {
            spdlog::error("Erreur lors de la sérialisation de la PMT");
            return {};
        }
        
        // Convertir la table en paquets TS
        std::vector<uint8_t> result;
        uint16_t pmtPid = service.pmtPid;
        
        // Parcourir les sections de la table binaire
        for (size_t i = 0; i < binTable.sectionCount(); ++i) {
            // Récupérer la section (qui est un SectionPtr, donc un shared_ptr<Section>)
            ts::SectionPtr sectionPtr = binTable.sectionAt(i);
            if (!sectionPtr) {
                continue;
            }
            
            // Créer un paquet à partir de la section
            ts::TSPacket pkt;
            pkt.init(pmtPid);
            
            // Récupérer les données de la section
            const uint8_t* sectionData = sectionPtr->content();
            size_t remainingSize = sectionPtr->size();
            bool first = true;
            
            while (remainingSize > 0) {
                // Calculer l'espace disponible dans le paquet
                size_t headerSize = first ? 4 : 1; // 4 octets pour le premier paquet, 1 pour les suivants
                size_t payloadSize = std::min(remainingSize, size_t(ts::PKT_SIZE - headerSize));
                
                // Initialiser le paquet
                pkt.init(pmtPid);
                pkt.setPUSI(first);
                
                // Si c'est le premier paquet, ajouter un pointeur de champ
                if (first) {
                    pkt.b[4] = 0; // pointer_field = 0 (la section commence immédiatement)
                    std::memcpy(pkt.b + 5, sectionData, payloadSize);
                    first = false;
                } else {
                    std::memcpy(pkt.b + 4, sectionData, payloadSize);
                }
                
                // Mettre à jour les compteurs
                sectionData += payloadSize;
                remainingSize -= payloadSize;
                
                // Ajouter le paquet au résultat
                result.insert(result.end(), pkt.b, pkt.b + ts::PKT_SIZE);
            }
        }
        
        return result;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la génération de la PMT: {}", e.what());
        return {};
    }
}

std::vector<uint8_t> DVBProcessor::generateSDT() {
    if (!sdt_) {
        return {};
    }
    
    try {
        // Mettre à jour la SDT
        sdt_->version = versionSDT_;
        sdt_->services.clear();
        
        // Créer une instance DuckContext pour les descripteurs
        ts::DuckContext duck;
        
        // Ajouter tous les services à la SDT en utilisant l'API correcte de TSDuck 3.40
        for (const auto& [serviceId, service] : services_) {
            // Dans TSDuck 3.40, on doit accéder directement à la map services
            auto& sdtServiceEntry = sdt_->services[serviceId]; // Crée une nouvelle entrée dans la map
            
            // Configuration des flags standard
            sdtServiceEntry.running_status = 4; // running
            sdtServiceEntry.CA_controlled = false;
            
            // Créer un descripteur de service
            // Dans TSDuck 3.40, utiliser l'approche avec descripteur et ByteBlock
            ts::ByteBlock descriptorPayload;
            uint8_t serviceType = service.serviceType;
            
            // Convertir les chaînes std::string en UString
            ts::UString providerName;
            ts::UString serviceName;
            providerName.assignFromUTF8(service.provider);
            serviceName.assignFromUTF8(service.name);
            
            // Créer un descripteur directement à partir de ses composants
            // Format du descripteur de service:
            // service_type (8 bits)
            // service_provider_length (8 bits)
            // service_provider_name (N octets)
            // service_name_length (8 bits)
            // service_name (N octets)
            
            // Ajouter le service_type
            descriptorPayload.append(serviceType);

            // Ajouter le provider name
            std::string providerUtf8 = providerName.toUTF8();
            ts::ByteBlock providerBytes;
            providerBytes.append(providerUtf8.data(), providerUtf8.size());
            descriptorPayload.append(uint8_t(providerBytes.size()));
            descriptorPayload.append(providerBytes);

            // Ajouter le service name
            std::string serviceUtf8 = serviceName.toUTF8();
            ts::ByteBlock serviceBytes;
            serviceBytes.append(serviceUtf8.data(), serviceUtf8.size());
            descriptorPayload.append(uint8_t(serviceBytes.size()));
            descriptorPayload.append(serviceBytes);

            // Créer le descripteur avec tag 0x48 (Service Descriptor)
            sdtServiceEntry.descs.add(descriptorPayload.data(), descriptorPayload.size());
        }
        
        // Incrémenter la version pour la prochaine fois
        versionSDT_ = (versionSDT_ + 1) % 32;
        
        // Sérialiser la SDT en BinaryTable
        ts::BinaryTable binTable;
        if (!sdt_->serialize(duck, binTable)) {
            spdlog::error("Erreur lors de la sérialisation de la SDT");
            return {};
        }
        
        // Convertir la table en paquets TS
        std::vector<uint8_t> result;
        uint16_t sdtPid = 0x11; // PID standard pour la SDT
        
        // Parcourir les sections de la table binaire
        for (size_t i = 0; i < binTable.sectionCount(); ++i) {
            // Récupérer la section (qui est un SectionPtr, donc un shared_ptr<Section>)
            ts::SectionPtr sectionPtr = binTable.sectionAt(i);
            if (!sectionPtr) {
                continue;
            }
            
            // Créer un paquet à partir de la section
            ts::TSPacket pkt;
            pkt.init(sdtPid);
            
            // Récupérer les données de la section
            const uint8_t* sectionData = sectionPtr->content();
            size_t remainingSize = sectionPtr->size();
            bool first = true;
            
            while (remainingSize > 0) {
                // Calculer l'espace disponible dans le paquet
                size_t headerSize = first ? 4 : 1; // 4 octets pour le premier paquet, 1 pour les suivants
                size_t payloadSize = std::min(remainingSize, size_t(ts::PKT_SIZE - headerSize));
                
                // Initialiser le paquet
                pkt.init(sdtPid);
                pkt.setPUSI(first);
                
                // Si c'est le premier paquet, ajouter un pointeur de champ
                if (first) {
                    pkt.b[4] = 0; // pointer_field = 0 (la section commence immédiatement)
                    std::memcpy(pkt.b + 5, sectionData, payloadSize);
                    first = false;
                } else {
                    std::memcpy(pkt.b + 4, sectionData, payloadSize);
                }
                
                // Mettre à jour les compteurs
                sectionData += payloadSize;
                remainingSize -= payloadSize;
                
                // Ajouter le paquet au résultat
                result.insert(result.end(), pkt.b, pkt.b + ts::PKT_SIZE);
            }
        }
        
        return result;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la génération de la SDT: {}", e.what());
        return {};
    }
}

std::vector<uint8_t> DVBProcessor::generateEIT() {
    // Pour simplifier, nous n'allons pas générer d'EIT
    return {};
}

std::vector<uint8_t> DVBProcessor::generateNIT() {
    if (!nit_) {
        return {};
    }
    
    try {
        // Mettre à jour la NIT
        nit_->version = versionNIT_;
        nit_->transports.clear();
        
        // Créer une instance DuckContext pour la sérialisation
        ts::DuckContext duck;
        
        // Pour TSDuck 3.40, ajouter un transport de façon compatible
        uint16_t tsid = 1;
        uint16_t onid = 1;
        // Utiliser TransportStreamId comme clé dans la map des transports
        ts::TransportStreamId tsKey(tsid, onid);
        // Obtenir une référence à la nouvelle entrée (qui est automatiquement créée)
        auto& transport = nit_->transports[tsKey];
        
        // Incrémenter la version pour la prochaine fois
        versionNIT_ = (versionNIT_ + 1) % 32;
        
        // Sérialiser la NIT en BinaryTable
        ts::BinaryTable binTable;
        if (!nit_->serialize(duck, binTable)) {
            spdlog::error("Erreur lors de la sérialisation de la NIT");
            return {};
        }
        
        // Convertir la table en paquets TS
        std::vector<uint8_t> result;
        uint16_t nitPid = 0x10; // PID standard pour la NIT
        
        // Parcourir les sections de la table binaire
        for (size_t i = 0; i < binTable.sectionCount(); ++i) {
            // Récupérer la section (qui est un SectionPtr, donc un shared_ptr<Section>)
            ts::SectionPtr sectionPtr = binTable.sectionAt(i);
            if (!sectionPtr) {
                continue;
            }
            
            // Créer un paquet à partir de la section
            ts::TSPacket pkt;
            pkt.init(nitPid);
            
            // Récupérer les données de la section
            const uint8_t* sectionData = sectionPtr->content();
            size_t remainingSize = sectionPtr->size();
            bool first = true;
            
            while (remainingSize > 0) {
                // Calculer l'espace disponible dans le paquet
                size_t headerSize = first ? 4 : 1; // 4 octets pour le premier paquet, 1 pour les suivants
                size_t payloadSize = std::min(remainingSize, size_t(ts::PKT_SIZE - headerSize));
                
                // Initialiser le paquet
                pkt.init(nitPid);
                pkt.setPUSI(first);
                
                // Si c'est le premier paquet, ajouter un pointeur de champ
                if (first) {
                    pkt.b[4] = 0; // pointer_field = 0 (la section commence immédiatement)
                    std::memcpy(pkt.b + 5, sectionData, payloadSize);
                    first = false;
                } else {
                    std::memcpy(pkt.b + 4, sectionData, payloadSize);
                }
                
                // Mettre à jour les compteurs
                sectionData += payloadSize;
                remainingSize -= payloadSize;
                
                // Ajouter le paquet au résultat
                result.insert(result.end(), pkt.b, pkt.b + ts::PKT_SIZE);
            }
        }
        
        return result;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la génération de la NIT: {}", e.what());
        return {};
    }
}

std::map<uint16_t, uint8_t> DVBProcessor::analyzePIDs(const std::vector<uint8_t>& data) {
    std::map<uint16_t, uint8_t> pidTypes;
    
    // Si nous avons déjà des services configurés, ne pas faire d'analyse
    if (!services_.empty()) {
        return pidTypes;
    }
    
    // Convertir les données en paquets TS
    size_t packetCount = data.size() / ts::PKT_SIZE;
    std::set<uint16_t> pids;
    
    for (size_t i = 0; i < packetCount; ++i) {
        const uint8_t* packetData = &data[i * ts::PKT_SIZE];
        
        // Extraire le PID des bytes 1-2 (13 bits)
        uint16_t pid = ((packetData[1] & 0x1F) << 8) | packetData[2];
        
        // Ignorer les PID réservés et PSI standard
        if (pid > 0x1F && pid < 0x1FFF && 
            pid != 0x0000 && pid != 0x0001 && 
            pid != 0x0010 && pid != 0x0011 && 
            pid != 0x0012 && pid != 0x0014) {
            pids.insert(pid);
        }
    }
    
    // S'il n'y a pas de service configuré et qu'on trouve des PID, créer un service par défaut
    if (services_.empty() && !pids.empty()) {
        DVBService service;
        service.serviceId = 1;
        service.pmtPid = 0x1000;  // Valeur arbitraire pour la PMT
        service.name = "Service par défaut";
        service.provider = "Fournisseur par défaut";
        service.serviceType = 0x01;  // TV numérique
        
        // Ajouter les PID détectés comme composants
        for (uint16_t pid : pids) {
            // Par défaut, supposer que c'est de la vidéo H.264
            service.components[pid] = 0x1B;  // Stream type pour H.264
            pidTypes[pid] = 0x1B;
        }
        
        // Ajouter le service
        setService(service);
    }
    
    return pidTypes;
}

std::vector<uint8_t> DVBProcessor::insertTables(const std::vector<uint8_t>& data, 
                                              const std::map<uint16_t, std::vector<uint8_t>>& tables) {
    // Si aucune table à insérer, retourner les données originales
    if (tables.empty()) {
        return data;
    }
    
    // Convertir les données en paquets TS
    size_t packetCount = data.size() / ts::PKT_SIZE;
    std::vector<ts::TSPacket> packets;
    packets.reserve(packetCount);
    
    for (size_t i = 0; i < packetCount; ++i) {
        ts::TSPacket packet;
        std::memcpy(packet.b, &data[i * ts::PKT_SIZE], ts::PKT_SIZE);
        packets.push_back(packet);
    }
    
    // Ordre d'insertion des tables PSI standard
    const std::vector<uint16_t> psiOrder = {0x0000, 0x0010, 0x0011, 0x0012};
    
    // Supprimer les paquets PSI existants et collecter les PID utilisés
    std::set<uint16_t> existingPIDs;
    
    for (auto it = packets.begin(); it != packets.end();) {
        uint16_t pid = it->getPID();
        existingPIDs.insert(pid);
        
        // Si c'est un PID de table PSI à remplacer, le supprimer
        if (tables.find(pid) != tables.end()) {
            it = packets.erase(it);
        } else {
            ++it;
        }
    }
    
    // Préparer les nouveaux paquets des tables PSI
    std::vector<ts::TSPacket> psiPackets;
    
    // Ajouter d'abord les tables PSI dans l'ordre standard
    for (uint16_t pid : psiOrder) {
        auto it = tables.find(pid);
        if (it != tables.end() && !it->second.empty()) {
            // Convertir cette table en paquets
            size_t tablePacketCount = it->second.size() / ts::PKT_SIZE;
            for (size_t i = 0; i < tablePacketCount; ++i) {
                ts::TSPacket packet;
                std::memcpy(packet.b, &it->second[i * ts::PKT_SIZE], ts::PKT_SIZE);
                psiPackets.push_back(packet);
            }
        }
    }
    
    // Puis ajouter les autres tables (PMT, etc.)
    for (const auto& [pid, tableData] : tables) {
        // Ignorer les PID déjà traités
        if (pid == 0x0000 || pid == 0x0010 || pid == 0x0011 || pid == 0x0012) {
            continue;
        }
        
        if (!tableData.empty()) {
            // Convertir cette table en paquets
            size_t tablePacketCount = tableData.size() / ts::PKT_SIZE;
            for (size_t i = 0; i < tablePacketCount; ++i) {
                ts::TSPacket packet;
                std::memcpy(packet.b, &tableData[i * ts::PKT_SIZE], ts::PKT_SIZE);
                psiPackets.push_back(packet);
            }
        }
    }
    
    // Insertion des paquets PSI au début du flux
    packets.insert(packets.begin(), psiPackets.begin(), psiPackets.end());
    

    // Convertir les paquets en vecteur d'octets
    std::vector<uint8_t> result;
    result.reserve(packets.size() * ts::PKT_SIZE);
    
    for (const auto& packet : packets) {
        result.insert(result.end(), packet.b, packet.b + ts::PKT_SIZE);
    }
    
    return result;
}

DVBProcessor::~DVBProcessor() {
    cleanup();
}
