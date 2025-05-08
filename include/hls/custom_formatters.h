#pragma once

#include <atomic>
#include <regex>
#include <fmt/format.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

// Formateur pour std::atomic<T>
template <typename T>
struct fmt::formatter<std::atomic<T>> : fmt::formatter<T> {
    auto format(const std::atomic<T>& atomic, fmt::format_context& ctx) const {
        return fmt::formatter<T>::format(atomic.load(), ctx);
    }
};

// Formateur pour std::sub_match<CharT>
template<typename CharT>
struct fmt::formatter<std::sub_match<CharT>> : fmt::formatter<std::basic_string<CharT>> {
    auto format(const std::sub_match<CharT>& match, fmt::format_context& ctx) const {
        return fmt::formatter<std::basic_string<CharT>>::format(match.str(), ctx);
    }
};

// Formateur pour AVCodecID
template<>
struct fmt::formatter<AVCodecID> : fmt::formatter<int> {
    auto format(const AVCodecID& codec_id, fmt::format_context& ctx) const {
        return fmt::formatter<int>::format(static_cast<int>(codec_id), ctx);
    }
};
