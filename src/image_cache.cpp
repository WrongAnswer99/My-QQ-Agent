#include "image_cache.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace {
bool IsNumber(const std::string& value) {
    return !value.empty() && value.size() <= 20
        && std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}
}

ImageCache::ImageCache(std::string directory) : directory_(std::move(directory)) {
    next_id_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string ImageCache::Store(const std::string& group_id, const std::string& base64) {
    if (!IsNumber(group_id) || base64.empty()) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        const auto directory = std::filesystem::path(directory_) / group_id;
        std::filesystem::create_directories(directory);
        std::filesystem::path path;
        std::string id;
        do {
            id = std::to_string(++next_id_);
            path = directory / (id + ".b64");
        } while (std::filesystem::exists(path));
        std::ofstream output(path, std::ios::binary);
        output << base64;
        output.close();
        if (!output) {
            std::filesystem::remove(path);
            return {};
        }
        return id;
    } catch (const std::filesystem::filesystem_error&) {
        return {};
    }
}

std::string ImageCache::Resolve(const std::string& group_id, const std::string& id) const {
    if (!IsNumber(group_id) || !IsNumber(id)) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream input(std::filesystem::path(directory_) / group_id / (id + ".b64"),
                        std::ios::binary);
    if (!input) {
        return {};
    }
    const std::string base64{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || base64.empty()) {
        return {};
    }
    return "base64://" + base64;
}
