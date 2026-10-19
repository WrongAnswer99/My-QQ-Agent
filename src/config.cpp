#include "config.hpp"

#include <fstream>

#include "json.hpp"

using json = nlohmann::json;

bool Config::LoadFromFile(const std::string& path, Config& out_config, std::string& error) {
    std::ifstream in(path);
    if (!in.is_open()) {
        error = "无法打开配置文件: " + path;
        return false;
    }

    try {
        json cfg = json::parse(in);
        out_config.napcat_http_base     = cfg.value("napcat_http_base", "http://127.0.0.1:3000");
        out_config.napcat_token         = cfg.value("napcat_token", "");
        out_config.http_report_port     = cfg.value("http_report_port", 8081);
        out_config.bot_qq               = cfg.value("bot_qq", "");
        out_config.master_qq            = cfg.value("master_qq", "");
        out_config.private_chat_enabled = cfg.value("private_chat_enabled", true);
        out_config.group_free_talk.clear();
        if (cfg.contains("group_free_talk")) {
            if (!cfg["group_free_talk"].is_array()) {
                error = "group_free_talk 必须是群号数组";
                return false;
            }
            for (const auto& value : cfg["group_free_talk"]) {
                std::string id;
                if (value.is_string()) id = value.get<std::string>();
                else if (value.is_number_unsigned()) id = std::to_string(value.get<std::uint64_t>());
                else if (value.is_number_integer() && value.get<std::int64_t>() > 0)
                    id = std::to_string(value.get<std::int64_t>());
                if (id.empty() || id.size() > 20 || id.find_first_not_of("0123456789") != std::string::npos
                    || id.find_first_not_of('0') == std::string::npos) {
                    error = "group_free_talk 的群号必须是正整数或数字字符串";
                    return false;
                }
                id.erase(0, id.find_first_not_of('0'));
                out_config.group_free_talk.push_back(id);
            }
        }

        const int group_history_limit = cfg.value("group_history_limit", 100);
        const int interaction_history_limit =
            cfg.value("group_interaction_history_limit", 30);
        if (group_history_limit < 0 || interaction_history_limit < 0) {
            error = "群聊历史数量配置不能为负数";
            return false;
        }
        out_config.group_history_limit = static_cast<std::size_t>(group_history_limit);
        out_config.group_interaction_history_limit =
            static_cast<std::size_t>(interaction_history_limit);
        out_config.startup_history_sync_enabled = cfg.value("startup_history_sync_enabled", true);
        out_config.startup_history_initial_hours = cfg.value("startup_history_initial_hours", 24);
        if (out_config.startup_history_initial_hours < 1
            || out_config.startup_history_initial_hours > 8760) {
            error = "startup_history_initial_hours 必须在 1 到 8760 之间";
            return false;
        }
        out_config.image_understanding_enabled = cfg.value("image_understanding_enabled", true);
        out_config.image_description_only_enabled = cfg.value("image_description_only_enabled", true);

        out_config.group_trigger_keywords.clear();
        if (cfg.contains("group_trigger_keywords") && cfg["group_trigger_keywords"].is_array()) {
            for (const auto& keyword : cfg["group_trigger_keywords"]) {
                out_config.group_trigger_keywords.push_back(keyword.get<std::string>());
            }
        }

        // AI 接口配置
        out_config.ai.local_varibles.clear();
        out_config.ai.free_talk_prompt = AiConfig{}.free_talk_prompt;
        if (cfg.contains("ai") && cfg["ai"].is_object()) {
            const json& ai = cfg["ai"];
            out_config.ai.api_key       = ai.value("api_key", "");
            out_config.ai.base_url      = ai.value("base_url", "");
            out_config.ai.model         = ai.value("model", "");
            out_config.ai.vision_model  = ai.value("vision_model", "");
            out_config.ai.system_prompt = ai.value("system_prompt", out_config.ai.system_prompt);
            out_config.ai.user_prompt   = ai.value("user_prompt", "");
            out_config.ai.free_talk_prompt = ai.value("free_talk_prompt", out_config.ai.free_talk_prompt);
            if (ai.contains("local_varibles")) {
                const json& variables = ai["local_varibles"];
                if (!variables.is_object()) {
                    error = "ai.local_varibles 必须是对象，变量值必须是字符串";
                    return false;
                }
                for (auto it = variables.begin(); it != variables.end(); ++it) {
                    if (it.key().empty() || it.key().find_first_of("[]") != std::string::npos
                        || !it.value().is_string()) {
                        error = "ai.local_varibles 的变量名不能为空或包含方括号，变量值必须是字符串";
                        return false;
                    }
                    out_config.ai.local_varibles.emplace(it.key(), it.value().get<std::string>());
                }
            }
        }
        return true;
    } catch (const std::exception& e) {
        error = "配置文件解析失败: ";
        error += e.what();
        return false;
    }
}

std::string Config::ExpandPromptVariables(const std::string& prompt) const {
    std::string expanded;
    expanded.reserve(prompt.size());
    std::size_t position = 0;
    while (position < prompt.size()) {
        const std::size_t start = prompt.find('[', position);
        if (start == std::string::npos) {
            expanded.append(prompt, position, std::string::npos);
            break;
        }
        expanded.append(prompt, position, start - position);
        const std::size_t end = prompt.find(']', start + 1);
        if (end == std::string::npos) {
            expanded.append(prompt, start, std::string::npos);
            break;
        }
        const auto variable = ai.local_varibles.find(prompt.substr(start + 1, end - start - 1));
        if (variable == ai.local_varibles.end()) {
            expanded.append(prompt, start, end - start + 1);
        } else {
            expanded += variable->second;
        }
        position = end + 1;
    }
    return expanded;
}
