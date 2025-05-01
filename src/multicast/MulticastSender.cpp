#include "multicast/MulticastSender.h"
#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>

#include <cstring>
#include <algorithm>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socklen_t = int;
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <fcntl.h>
  #define SOCKET int
  #define INVALID_SOCKET -1
  #define SOCKET_ERROR -1
  #define closesocket close
#endif

namespace hls_to_dvb {

MulticastSender::MulticastSender(const std::string& groupAddress, int port, 
                              const std::string& interface, int ttl)
    : groupAddress_(groupAddress), port_(port), interface_(interface), ttl_(ttl),
      running_(false), bitrateKbps_(0), socket_(INVALID_SOCKET) {
    
    // Initialiser l'horodatage de dernier envoi
    stats_.lastSendTime = std::chrono::system_clock::now();
}

MulticastSender::~MulticastSender() {
    stop();
    closeSocket();
}

bool MulticastSender::initialize() {
#ifdef _WIN32
    // Initialiser Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        spdlog::error("WSAStartup failed");
        return false;
    }
#endif
    
    // Créer le socket
    if (!createSocket()) {
        return false;
    }
    
    spdlog::info("MulticastSender initialized for group {}:{}", groupAddress_, port_);
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "MulticastSender",
        "Initialized for group " + groupAddress_ + ":" + std::to_string(port_),
        false
    );
    
    return true;
}

bool MulticastSender::start() {
    if (running_) {
        spdlog::warn("MulticastSender already running");
        return false;
    }
    
    if (socket_ == INVALID_SOCKET) {
        spdlog::error("Cannot start MulticastSender: socket not initialized");
        return false;
    }
    
    running_ = true;
    
    // Démarrer le thread d'envoi
    senderThread_ = std::thread(&MulticastSender::senderLoop, this);
    
    spdlog::info("MulticastSender started for group {}:{}", groupAddress_, port_);
    return true;
}

void MulticastSender::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Notifier tous les threads en attente
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queueCond_.notify_all();
    }
    
    // Attendre que le thread se termine
    if (senderThread_.joinable()) {
        senderThread_.join();
    }
    
    spdlog::info("MulticastSender stopped for group {}:{}", groupAddress_, port_);
}

bool MulticastSender::isRunning() const {
    return running_;
}

bool MulticastSender::send(const std::vector<uint8_t>& data) {
    if (!running_) {
        spdlog::warn("MulticastSender not running");
        return false;
    }
    
    // Ajouter les données à la file d'attente
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        dataQueue_.push(data);
    }
    
    // Notifier le thread d'envoi
    queueCond_.notify_one();
    
    return true;
}

void MulticastSender::setBitrate(uint32_t bitrateKbps) {
    bitrateKbps_ = bitrateKbps;
    spdlog::info("MulticastSender bitrate set to {} kbps", bitrateKbps);
}

const MulticastStats& MulticastSender::getStats() const {
    return stats_;
}

std::string MulticastSender::getGroupAddress() const {
    return groupAddress_;
}

int MulticastSender::getPort() const {
    return port_;
}

void MulticastSender::senderLoop() {
    spdlog::debug("MulticastSender thread started");
    
    // Structure pour l'adresse de destination
    struct sockaddr_in destAddr;
    std::memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sin_family = AF_INET;
    destAddr.sin_addr.s_addr = inet_addr(groupAddress_.c_str());
    destAddr.sin_port = htons(port_);
    
    // Taille maximum d'un datagramme UDP (pour éviter la fragmentation)
    const size_t MAX_PACKET_SIZE = 1316; // 7 paquets MPEG-TS (7*188=1316)
    
    // Timers pour le contrôle de débit
    auto lastSendTime = std::chrono::steady_clock::now();
    size_t bytesSent = 0;
    
    while (running_) {
        std::vector<uint8_t> data;
        
        // Attendre des données à envoyer
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (dataQueue_.empty()) {
                // Attendre qu'il y ait des données ou que le thread doive s'arrêter
                queueCond_.wait_for(lock, std::chrono::milliseconds(100),
                                   [this] { return !dataQueue_.empty() || !running_; });
                
                if (!running_) {
                    break;
                }
                
                if (dataQueue_.empty()) {
                    continue;
                }
            }
            
            data = std::move(dataQueue_.front());
            dataQueue_.pop();
        }
        
        // Si limitation de débit active, contrôler le débit
        if (bitrateKbps_ > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSendTime).count();
            
            // Calculer le temps nécessaire pour envoyer les données au débit configuré
            double byteRate = bitrateKbps_ * 1000.0 / 8.0; // Octets par seconde
            double expectedTimeMs = (bytesSent * 1000.0) / byteRate;
            
            // Si on est en avance, attendre
            if (elapsedMs < expectedTimeMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<long>(expectedTimeMs - elapsedMs)));
            }
            
            // Réinitialiser si on dépasse 1 seconde
            if (elapsedMs > 1000) {
                lastSendTime = now;
                bytesSent = 0;
            }
        }
        
        // Envoyer les données par paquets pour éviter la fragmentation
        for (size_t offset = 0; offset < data.size(); offset += MAX_PACKET_SIZE) {
            // Calculer la taille du paquet actuel
            size_t packetSize = std::min(MAX_PACKET_SIZE, data.size() - offset);
            
            // Envoyer le paquet
            int sendResult = sendto(socket_, 
                                  reinterpret_cast<const char*>(data.data() + offset), 
                                  static_cast<int>(packetSize), 
                                  0, 
                                  reinterpret_cast<struct sockaddr*>(&destAddr), 
                                  sizeof(destAddr));
            
            if (sendResult == SOCKET_ERROR) {
                stats_.errors++;
                
#ifdef _WIN32
                spdlog::error("Error sending multicast packet: {}", WSAGetLastError());
#else
                spdlog::error("Error sending multicast packet: {}", strerror(errno));
#endif
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::ERROR,
                    "MulticastSender",
                    "Error sending multicast packet",
                    false
                );
                
                // Attendre un peu avant de réessayer
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            
            // Mettre à jour les statistiques
            stats_.packetsSent++;
            stats_.bytesSent += packetSize;
            bytesSent += packetSize;
            
            if (bitrateKbps_ > 0) {
                // Ajouter un léger délai entre les paquets pour lisser le débit
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        
        // Mettre à jour le débit instantané
        auto now = std::chrono::system_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - stats_.lastSendTime).count();
        
        if (elapsedMs > 0) {
            stats_.instantBitrate = (data.size() * 8.0) / (elapsedMs / 1000.0);
            
            // Mise à jour du débit moyen (moyenne mobile)
            if (stats_.bitrate == 0.0) {
                stats_.bitrate = stats_.instantBitrate;
            } else {
                stats_.bitrate = stats_.bitrate * 0.9 + stats_.instantBitrate * 0.1;
            }
        }
        
        stats_.lastSendTime = now;
    }
    
    spdlog::debug("MulticastSender thread ended");
}

bool MulticastSender::createSocket() {
    // Fermer le socket existant si nécessaire
    closeSocket();
    
    // Créer un nouveau socket UDP
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ == INVALID_SOCKET) {
#ifdef _WIN32
        spdlog::error("Failed to create socket: {}", WSAGetLastError());
#else
        spdlog::error("Failed to create socket: {}", strerror(errno));
#endif
        return false;
    }
    
    // Configurer les options multicast
    
    // Définir le TTL
    if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL, 
                  reinterpret_cast<const char*>(&ttl_), sizeof(ttl_)) < 0) {
#ifdef _WIN32
        spdlog::error("Failed to set TTL: {}", WSAGetLastError());
#else
        spdlog::error("Failed to set TTL: {}", strerror(errno));
#endif
        closeSocket();
        return false;
    }
    
    // Définir l'interface de sortie si spécifiée
    if (!interface_.empty()) {
        struct in_addr localInterface;
        localInterface.s_addr = inet_addr(interface_.c_str());
        
        if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, 
                      reinterpret_cast<const char*>(&localInterface), sizeof(localInterface)) < 0) {
#ifdef _WIN32
            spdlog::error("Failed to set outgoing interface: {}", WSAGetLastError());
#else
            spdlog::error("Failed to set outgoing interface: {}", strerror(errno));
#endif
            closeSocket();
            return false;
        }
    }
    
    // Activer la réutilisation d'adresse
    int reuse = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, 
                  reinterpret_cast<const char*>(&reuse), sizeof(reuse)) < 0) {
#ifdef _WIN32
        spdlog::error("Failed to set SO_REUSEADDR: {}", WSAGetLastError());
#else
        spdlog::error("Failed to set SO_REUSEADDR: {}", strerror(errno));
#endif
        closeSocket();
        return false;
    }
    
    // Augmenter la taille du buffer d'envoi
    int sendBufSize = 1024 * 1024; // 1 MB
    if (setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, 
                  reinterpret_cast<const char*>(&sendBufSize), sizeof(sendBufSize)) < 0) {
#ifdef _WIN32
        spdlog::warn("Failed to set send buffer size: {}", WSAGetLastError());
#else
        spdlog::warn("Failed to set send buffer size: {}", strerror(errno));
#endif
        // On continue quand même, ce n'est pas critique
    }
    
    return true;
}

void MulticastSender::closeSocket() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    
#ifdef _WIN32
    // Nettoyer Winsock
    WSACleanup();
#endif
}

} // namespace hls_to_dvb


