#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <mutex>

// Forward declarations
namespace ts {
    class PAT;
    class PMT;
    class SDT;
    class EIT;
    class TDT;
    class TOT;
    class NIT;
}

/**
 * @struct DVBService
 * @brief Représente un service DVB dans le flux MPEG-TS
 */
struct DVBService {
    uint16_t serviceId;         ///< Identifiant du service
    uint16_t pmtPid;            ///< PID de la PMT du service
    std::string name;           ///< Nom du service
    std::string provider;       ///< Nom du fournisseur
    uint8_t serviceType;        ///< Type de service (0x01 = TV numérique, 0x02 = Radio numérique, etc.)
    std::map<uint16_t, uint8_t> components; ///< Composants du service (PID -> type de flux)
};

/**
 * @class DVBProcessor
 * @brief Traite les flux MPEG-TS pour assurer leur conformité avec la norme DVB
 * 
 * Gère la génération et la mise à jour des tables PSI/SI requises par la norme DVB.
 */
class DVBProcessor {
public:
    /**
     * @brief Constructeur
     */
    DVBProcessor();
    
    /**
     * @brief Initialise le processeur DVB
     */
    void initialize();
    
    /**
     * @brief Nettoie les ressources utilisées
     */
    void cleanup();
    
    /**
     * @brief Met à jour les tables PSI/SI dans un flux MPEG-TS
     * @param data Données MPEG-TS à mettre à jour
     * @param discontinuity Indique s'il y a une discontinuité
     * @return Données MPEG-TS avec tables mises à jour
     */
         std::vector<uint8_t> updatePSITables(const std::vector<uint8_t>& data, bool discontinuity = false);
    
    /**
     * @brief Configure un service DVB
     * @param service Configuration du service
     */
    void setService(const DVBService& service);
    
    /**
     * @brief Supprime un service DVB
     * @param serviceId Identifiant du service à supprimer
     * @return true si le service a été supprimé
     */
    bool removeService(uint16_t serviceId);
    
    /**
     * @brief Récupère la liste des services configurés
     * @return Liste des services DVB
     */
    std::vector<DVBService> getServices() const;
    
    /**
     * @brief Destructeur
     */
    ~DVBProcessor();
    
private:
    mutable std::mutex mutex_;                      ///< Mutex pour l'accès concurrent
    std::map<uint16_t, DVBService> services_;        ///< Services configurés (serviceId -> service)
    std::unique_ptr<ts::PAT> pat_;                  ///< Table d'association de programmes
    std::map<uint16_t, std::unique_ptr<ts::PMT>> pmts_; ///< Tables de mappage de programmes (serviceId -> PMT)
    std::unique_ptr<ts::SDT> sdt_;                  ///< Table de description de service
    std::unique_ptr<ts::EIT> eit_;                  ///< Table d'information d'événement
    std::unique_ptr<ts::NIT> nit_;                  ///< Table d'information réseau
    uint8_t versionPAT_;                            ///< Version de la PAT
    uint8_t versionSDT_;                            ///< Version de la SDT
    uint8_t versionEIT_;                            ///< Version de la EIT
    uint8_t versionNIT_;                            ///< Version de la NIT
    std::map<uint16_t, uint8_t> versionPMT_;        ///< Versions des PMT (serviceId -> version)
    
    /**
     * @brief Génère une table PAT
     * @return Données binaires de la PAT
     */
    std::vector<uint8_t> generatePAT();
    
    /**
     * @brief Génère une table PMT pour un service
     * @param serviceId Identifiant du service
     * @return Données binaires de la PMT
     */
    std::vector<uint8_t> generatePMT(uint16_t serviceId);
    
    /**
     * @brief Génère une table SDT
     * @return Données binaires de la SDT
     */
    std::vector<uint8_t> generateSDT();
    
    /**
     * @brief Génère une table EIT
     * @return Données binaires de la EIT
     */
    std::vector<uint8_t> generateEIT();
    
    /**
     * @brief Génère une table NIT
     * @return Données binaires de la NIT
     */
    std::vector<uint8_t> generateNIT();
    
    /**
     * @brief Analyse un flux MPEG-TS pour détecter les PID et les types de flux
     * @param data Données MPEG-TS à analyser
     * @return Map des PID détectés et leurs types
     */
    std::map<uint16_t, uint8_t> analyzePIDs(const std::vector<uint8_t>& data);
    
    /**
     * @brief Insère des tables PSI/SI dans un flux MPEG-TS
     * @param data Données MPEG-TS d'origine
     * @param tables Tables PSI/SI à insérer (map PID -> données)
     * @return Données MPEG-TS avec tables insérées
     */
    std::vector<uint8_t> insertTables(const std::vector<uint8_t>& data, 
                                    const std::map<uint16_t, std::vector<uint8_t>>& tables);
};