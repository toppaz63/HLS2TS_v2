#include "core/SegmentBuffer.h"
#include <spdlog/spdlog.h>

namespace hls_to_dvb {

SegmentBuffer::SegmentBuffer(size_t bufferSize, const std::string& name)
    : bufferSize_(bufferSize), name_(name) {
    spdlog::info("SegmentBuffer '{}' created with size {}", name_, bufferSize_);
}

bool SegmentBuffer::pushSegment(const MPEGTSSegment& segment) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (segments_.size() >= bufferSize_) {
        spdlog::warn("SegmentBuffer '{}' is full, dropping segment", name_);
        return false;
    }
    
    segments_.push(segment);
    condition_.notify_one();
    return true;
}

bool SegmentBuffer::getSegment(MPEGTSSegment& segment, uint32_t waitTimeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (segments_.empty()) {
        if (waitTimeoutMs == 0) {
            return false;
        }
        
        auto waitResult = condition_.wait_for(lock, 
            std::chrono::milliseconds(waitTimeoutMs),
            [this]() { return !segments_.empty(); });
        
        if (!waitResult) {
            return false;
        }
    }
    
    segment = segments_.front();
    segments_.pop();
    return true;
}

size_t SegmentBuffer::getBufferSize() const {
    return bufferSize_.load();
}

void SegmentBuffer::setBufferSize(size_t newSize) {
    if (newSize == 0) {
        spdlog::error("Cannot set buffer size to 0, keeping current size {}", bufferSize_.load());
        return;
    }
    
    size_t oldSize = bufferSize_.exchange(newSize);
    spdlog::info("SegmentBuffer '{}' size changed from {} to {}", name_, oldSize, newSize);
}

} // namespace hls_to_dvb
