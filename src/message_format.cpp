#include "message_format.hpp"

#include <cctype>
#include <string>

using json = nlohmann::json;

namespace {

bool IsTimestampPrefix(const std::string& text, std::size_t close_bracket) {
    // [YYYY-MM-DD HH:MM:SS]
    if (close_bracket != 20 || text.size() <= close_bracket) {
        return false;
    }
    constexpr std::size_t separators[] = {5, 8, 11, 14, 17};
    constexpr char expected[] = {'-', '-', ' ', ':', ':'};
    for (std::size_t index = 1; index < close_bracket; ++index) {
        bool is_separator = false;
        for (std::size_t separator_index = 0; separator_index < 5; ++separator_index) {
            if (index == separators[separator_index]) {
                if (text[index] != expected[separator_index]) {
                    return false;
                }
                is_separator = true;
                break;
            }
        }
        if (!is_separator && !std::isdigit(static_cast<unsigned char>(text[index]))) {
            return false;
        }
    }
    return true;
}

bool ParseMentionAt(const std::string& text, std::size_t start,
                    std::size_t& end, std::string& qq) {
    const bool bracketed = start + 1 < text.size() && text[start] == '[' && text[start + 1] == '@';
    if (!bracketed && text[start] != '@') {
        return false;
    }

    std::size_t cursor = start + (bracketed ? 2 : 1);
    const std::size_t digits_start = cursor;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    const std::size_t digit_count = cursor - digits_start;
    if (digit_count < 5 || digit_count > 12) {
        return false;
    }
    if (bracketed) {
        if (cursor >= text.size() || text[cursor] != ']') {
            return false;
        }
        end = cursor + 1;
    } else {
        end = cursor;
    }
    qq = text.substr(digits_start, digit_count);
    return true;
}

void PushTextSegment(json& segments, const std::string& text) {
    if (!text.empty()) {
        segments.push_back({{"type", "text"}, {"data", {{"text", text}}}});
    }
}

} // namespace

std::string CleanAIReply(const std::string& reply) {
    if (reply.empty() || reply.front() != '[') {
        return reply;
    }
    const std::size_t close_bracket = reply.find(']');
    if (close_bracket == std::string::npos || !IsTimestampPrefix(reply, close_bracket)) {
        return reply;
    }

    std::size_t sender_start = close_bracket + 1;
    while (sender_start < reply.size() && reply[sender_start] == ' ') {
        ++sender_start;
    }
    if (reply.compare(sender_start, std::string("机器人").size(), "机器人") != 0) {
        return reply;
    }
    const std::size_t separator = reply.find(": ", sender_start);
    if (separator == std::string::npos) {
        return reply;
    }
    return reply.substr(separator + 2);
}

json BuildGroupReplySegments(const std::string& reply, const ImageResolver& resolve_image) {
    json segments = json::array();
    std::size_t text_start = 0;
    std::size_t cursor = 0;
    while (cursor < reply.size()) {
        if (resolve_image && reply.compare(cursor, 6, "[image") == 0) {
            std::size_t end = cursor + 6;
            while (end < reply.size() && reply[end] >= '0' && reply[end] <= '9') {
                ++end;
            }
            if (end > cursor + 6 && end < reply.size() && reply[end] == ']') {
                PushTextSegment(segments, reply.substr(text_start, cursor - text_start));
                const std::string file = resolve_image(reply.substr(cursor + 6, end - cursor - 6));
                if (file.empty()) {
                    PushTextSegment(segments, "[原图不可用]");
                } else {
                    segments.push_back({{"type", "image"}, {"data", {{"file", file}}}});
                }
                cursor = end + 1;
                text_start = cursor;
                continue;
            }
        }
        const bool possible_mention = reply[cursor] == '@'
            || (reply[cursor] == '[' && cursor + 1 < reply.size() && reply[cursor + 1] == '@');
        if (!possible_mention) {
            ++cursor;
            continue;
        }

        std::size_t mention_end = cursor;
        std::string qq;
        if (!ParseMentionAt(reply, cursor, mention_end, qq)) {
            ++cursor;
            continue;
        }
        PushTextSegment(segments, reply.substr(text_start, cursor - text_start));
        segments.push_back({{"type", "at"}, {"data", {{"qq", qq}}}});
        cursor = mention_end;
        text_start = mention_end;
    }
    PushTextSegment(segments, reply.substr(text_start));
    if (segments.empty()) {
        PushTextSegment(segments, reply);
    }
    return segments;
}
