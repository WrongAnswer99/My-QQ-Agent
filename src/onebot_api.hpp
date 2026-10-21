#pragma once

#include <string>

#include "json.hpp"

// OneBot 11 HTTP API 客户端（基于 libcurl）
class OneBotApi {
public:
    explicit OneBotApi(const std::string& base_url, const std::string& token = "");
    ~OneBotApi();

    // 发送私聊消息，成功返回 true
    bool SendPrivateMsg(const std::string& user_id, const std::string& message);

    // 发送群聊消息，成功返回 true
    bool SendGroupMsg(const std::string& group_id, const std::string& message);

    // 发送 OneBot 消息段数组（用于真正的 @ 消息等）。
    bool SendGroupMsgSegments(const std::string& group_id,
                              const nlohmann::json& message_segments,
                              std::string* sent_message_id = nullptr);

    // 根据消息 id 获取消息内容（data.message 部分存入 message），成功返回 true
    bool GetMsg(const std::string& message_id, nlohmann::json& message);

    bool GetGroupList(nlohmann::json& groups, std::string* error = nullptr);
    bool GetGroupHistory(const std::string& group_id, const std::string& cursor,
                         nlohmann::json& messages, std::string* error = nullptr);

    // 通过 NapCat 文件标识获取文件的 Base64 数据。
    bool GetFileBase64(const std::string& file_id, std::string& base64);

    // 标准 OneBot get_image：将图片文件名解析为下载地址或本地路径。
    bool GetImage(const std::string& file, nlohmann::json& data);

    // 下载图片消息段提供的 HTTP(S) 地址，成功时返回二进制内容。
    bool DownloadUrl(const std::string& url, std::string& data);

private:
    // 调用任意 OneBot action，body_json 为动作参数，响应 JSON 存入 response
    bool PostJson(const std::string& action, const std::string& body_json, std::string& response,
                  std::string* error = nullptr);

    std::string base_url_;
    std::string token_;
};
