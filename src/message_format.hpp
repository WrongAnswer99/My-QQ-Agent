#pragma once

#include <functional>
#include <string>

#include "json.hpp"

// 清理模型偶尔仿照历史输出的“[时间] 机器人(QQ):”前缀。
std::string CleanAIReply(const std::string& reply);

// 将回复中的 @ 标记和 [image编号] 转成 OneBot 11 消息段数组。
using ImageResolver = std::function<std::string(const std::string&)>;
nlohmann::json BuildGroupReplySegments(const std::string& reply,
                                      const ImageResolver& resolve_image = {});
