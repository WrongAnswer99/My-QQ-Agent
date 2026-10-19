#include <filesystem>
#include <iostream>
#include <string>

#include "group_history.hpp"
#include "message_format.hpp"
#include "json.hpp"
#include "message_format.hpp"

using json = nlohmann::json;

namespace {

bool Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[失败] " << message << std::endl;
    }
    return condition;
}

} // namespace

int main() {
    const std::string file_path = "test_group_history.json";
    std::error_code filesystem_error;
    std::filesystem::remove(file_path, filesystem_error);

    {
        // 先用较大的上限写入，再用较小上限加载，验证配置变更后也会裁剪。
        GroupHistoryStore history(10, 10, true, file_path);
        history.RecordIncoming("123", 1, "用户A", "A", {}, true);
        history.RecordBotReply("123", 2, "机器人", "回复A", true);
        history.RecordIncoming("123", 3, "用户B", "B",
                               {{"image/png", "iVBORw0KGgo="}}, true);
        history.RecordBotReply("123", 4, "机器人", "回复B", true);
        history.RecordIncoming("123", 5, "用户C", "C",
                               {{"image/jpeg", "/9j/AA=="}}, true);
        history.RecordBotReply("123", 6, "机器人", "回复C", true);
    }

    GroupHistoryStore restored(3, 2, true, file_path);
    std::string error;
    if (!Check(restored.Load(error), "无法重新加载 JSON：" + error)) {
        return 1;
    }
    const GroupHistorySnapshot snapshot =
        restored.RecordIncoming("123", 7, "用户D", "D", {}, true);

    bool passed = true;
    passed &= Check(snapshot.messages.size() == 3, "普通群聊历史没有裁剪到 3 条");
    passed &= Check(snapshot.messages.size() < 3 || snapshot.messages.front().text == "回复B",
                    "普通群聊历史没有丢弃最旧记录");
    passed &= Check(snapshot.mentions.size() == 2, "@ 历史没有裁剪到 2 条");
    passed &= Check(snapshot.mentions.size() < 2 || snapshot.mentions.front().text == "B",
                    "@ 历史没有丢弃最旧记录");
    passed &= Check(snapshot.mentions.size() < 2
                        || (snapshot.mentions.front().images.size() == 1
                            && snapshot.mentions.front().images.front().base64 == "iVBORw0KGgo="),
                    "图片 Base64 没有从 JSON 正确恢复");
    passed &= Check(snapshot.replies.size() == 2, "回复历史没有裁剪到 2 条");
    passed &= Check(snapshot.replies.size() < 2 || snapshot.replies.front().text == "回复B",
                    "回复历史没有丢弃最旧记录");

    GroupHistoryStore images_disabled(3, 2, false, file_path);
    std::string disabled_error;
    passed &= Check(images_disabled.Load(disabled_error),
                    "关闭图片后无法加载 JSON：" + disabled_error);
    const GroupHistorySnapshot without_images =
        images_disabled.RecordIncoming("123", 8, "用户E", "E", {}, true);
    const auto contains_image = [](const std::vector<GroupHistoryEntry>& entries) {
        for (const GroupHistoryEntry& entry : entries) {
            if (!entry.images.empty()) {
                return true;
            }
        }
        return false;
    };
    passed &= Check(!contains_image(without_images.messages)
                        && !contains_image(without_images.mentions)
                        && !contains_image(without_images.replies),
                    "关闭图片历史后仍加载了 Base64 数据");

    const std::string prefixed_reply =
        "[2026-09-15 16:57:25] 机器人(QQ:1000000001): 回复正文";
    passed &= Check(CleanAIReply(prefixed_reply) == "回复正文",
                    "没有正确清理模型仿写的时间和机器人前缀");

    const json segments = BuildGroupReplySegments(
        "你好 [@1000000002]，也叫一下 @1241183351！");
    passed &= Check(segments.size() == 5, "@ 回复没有拆成正确数量的 OneBot 消息段");
    passed &= Check(segments.size() < 2
                        || (segments[1].value("type", "") == "at"
                            && segments[1]["data"].value("qq", "") == "1000000002"),
                    "方括号 @ 没有转换为 OneBot at 消息段");
    passed &= Check(segments.size() < 4
                        || (segments[3].value("type", "") == "at"
                            && segments[3]["data"].value("qq", "") == "1241183351"),
                    "普通文字 @ 没有转换为 OneBot at 消息段");

    std::filesystem::remove(file_path, filesystem_error);
    if (!passed) {
        return 1;
    }
    std::cout << "群聊 JSON 滚动历史测试通过" << std::endl;
    return 0;
}
