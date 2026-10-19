#include "ai_client.hpp"

#include <algorithm>
#include <ctime>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>

#include "json.hpp"

using json = nlohmann::json;

namespace {

// 回调函数，用于接收 HTTP 响应数据
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t total_size = size * nmemb;
    userp->append(static_cast<char*>(contents), total_size);
    return total_size;
}

} // namespace

namespace ai {

namespace {

Settings g_settings;          // 全局 AI 设置
std::once_flag g_init_flag;   // 保证 curl_global_init 只执行一次
std::mutex g_log_mutex;       // 防止并发 AI 请求交叉写入日志

void GlobalInitOnce() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void LogAIOutput(const std::string& output) {
    // 日志失败不能影响正常回复，因此这里有意静默返回。
    std::lock_guard<std::mutex> lock(g_log_mutex);
    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    if (localtime_s(&local_time, &now) != 0) {
        return;
    }

    char date[11]{};
    char timestamp[20]{};
    if (std::strftime(date, sizeof(date), "%Y-%m-%d", &local_time) == 0
        || std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                         &local_time) == 0) {
        return;
    }

    std::error_code directory_error;
    const std::filesystem::path log_directory = std::filesystem::current_path() / "logs";
    std::filesystem::create_directories(log_directory, directory_error);
    if (directory_error) {
        return;
    }

    std::ofstream log(log_directory / (std::string(date) + ".log"),
                      std::ios::binary | std::ios::app);
    if (!log) {
        return;
    }
    log << "[" << timestamp << "] AI output\n" << output << "\n\n";
}

} // namespace

bool Init(const Settings& settings, std::string& error) {
    if (settings.api_key.empty() || settings.base_url.empty() || settings.model.empty()) {
        error = "AI 配置不完整（api_key / base_url / model 均不能为空）";
        return false;
    }

    g_settings = settings;
    // 支持 API 基础地址；在查询参数之前补全路径，避免重复追加。
    const auto suffix_pos = g_settings.base_url.find_first_of("?#");
    std::string endpoint = g_settings.base_url.substr(0, suffix_pos);
    const std::string suffix = suffix_pos == std::string::npos ? "" : g_settings.base_url.substr(suffix_pos);
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    const std::string chat_path = "/chat/completions";
    if (endpoint.size() < chat_path.size()
        || endpoint.compare(endpoint.size() - chat_path.size(), chat_path.size(), chat_path) != 0) {
        endpoint += chat_path;
    }
    g_settings.base_url = endpoint + suffix;
    std::call_once(g_init_flag, GlobalInitOnce);
    return true;
}

std::string Chat(const ChatRequest& request, std::string& error) {
    error.clear();
    if (g_settings.api_key.empty()) {
        error = "AI 客户端未初始化，请先调用 ai::Init";
        return std::string();
    }

    // 构建 JSON 请求体
    json request_body;
    const bool has_images = std::any_of(request.messages.begin(), request.messages.end(),
        [](const Message& message) { return !message.images.empty(); });
    request_body["model"] = has_images && !g_settings.vision_model.empty()
        ? g_settings.vision_model : g_settings.model;
    request_body["messages"] = json::array();
    request_body["messages"].push_back({
        {"role", "system"},
        {"content", request.system_prompt}
    });
    for (const Message& message : request.messages) {
        if (message.images.empty()) {
            request_body["messages"].push_back({
                {"role", message.role},
                {"content", message.content}
            });
            continue;
        }

        json content = json::array();
        if (!message.content.empty()) {
            content.push_back({{"type", "text"}, {"text", message.content}});
        }
        for (const Message::Image& image : message.images) {
            content.push_back({
                {"type", "image_url"},
                {"image_url", {
                    {"url", "data:" + image.mime_type + ";base64," + image.base64}
                }}
            });
        }
        request_body["messages"].push_back({
            {"role", message.role},
            {"content", std::move(content)}
        });
    }
    request_body["stream"] = false;
    if (request.temperature >= 0.0) {
        request_body["temperature"] = request.temperature;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        error = "curl_easy_init 失败";
        return std::string();
    }

    std::string request_json = request_body.dump();
    std::string read_buffer;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + g_settings.api_key).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, g_settings.base_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode result = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        error = std::string("HTTP 请求失败: ") + curl_easy_strerror(result);
        return std::string();
    }

    // 解析响应
    try {
        json response = json::parse(read_buffer);
        if (response.contains("error")) {
            error = "API 返回错误: " + response["error"].dump();
            return std::string();
        }
        if (response.contains("choices")) {
            const std::string output =
                response["choices"][0]["message"]["content"].get<std::string>();
            LogAIOutput(output);
            return output;
        }
        if (response.contains("text")) {
            const std::string output = response["text"].get<std::string>();
            LogAIOutput(output);
            return output;
        }
        error = "无法识别的响应: " + response.dump();
    } catch (const json::exception& e) {
        error = std::string("JSON 解析失败: ") + e.what();
        return std::string();
    }
    return std::string();
}

} // namespace ai
