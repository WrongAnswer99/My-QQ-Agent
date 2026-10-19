#pragma once

#include <string>
#include <vector>

// AI 回复接口：基于 libcurl 调用 OpenAI 兼容接口（参考 test 项目的实现）
namespace ai {

// AI 客户端设置（由 Init 传入，数据来自 config.json 的 ai 段）
struct Settings {
    std::string api_key;  // API Key
    std::string base_url; // 完整的 chat/completions 端点地址
    std::string model;    // 文本模型名
    std::string vision_model; // 请求包含图片时使用；留空则沿用 model
};

// 初始化 AI 客户端，需在首次调用 Chat 之前调用一次
bool Init(const Settings& settings, std::string& error);

struct Message {
    std::string role;    // system / user / assistant
    std::string content;
    struct Image {
        std::string mime_type;
        std::string base64;
    };
    std::vector<Image> images;
};

struct ChatRequest {
    std::string system_prompt; // 系统提示词
    std::vector<Message> messages; // 按时间顺序发送的上下文与当前消息
    // 小于 0 时不传该参数，沿用服务端默认值；结构化决策可设为 0 降低随机性。
    double temperature = -1.0;
};

// 调用 AI 接口，成功返回回复文本；失败返回空串并填充 error
std::string Chat(const ChatRequest& request, std::string& error);

} // namespace ai
