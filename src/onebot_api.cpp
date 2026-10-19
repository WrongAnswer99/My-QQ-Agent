#include "onebot_api.hpp"

#include <curl/curl.h>

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

OneBotApi::OneBotApi(const std::string& base_url, const std::string& token)
    : base_url_(base_url), token_(token) {
    while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
}

OneBotApi::~OneBotApi() {}

bool OneBotApi::SendPrivateMsg(const std::string& user_id, const std::string& message) {
    json body;
    body["user_id"] = std::stoll(user_id);
    body["message"] = message;

    std::string response;
    return PostJson("send_private_msg", body.dump(), response);
}

bool OneBotApi::SendGroupMsg(const std::string& group_id, const std::string& message) {
    json body;
    body["group_id"] = std::stoll(group_id);
    body["message"] = message;

    std::string response;
    return PostJson("send_group_msg", body.dump(), response);
}

bool OneBotApi::SendGroupMsgSegments(const std::string& group_id,
                                     const json& message_segments) {
    if (!message_segments.is_array() || message_segments.empty()) {
        return false;
    }
    json body;
    body["group_id"] = std::stoll(group_id);
    body["message"] = message_segments;

    std::string response;
    return PostJson("send_group_msg", body.dump(), response);
}

bool OneBotApi::GetMsg(const std::string& message_id, json& message) {
    json body;
    body["message_id"] = std::stoll(message_id);

    std::string response;
    if (!PostJson("get_msg", body.dump(), response)) {
        return false;
    }

    try {
        json resp = json::parse(response);
        if (resp.value("status", "") != "ok" || !resp.contains("data")) {
            return false;
        }
        message = resp["data"]["message"];
        return true;
    } catch (...) {
        return false;
    }
}

bool OneBotApi::GetGroupList(json& groups, std::string* error) {
    std::string response;
    if (!PostJson("get_group_list", "{\"no_cache\":true}", response, error)) return false;
    try {
        groups = json::parse(response).at("data");
        if (groups.is_array()) return true;
    } catch (...) {}
    if (error) *error = "get_group_list 响应格式错误：data 应为数组";
    return false;
}

bool OneBotApi::GetGroupHistory(const std::string& group_id, const std::string& cursor,
                               json& messages, std::string* error) {
    json body = {{"group_id", group_id}, {"count", 20}, {"reverse_order", false}};
    if (!cursor.empty()) body["message_seq"] = cursor;
    std::string response;
    if (!PostJson("get_group_msg_history", body.dump(), response, error)) return false;
    try {
        messages = json::parse(response).at("data").at("messages");
        if (messages.is_array()) return true;
    } catch (...) {}
    if (error) *error = "get_group_msg_history 响应格式错误：data.messages 应为数组";
    return false;
}

bool OneBotApi::GetFileBase64(const std::string& file_id, std::string& base64) {
    if (file_id.empty()) {
        return false;
    }

    json body;
    body["file_id"] = file_id;

    std::string response;
    if (!PostJson("get_file", body.dump(), response)) {
        return false;
    }

    try {
        const json resp = json::parse(response);
        if (resp.value("status", "") != "ok" || !resp.contains("data")
            || !resp["data"].is_object()) {
            return false;
        }
        base64 = resp["data"].value("base64", "");
        return !base64.empty();
    } catch (...) {
        return false;
    }
}

bool OneBotApi::GetImage(const std::string& file, json& data) {
    std::string response;
    if (file.empty() || !PostJson("get_image", json{{"file", file}}.dump(), response)) {
        return false;
    }
    try {
        const json parsed = json::parse(response);
        if (!parsed.contains("data") || !parsed["data"].is_object()) {
            return false;
        }
        data = parsed["data"];
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool OneBotApi::DownloadUrl(const std::string& url, std::string& data) {
    if (url.empty()) {
        return false;
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return false;
    }

    data.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    const CURLcode result = curl_easy_perform(curl);
    long http_code = 0;
    if (result == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || http_code < 200 || http_code >= 300 || data.empty()) {
        data.clear();
        return false;
    }
    return true;
}

bool OneBotApi::PostJson(const std::string& action, const std::string& body_json, std::string& response,
                         std::string* error) {
    if (error) error->clear();
    response.clear();
    const auto fail = [&](std::string reason) {
        if (!error) return;
        // 不输出请求、完整响应或 URL；服务端错误中若回显令牌则脱敏。
        if (!token_.empty()) {
            std::size_t pos = 0;
            while ((pos = reason.find(token_, pos)) != std::string::npos) {
                reason.replace(pos, token_.size(), "[REDACTED]");
                pos += 10;
            }
        }
        for (char& ch : reason) if (ch == '\r' || ch == '\n') ch = ' ';
        *error = action + ": " + reason.substr(0, 400);
    };
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        fail("无法初始化 HTTP 客户端");
        return false;
    }

    bool ok = false;
    std::string url = base_url_ + "/" + action;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!token_.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + token_).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) {
            // OneBot 响应格式: {"status":"ok", ...}
            try {
                json resp = json::parse(response);
                ok = (resp.value("status", "") == "ok");
                if (!ok) {
                    std::string detail = "OneBot 返回失败";
                    if (resp.contains("retcode") && resp["retcode"].is_number())
                        detail += "，retcode=" + resp["retcode"].dump();
                    for (const char* key : {"message", "wording"}) {
                        if (resp.contains(key) && resp[key].is_string())
                            detail += "，" + std::string(key) + "=" + resp[key].get<std::string>();
                    }
                    fail(detail);
                }
            } catch (...) {
                fail("HTTP 200，但响应不是有效的 OneBot JSON 对象");
            }
        } else {
            fail("HTTP " + std::to_string(http_code)
                + (http_code == 401 || http_code == 403 ? "，请检查 NapCat 访问令牌"
                   : http_code == 404 ? "，请检查正向 HTTP 地址及接口路径" : ""));
        }
    } else {
        fail("连接/传输失败，curl=" + std::to_string(static_cast<int>(result))
            + " (" + curl_easy_strerror(result) + ")"
            + (result == CURLE_COULDNT_CONNECT ? "，请确认 NapCat 已登录且正向 HTTP 服务已监听" : ""));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}
