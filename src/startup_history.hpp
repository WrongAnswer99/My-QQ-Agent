#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <set>
#include <string>

#include "onebot_api.hpp"

// 每页 20 条只是传输批量；一直回溯到 since，不限制离线消息总数。
// import_message 必须是静默入口。在线消息留给 HTTP 上报处理。
inline bool FetchStartupHistory(OneBotApi& api, const std::string& group_id,
                                std::int64_t since, std::int64_t startup_time,
                                const std::function<void(const nlohmann::json&)>& import_message,
                                std::string* error = nullptr) {
    using json = nlohmann::json;
    const auto scalar = [](const json& value) -> std::string {
        if (value.is_string()) return value.get<std::string>();
        if (value.is_number_integer()) return value.dump();
        return {};
    };
    if (error) error->clear();
    std::string cursor;
    std::set<std::string> cursors;
    while (true) {
        json messages;
        if (!api.GetGroupHistory(group_id, cursor, messages, error)) return false;
        if (messages.empty()) return true;
        // NapCat 默认按时间从旧到新返回；排序同时容忍倒序返回。
        std::stable_sort(messages.begin(), messages.end(), [](const json& a, const json& b) {
            return a.value("time", std::int64_t{0}) < b.value("time", std::int64_t{0});
        });
        bool reached = false;
        for (const auto& event : messages) {
            if (!event.contains("time") || !event["time"].is_number_integer()
                || !event.contains("message") || !event["message"].is_array()
                || !event.contains("message_id") || scalar(event["message_id"]).empty()) {
                if (error) *error = "历史消息字段无效，请将 NapCat 正向 HTTP 消息格式设为 array";
                return false; // 不把不支持的消息格式当成同步成功
            }
            const auto timestamp = event["time"].get<std::int64_t>();
            if (timestamp < since) reached = true;
            if (timestamp >= since && timestamp < startup_time) import_message(event);
        }
        if (reached) return true;
        // 参数虽然名为 message_seq，官方实现会把 OneBot message_id 映射为原始 msgId。
        const std::string next = scalar(messages.front()["message_id"]);
        if (next.empty() || next == cursor || !cursors.insert(next).second) {
            if (error) *error = "分页未返回更早消息，尚未到时间边界；已取得的消息已静默入库";
            return false;
        }
        cursor = next;
        // 不根据不足 20 条判定结束，QQ 可能返回短页。
    }
}
