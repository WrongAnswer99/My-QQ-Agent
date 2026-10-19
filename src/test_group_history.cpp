#include <filesystem>
#include <iostream>
#include <fstream>
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
        GroupHistoryStore history(10, 10, file_path);
        history.RecordIncoming("123", 1, "用户A", "A", {}, true);
        history.RecordBotReply("123", 2, "机器人", "回复A", true);
        history.RecordIncoming("123", 3, "用户B", "B",
                               {{"image/png", "iVBORw0KGgo=", "白色测试图片", "123456", 0}}, true);
        history.RecordBotReply("123", 4, "机器人", "回复B", true);
        history.RecordIncoming("123", 5, "用户C", "C",
                               {{"image/jpeg", "/9j/AA=="}}, true);
        history.RecordBotReply("123", 6, "机器人", "回复C", true);
    }

    GroupHistoryStore restored(3, 2, file_path);
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

    passed &= Check(snapshot.mentions.size() == 2
                        && snapshot.mentions.front().images.size() == 1
                        && snapshot.mentions.front().images.front().description == "白色测试图片"
                        && snapshot.mentions.front().images.front().id == "123456"
                        && snapshot.mentions.front().images.front().text_offset == 0,
                    "图片描述、编号或位置没有正确恢复");

    // 旧版描述迁移：从同目录缓存恢复原图，没有缓存也保留描述及编号。
    const std::string legacy_path = "test_history_migration/history.json";
    std::filesystem::create_directories("test_history_migration/image_cache/123");
    std::ofstream("test_history_migration/image_cache/123/456.b64") << "iVBORw0KGgo=";
    const json old_entry = {{"sequence", 1}, {"time", 1}, {"sender", "用户"},
        {"text", "看看\n【图片 1 的自动描述（仅作历史参考）】\n"
                 "原图编号：image456，发送标记：[image456]\n旧图片描述\n"
                 "【图片 2 的自动描述（仅作历史参考）】\n"
                 "原图编号：image789，发送标记：[image789]\n第二张描述"},
        {"images", json::array()}};
    std::ofstream(legacy_path) << json({{"version", 2}, {"groups", {
        {"123", {{"messages", json::array({old_entry})}}}}}}).dump();
    {
        GroupHistoryStore legacy(10, 10, legacy_path);
        passed &= Check(legacy.Load(error), "旧描述加载失败：" + error);
        const auto migrated = legacy.RecordIncoming("123", 2, "用户", "新消息", {}, false);
        passed &= Check(migrated.messages.size() == 1
                            && migrated.messages[0].text == "看看"
                            && migrated.messages[0].images.size() == 2
                            && migrated.messages[0].images[0].base64 == "iVBORw0KGgo="
                            && migrated.messages[0].images[0].description == "旧图片描述"
                            && migrated.messages[0].images[0].id == "456"
                            && migrated.messages[0].images[0].text_offset == std::string("看看").size()
                            && migrated.messages[0].images[1].description == "第二张描述"
                            && migrated.messages[0].images[1].base64.empty(),
                        "旧版描述没有从正文正确迁移为图片数据");
    }
    {
        GroupHistoryStore again(10, 10, legacy_path);
        passed &= Check(again.Load(error), "迁移后重新加载失败：" + error);
        const auto snapshot = again.RecordIncoming("123", 3, "用户", "再次加载", {}, false);
        passed &= Check(snapshot.messages.size() == 2
                            && snapshot.messages[0].images.size() == 2
                            && snapshot.messages[0].text == "看看",
                        "迁移结果重新加载时丢失或重复");
    }
    std::filesystem::remove_all("test_history_migration", filesystem_error);

    // 离线补取晚于实时入库，仍按消息时间排序；ID 和边界跨重启保留。
    {
        GroupHistoryStore sync(3, 2, file_path);
        sync.SyncSince("7", 10);
        sync.RecordIncoming("7", 30, "用户", "实时", {}, false, "30");
        sync.RecordIncoming("7", 20, "用户", "历史", {}, true, "20");
        sync.RecordIncoming("7", 10, "用户", "最旧", {}, false, "10");
        const auto before = sync.RecordIncoming("7", 20, "用户", "历史", {}, true, "20");
        passed &= Check(before.messages.size() == 3 && before.messages.front().timestamp == 10
            && before.messages.back().timestamp == 30, "补取排序或消息 ID 去重错误");
        sync.MarkSynced("7", 20);
        GroupHistoryStore restart(3, 2, file_path);
        passed &= Check(restart.Load(error) && restart.Contains("7", "20")
            && restart.Contains("7", "30") && restart.SyncSince("7", 999) == 20,
            "重启未保留 ID 或同步进度");
    }

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
