#include "group_history.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>
#include <regex>
#include <sstream>

#include "json.hpp"

using json = nlohmann::json;

namespace {

json EntryToJson(const GroupHistoryEntry& entry) {
    json value = {
        {"sequence", entry.sequence},
        {"message_id", entry.message_id},
        {"time", entry.timestamp},
        {"sender", entry.sender},
        {"text", entry.text},
        {"images", json::array()}
    };
    for (const GroupHistoryImage& image : entry.images) {
        value["images"].push_back({
            {"mime_type", image.mime_type},
            {"base64", image.base64},
            {"description", image.description},
            {"id", image.id},
            {"text_offset", std::min(image.text_offset, entry.text.size())}
        });
    }
    return value;
}

bool EntryFromJson(const json& value, GroupHistoryEntry& entry) {
    if (!value.is_object() || !value.contains("sequence") || !value.contains("time")
        || !value.contains("sender") || !value.contains("text")) {
        return false;
    }
    try {
        entry.sequence = value.at("sequence").get<std::uint64_t>();
        entry.timestamp = value.at("time").get<std::int64_t>();
        entry.sender = value.at("sender").get<std::string>();
        entry.text = value.at("text").get<std::string>();
        entry.message_id = value.value("message_id", "");
        entry.images.clear();
        if (value.contains("images") && value["images"].is_array()) {
            for (const json& image_value : value["images"]) {
                if (!image_value.is_object()) {
                    continue;
                }
                const std::string mime_type = image_value.value("mime_type", "");
                const std::string base64 = image_value.value("base64", "");
                const std::string description = image_value.value("description", "");
                const std::string id = image_value.value("id", "");
                if (!base64.empty() || !description.empty() || !id.empty()) {
                    entry.images.push_back({
                        mime_type.empty() ? "image/jpeg" : mime_type,
                        base64, description, id,
                        std::min(image_value.value("text_offset", entry.text.size()), entry.text.size())
                    });
                }
            }
        }
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

// v2 描述模式曾将自动描述追加到正文。旧记录无法恢复精确位置，放在原文末尾。
void MigrateLegacyDescriptions(GroupHistoryEntry& entry, const std::string& group_id,
                               const std::filesystem::path& cache_directory) {
    if (!entry.images.empty()) return;
    static const std::regex header(
        "\\n【图片 [0-9]+ 的自动描述（仅作历史参考）】\\n"
        "(?:原图编号：image([0-9]{1,20})，发送标记：\\[image[0-9]+\\]|"
        "\\[原图缓存失败，无法发送原图\\])\\n");
    const std::string original = entry.text;
    std::vector<std::smatch> matches;
    for (std::sregex_iterator it(original.begin(), original.end(), header), end; it != end; ++it) {
        matches.push_back(*it);
    }
    if (matches.empty()) return;
    entry.text = original.substr(0, matches.front().position());
    if (entry.text == "[图片消息]") entry.text.clear();
    for (std::size_t i = 0; i < matches.size(); ++i) {
        GroupHistoryImage image;
        image.id = matches[i][1].str();
        image.text_offset = entry.text.size();
        const std::size_t start = matches[i].position() + matches[i].length();
        const std::size_t end = i + 1 < matches.size() ? matches[i + 1].position() : original.size();
        image.description = original.substr(start, end - start);
        if (!image.id.empty() && !group_id.empty()
            && group_id.find_first_not_of("0123456789") == std::string::npos) {
            std::ifstream input(cache_directory / group_id / (image.id + ".b64"), std::ios::binary);
            std::ostringstream bytes;
            bytes << input.rdbuf();
            image.base64 = bytes.str();
        }
        image.mime_type = image.base64.compare(0, 5, "iVBOR") == 0 ? "image/png"
            : image.base64.compare(0, 4, "R0lG") == 0 ? "image/gif"
            : image.base64.compare(0, 4, "UklG") == 0 ? "image/webp" : "image/jpeg";
        entry.images.push_back(std::move(image));
    }
}

} // namespace

GroupHistoryStore::GroupHistoryStore(std::size_t message_limit,
                                     std::size_t interaction_limit,
                                     std::string file_path)
    : message_limit_(message_limit),
      interaction_limit_(interaction_limit),
      file_path_(std::move(file_path)) {
    std::error_code error;
    const std::filesystem::path parent = std::filesystem::path(file_path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
    }
}

bool GroupHistoryStore::Load(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream input(file_path_);
    if (!input.is_open()) {
        return true;
    }

    try {
        const json root = json::parse(input);
        if (!root.is_object() || !root.contains("groups") || !root["groups"].is_object()) {
            error = "群聊历史文件格式无效: " + file_path_;
            return false;
        }

        groups_.clear();
        next_sequence_ = 1;
        for (auto group_it = root["groups"].begin(); group_it != root["groups"].end(); ++group_it) {
            if (!group_it.value().is_object()) {
                continue;
            }
            PerGroupHistory history;
            const json& group_json = group_it.value();
            history.synced_until = group_json.value("synced_until", std::int64_t{0});
            if (group_json.contains("seen") && group_json["seen"].is_object()) {
                history.seen = group_json["seen"].get<std::unordered_map<std::string, std::int64_t>>();
            }
            const auto load_entries = [this, &group_it, &root](const json& values,
                                             std::deque<GroupHistoryEntry>& destination,
                                             std::size_t limit) {
                if (!values.is_array()) {
                    return;
                }
                for (const json& value : values) {
                    GroupHistoryEntry entry;
                    if (!EntryFromJson(value, entry)) {
                        continue;
                    }
                    if (root.value("version", 1) < 3) {
                        MigrateLegacyDescriptions(entry, group_it.key(),
                            std::filesystem::path(file_path_).parent_path() / "image_cache");
                    }
                    next_sequence_ = std::max(next_sequence_, entry.sequence + 1);
                    PushAndTrim(destination, std::move(entry), limit);
                }
            };
            load_entries(group_json.value("messages", json::array()),
                         history.messages, message_limit_);
            load_entries(group_json.value("mentions", json::array()),
                         history.mentions, interaction_limit_);
            load_entries(group_json.value("replies", json::array()),
                         history.replies, interaction_limit_);
            groups_.emplace(group_it.key(), std::move(history));
        }
        input.close();
        if (!SaveToFileLocked()) {
            error = "群聊历史裁剪后无法写回: " + file_path_;
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = "群聊历史加载失败: " + file_path_ + "，" + exception.what();
        return false;
    }
}

GroupHistorySnapshot GroupHistoryStore::RecordIncoming(const std::string& group_id,
                                                        std::int64_t timestamp,
                                                        const std::string& sender,
                                                        const std::string& text,
                                                        const std::vector<GroupHistoryImage>& images,
                                                        bool mentions_bot,
                                                        const std::string& message_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    PerGroupHistory& history = groups_[group_id];

    GroupHistorySnapshot snapshot;
    snapshot.messages.assign(history.messages.begin(), history.messages.end());
    snapshot.mentions.assign(history.mentions.begin(), history.mentions.end());
    snapshot.replies.assign(history.replies.begin(), history.replies.end());

    if (!message_id.empty()) {
        if (history.seen.count(message_id)) return snapshot;
        history.seen[message_id] = timestamp;
        // 老版本没有平台 ID，认领相同时间、发送者和正文的记录，避免升级后重复。
        bool adopted = false;
        for (auto* entries : {&history.messages, &history.mentions, &history.replies}) {
            for (auto& old : *entries) {
                if (old.timestamp == timestamp && old.sender == sender && old.text == text
                    && (old.message_id.empty() || old.message_id == message_id)) {
                    old.message_id = message_id;
                    adopted = true;
                    break;
                }
            }
        }
        if (adopted) {
            SaveToFileLocked();
            return snapshot;
        }
    }
    GroupHistoryEntry entry{
        next_sequence_++, timestamp, sender, text, images, message_id
    };
    PushAndTrim(history.messages, entry, message_limit_);
    if (mentions_bot) {
        PushAndTrim(history.mentions, std::move(entry), interaction_limit_);
    }
    if (!SaveToFileLocked()) {
        std::cerr << "[群聊历史保存失败] " << file_path_ << std::endl;
    }
    return snapshot;
}

bool GroupHistoryStore::Contains(const std::string& group_id, const std::string& message_id) {
    if (message_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = groups_.find(group_id);
    if (found == groups_.end()) return false;
    const auto& history = found->second;
    if (history.seen.count(message_id)) return true;
    for (const auto* entries : {&history.messages, &history.mentions, &history.replies}) {
        for (const auto& entry : *entries) if (entry.message_id == message_id) return true;
    }
    return false;
}

std::int64_t GroupHistoryStore::SyncSince(const std::string& group_id, std::int64_t fallback) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& history = groups_[group_id];
    if (history.synced_until == 0) {
        history.synced_until = fallback;
        SaveToFileLocked(); // 失败或中断后仍使用相同起点
    }
    return history.synced_until;
}

void GroupHistoryStore::MarkSynced(const std::string& group_id, std::int64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& history = groups_[group_id];
    history.synced_until = std::max(history.synced_until, timestamp);
    for (auto it = history.seen.begin(); it != history.seen.end();) {
        if (it->second < history.synced_until) it = history.seen.erase(it);
        else ++it;
    }
    if (!SaveToFileLocked()) std::cerr << "[同步进度保存失败] " << file_path_ << std::endl;
}

void GroupHistoryStore::RecordBotReply(const std::string& group_id,
                                       std::int64_t timestamp,
                                       const std::string& bot_sender,
                                       const std::string& text,
                                       bool reply_to_mention) {
    std::lock_guard<std::mutex> lock(mutex_);
    PerGroupHistory& history = groups_[group_id];
    GroupHistoryEntry entry{next_sequence_++, timestamp, bot_sender, text, {}};
    PushAndTrim(history.messages, entry, message_limit_);
    if (reply_to_mention) {
        PushAndTrim(history.replies, std::move(entry), interaction_limit_);
    }
    if (!SaveToFileLocked()) {
        std::cerr << "[群聊历史保存失败] " << file_path_ << std::endl;
    }
}

void GroupHistoryStore::PushAndTrim(std::deque<GroupHistoryEntry>& entries,
                                    GroupHistoryEntry entry,
                                    std::size_t limit) {
    if (limit == 0) {
        entries.clear();
        return;
    }
    const auto position = std::upper_bound(entries.begin(), entries.end(), entry.timestamp,
        [](std::int64_t time, const GroupHistoryEntry& item) { return time < item.timestamp; });
    entries.insert(position, std::move(entry));
    while (entries.size() > limit) {
        entries.pop_front();
    }
}

bool GroupHistoryStore::SaveToFileLocked() const {
    json root;
    root["version"] = 4;
    root["groups"] = json::object();
    for (const auto& group_pair : groups_) {
        json group_json;
        group_json["synced_until"] = group_pair.second.synced_until;
        group_json["seen"] = group_pair.second.seen;
        group_json["messages"] = json::array();
        group_json["mentions"] = json::array();
        group_json["replies"] = json::array();
        for (const GroupHistoryEntry& entry : group_pair.second.messages) {
            group_json["messages"].push_back(EntryToJson(entry));
        }
        for (const GroupHistoryEntry& entry : group_pair.second.mentions) {
            group_json["mentions"].push_back(EntryToJson(entry));
        }
        for (const GroupHistoryEntry& entry : group_pair.second.replies) {
            group_json["replies"].push_back(EntryToJson(entry));
        }
        root["groups"][group_pair.first] = std::move(group_json);
    }

    std::ofstream output(file_path_, std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output << root.dump(2);
    return output.good();
}
