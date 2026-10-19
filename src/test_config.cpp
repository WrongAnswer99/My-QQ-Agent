#include "config.hpp"
#include "image_cache.hpp"
#include "message_format.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using json = nlohmann::json;

static void Check(bool condition, const char* label) {
    if (!condition) throw std::runtime_error(label);
}

int main() {
    const auto root = std::filesystem::temp_directory_path()
        / ("qq-config-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    try {
        Config config;
        config.ai.local_varibles = {{"bot_name", "测试机器人"}, {"greeting", "你好"},
                                   {"empty", ""}, {"nested", "[bot_name]"}};
        Check(config.ExpandPromptVariables("[greeting][bot_name]/[bot_name]")
              == "你好测试机器人/测试机器人", "multiple variables");
        const std::string literal = "[YYYY-MM-DD HH:MM:SS][@123456789][image123][unknown]";
        Check(config.ExpandPromptVariables(literal) == literal, "undefined preserved");
        Check(config.ExpandPromptVariables("[empty][nested]") == "[bot_name]", "single pass");
        Check(config.ExpandPromptVariables("[Bot_name][open") == "[Bot_name][open", "literal fragments");
        Check(config.ExpandPromptVariables("").empty(), "empty prompt");
        const auto config_path = root / "config.json";
        const auto load = [&](const json& data) {
            std::ofstream output(config_path);
            output << data.dump();
            output.close();
            std::string error;
            return Config::LoadFromFile(config_path.string(), config, error);
        };
        Check(load({{"ai", {{"local_varibles", {{"bot_name", "name"}}}}}}), "load variables");
        Check(config.ExpandPromptVariables("[bot_name]") == "name", "loaded substitution");
        Check(config.image_description_only_enabled, "default description mode");
        Check(config.startup_history_sync_enabled && config.startup_history_initial_hours == 24,
              "startup sync defaults");
        Check(load({{"startup_history_sync_enabled", false}, {"startup_history_initial_hours", 48}})
              && !config.startup_history_sync_enabled && config.startup_history_initial_hours == 48,
              "startup sync settings");
        for (const auto invalid : {0, -1, 8761}) {
            Check(!load({{"startup_history_initial_hours", invalid}}), "invalid initial window");
        }
        Check(load({{"image_description_only_enabled", false}}), "raw image mode");
        Check(!config.image_description_only_enabled, "inverted flag");
        Check(config.ExpandPromptVariables("[bot_name]") == "[bot_name]", "clear missing variables");
        Check(!load({{"ai", {{"local_varibles", json::array()}}}}), "reject array");
        Check(!load({{"ai", {{"local_varibles", {{"x", 1}}}}}}), "reject non-string");

        Check(load({{"group_free_talk", {"0007", 8}}, {"ai", {{"free_talk_prompt", "判断[bot_name][unknown]"},
              {"local_varibles", {{"bot_name", "name"}}}}}}), "load free talk");
        Check(config.group_free_talk == std::vector<std::string>({"7", "8"}), "normalize group ids");
        Check(config.ExpandPromptVariables(config.ai.free_talk_prompt) == "判断name[unknown]", "decision variables");
        Check(load(json::object()) && config.group_free_talk.empty(), "free talk defaults off");
        Check(!config.ai.free_talk_prompt.empty(), "default decision prompt");
        Check(!load({{"group_free_talk", "7"}}), "reject non-array groups");
        for (const json& invalid : json::array({0, -1, 1.5, true, "", "abc", "../7"})) {
            Check(!load({{"group_free_talk", json::array({invalid})}}), "reject invalid group id");
        }

        const auto directory = (root / "images").string();
        ImageCache cache(directory);
        const std::string id = cache.Store("7", "aGVsbG8=");
        const std::string id2 = cache.Store("7", "d29ybGQ=");
        Check(!id.empty() && id != id2, "unique identifiers");
        ImageCache restarted(directory);
        Check(restarted.Resolve("7", id) == "base64://aGVsbG8=", "restart retrieval");
        Check(restarted.Resolve("8", id).empty(), "group isolation");
        Check(restarted.Resolve("../7", id).empty(), "path validation");
        Check(restarted.Resolve("7", "../x").empty(), "identifier validation");
        const ImageResolver resolve = [&](const std::string& number) { return restarted.Resolve("7", number); };
        const auto segments = BuildGroupReplySegments("前[@123456789][image" + id + "]中[image" + id2 + "]后", resolve);
        Check(segments.size() == 6 && segments[1]["type"] == "at", "mixed mention and image");
        Check(segments[2]["data"]["file"] == "base64://aGVsbG8=", "first image");
        Check(segments[4]["data"]["file"] == "base64://d29ybGQ=", "second image");
        Check(BuildGroupReplySegments("[image0]", resolve)[0]["data"]["text"] == "[原图不可用]", "missing image");
        Check(BuildGroupReplySegments("[imageX]", resolve)[0]["data"]["text"] == "[imageX]", "literal marker");
        Check(BuildGroupReplySegments("[image123]")[0]["type"] == "text", "disabled resolver");
        std::filesystem::remove_all(root);
        std::cout << "PASS: config variables, image cache and reply formatting\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
