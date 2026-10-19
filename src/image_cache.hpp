#pragma once

#include <cstdint>
#include <mutex>
#include <string>

// 原图单独缓存，不进入描述模式的模型历史；只允许在所属群内按编号查找。
class ImageCache {
public:
    explicit ImageCache(std::string directory = "userdata/image_cache");
    // 返回纯数字编号；写入失败返回空串。
    std::string Store(const std::string& group_id, const std::string& base64);
    // 返回 OneBot base64:// 文件参数；编号不存在或不合法时返回空串。
    std::string Resolve(const std::string& group_id, const std::string& id) const;
private:
    std::string directory_;
    std::uint64_t next_id_ = 0;
    mutable std::mutex mutex_;
};
