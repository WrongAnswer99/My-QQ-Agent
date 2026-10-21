#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "json.hpp"

// 清理模型偶尔仿照历史输出的“[时间] 机器人(QQ):”前缀。
std::string CleanAIReply(const std::string& reply);

// 消费可选的 [&本地编号]，仅允许引用本次上下文映射中的真实消息。
// 多个、未闭合、越界或没有实际消息 ID 的标记均拒绝；不会发出原始控制标记。
bool ExtractReplyReference(std::string& text,
                           const std::unordered_map<std::string, std::string>& references,
                           std::string& message_id);

// 将回复中的 @ 标记和 [image编号] 转成 OneBot 11 消息段数组。
using ImageResolver = std::function<std::string(const std::string&)>;
nlohmann::json BuildGroupReplySegments(const std::string& reply,
                                      const ImageResolver& resolve_image = {});
