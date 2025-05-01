#include "SegmentBuffer.h"
#include "spdlog/spdlog.h"

SegmentBuffer::SegmentBuffer(size_t bufferSize)
    : bufferSize_(bufferSize) {
    
    spdlog::debug("Buffer de segments créé avec une taille de {}", bufferSize);
}

bool SegmentBuffer::pushSegment(const MPEGTSSegment& segment) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Vérifier si le buffer est plein
    if (buffer_.size() >= bufferSize_) {
        // Si le buffer est plein, supprimer le segment le plus ancien
        buffer_.pop_front();
        
        spdlog::debug("Buffer plein, suppression du segment le plus ancien");
    }
    
    // Ajouter le segment
    buffer_.push_back(segment);
    
    // Notifier les threads en attente
    conditionVar_.notify_one();
    
    spdlog::debug("Segment {} ajouté au buffer, taille actuelle: {}/{}", 
                segment.sequenceNumber, buffer_.size(), bufferSize_.load());
    
    return true;
}

bool SegmentBuffer::getSegment(MPEGTSSegment& segment, int timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Si le buffer est vide et qu'un timeout est spécifié, attendre qu'un segment soit disponible
    if (buffer_.empty() && timeout > 0) {
        auto waitResult = conditionVar_.wait_for(lock, std::chrono::milliseconds(timeout),
                                               [this] { return !buffer_.empty(); });
        
        if (!waitResult) {
            // Timeout atteint
            spdlog::debug("Timeout atteint en attendant un segment, buffer vide");
            return false;
        }
    }
    
    // Vérifier si le buffer est toujours vide
    if (buffer_.empty()) {
        return false;
    }
    
    // Récupérer le segment le plus ancien
    segment = buffer_.front();
    buffer_.pop_front();
    
    spdlog::debug("Segment {} récupéré du buffer, taille actuelle: {}/{}", 
                segment.sequenceNumber, buffer_.size(), bufferSize_.load());
    
    return true;
}

void SegmentBuffer::setBufferSize(size_t bufferSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    bufferSize_ = bufferSize;
    
    // Si le buffer actuel est plus grand que la nouvelle taille, supprimer les segments les plus anciens
    while (buffer_.size() > bufferSize) {
        buffer_.pop_front();
    }
    
    spdlog::debug("Taille du buffer ajustée à {}, taille actuelle: {}/{}", 
                bufferSize, buffer_.size(), bufferSize_.load());
}

size_t SegmentBuffer::getBufferSize() const {
    return bufferSize_;
}

size_t SegmentBuffer::getCurrentSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.size();
}

void SegmentBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    buffer_.clear();
    
    spdlog::debug("Buffer vidé");
}
