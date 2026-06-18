#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <regex>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <ctime>
#include <vector>
#include <cstdlib>
#include <sys/stat.h>


std::string BOT_TOKEN;
std::string CHAT_ID;


const float TEMP_ALARM = 50.0f;
const float TEMP_WARNING = 45.0f;
const auto REPORT_INTERVAL = std::chrono::minutes(5);
const auto ELEVATED_REPORT_INTERVAL = std::chrono::seconds(15);
const auto SENSOR_INTERVAL = std::chrono::seconds(15);
const std::string BOT_STATE_FILE = "bot_state.json";
const std::string RENDER_DIR = "runtime";
const std::string DASHBOARD_HTML_FILE = RENDER_DIR + "/temperature_dashboard.html";
const std::string DASHBOARD_IMAGE_FILE = RENDER_DIR + "/temperature_dashboard.jpg";
const std::string DASHBOARD_IMAGE_TMP_FILE = RENDER_DIR + "/temperature_dashboard.tmp.jpg";

//Фун-ции

struct ChatBotState {
    long live_dashboard_message_id = -1;
    long last_alert_text_message_id = -1;
    long language_prompt_message_id = -1;
    std::string language;
};

struct BotState {
    std::map<std::string, ChatBotState> chats;
    long last_update_id = -1;
};

struct TelegramApiResult {
    CURLcode curlCode = CURLE_OK;
    bool transportOk = false;
    long httpStatus = 0;
    bool apiOk = false;
    std::string response;
    long messageId = -1;
};

enum class EditOutcome {
    Success,
    PermanentFailure,
    TransientFailure
};

enum class DeleteOutcome {
    Success,
    PermanentFailure,
    TransientFailure
};

enum class TemperatureState {
    Unknown,
    Normal,
    Elevated,
    High
};

BotState g_botState;

ChatBotState& currentChatState();

std::string escape(const std::string& text) {
    std::string out;
    std::string spec = "_*[]()~`>#+-=|{}.!";
    for (char c : text) {
        if (spec.find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return out;
}

float getCPUTemperature() {
    std::ifstream file("/sys/class/thermal/thermal_zone0/temp");
    int temp_milli = 0;
    if (file >> temp_milli) return temp_milli / 1000.0f;
    return -1.0f;
}

size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool containsText(const std::string& text, const std::string& needle) {
    return toLower(text).find(toLower(needle)) != std::string::npos;
}

bool extractOkFlag(const std::string& response) {
    std::regex re("\"ok\"\\s*:\\s*true");
    return std::regex_search(response, re);
}

long extractMessageId(const std::string& response) {
    std::regex re("\"message_id\"\\s*:\\s*(\\d+)");
    std::smatch match;
    if (std::regex_search(response, match, re) && match.size() > 1) {
        return std::stol(match[1].str());
    }
    return -1;
}

std::string curlEscape(CURL* curl, const std::string& value) {
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (!encoded) return "";
    std::string result(encoded);
    curl_free(encoded);
    return result;
}

using TelegramField = std::pair<std::string, std::string>;

std::string buildPostFields(CURL* curl, std::initializer_list<TelegramField> fields) {
    std::string result;
    bool first = true;
    for (const auto& field : fields) {
        if (!first) result += "&";
        first = false;
        result += curlEscape(curl, field.first);
        result += "=";
        result += curlEscape(curl, field.second);
    }
    return result;
}

TelegramApiResult callTelegramApi(const std::string& method, std::initializer_list<TelegramField> fields) {
    TelegramApiResult result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.curlCode = CURLE_FAILED_INIT;
        return result;
    }

    std::string response;
    std::string url = "https://api.telegram.org/bot" + BOT_TOKEN + "/" + method;
    std::string postFields = buildPostFields(curl, fields);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    result.curlCode = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
    curl_easy_cleanup(curl);

    result.transportOk = (result.curlCode == CURLE_OK);
    result.response = response;
    result.apiOk = extractOkFlag(response);
    result.messageId = extractMessageId(response);
    return result;
}

TelegramApiResult callTelegramMultipartApi(
    const std::string& method,
    std::initializer_list<TelegramField> fields,
    const std::string& fileField,
    const std::string& filePath,
    const std::string& mimeType
) {
    TelegramApiResult result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.curlCode = CURLE_FAILED_INIT;
        return result;
    }

    curl_mime* mime = curl_mime_init(curl);
    if (!mime) {
        curl_easy_cleanup(curl);
        result.curlCode = CURLE_FAILED_INIT;
        return result;
    }

    for (const auto& field : fields) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, field.first.c_str());
        curl_mime_data(part, field.second.c_str(), CURL_ZERO_TERMINATED);
    }

    curl_mimepart* filePart = curl_mime_addpart(mime);
    curl_mime_name(filePart, fileField.c_str());
    curl_mime_filedata(filePart, filePath.c_str());
    curl_mime_type(filePart, mimeType.c_str());

    std::string response;
    std::string url = "https://api.telegram.org/bot" + BOT_TOKEN + "/" + method;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    result.curlCode = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    result.transportOk = (result.curlCode == CURLE_OK);
    result.response = response;
    result.apiOk = extractOkFlag(response);
    result.messageId = extractMessageId(response);
    return result;
}

bool telegramApiOk(const TelegramApiResult& result) {
    return result.transportOk && result.httpStatus >= 200 && result.httpStatus < 300 && result.apiOk;
}

void logTelegramFailure(const std::string& action, const TelegramApiResult& result) {
    std::cout << action << " failed"
              << " curl_status=" << curl_easy_strerror(result.curlCode)
              << " http_status=" << result.httpStatus
              << " response=" << result.response << std::endl;
}

bool isMessageNotModified(const TelegramApiResult& result) {
    return result.httpStatus == 400 && containsText(result.response, "message is not modified");
}

bool isPermanentEditFailure(const TelegramApiResult& result) {
    if (!result.transportOk || isMessageNotModified(result)) return false;
    if (result.httpStatus != 400) return false;

    return containsText(result.response, "message to edit not found") ||
           containsText(result.response, "message can't be edited") ||
           containsText(result.response, "message cannot be edited") ||
           containsText(result.response, "message_id_invalid") ||
           containsText(result.response, "message is not a text message") ||
           containsText(result.response, "there is no text in the message to edit") ||
           containsText(result.response, "message is a media message") ||
           containsText(result.response, "message is not a media message") ||
           containsText(result.response, "there is no photo in the message to edit") ||
           containsText(result.response, "wrong type of the web page content");
}

bool isPermanentDeleteFailure(const TelegramApiResult& result) {
    if (!result.transportOk) return false;
    if (result.httpStatus != 400) return false;

    return containsText(result.response, "message to delete not found") ||
           containsText(result.response, "message can't be deleted") ||
           containsText(result.response, "message cannot be deleted") ||
           containsText(result.response, "message_id_invalid");
}

std::string regexEscape(const std::string& value) {
    std::string result;
    const std::string special = R"(\.^$|()[]{}*+?)";
    for (char c : value) {
        if (special.find(c) != std::string::npos) result += '\\';
        result += c;
    }
    return result;
}

bool extractLongField(const std::string& object, const std::string& name, long& value) {
    std::regex re("\"" + regexEscape(name) + "\"\\s*:\\s*(-?\\d+)");
    std::smatch match;
    if (std::regex_search(object, match, re) && match.size() > 1) {
        value = std::stol(match[1].str());
        return true;
    }
    return false;
}

std::vector<long> extractLongFields(const std::string& object, const std::string& name) {
    std::vector<long> values;
    std::regex re("\"" + regexEscape(name) + "\"\\s*:\\s*(-?\\d+)");
    auto begin = std::sregex_iterator(object.begin(), object.end(), re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        values.push_back(std::stol((*it)[1].str()));
    }

    return values;
}

std::string extractStringField(const std::string& object, const std::string& name) {
    std::regex re("\"" + regexEscape(name) + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (std::regex_search(object, match, re) && match.size() > 1) {
        return match[1].str();
    }
    return "";
}

std::string normalizeLanguage(const std::string& language) {
    return language == "en" ? "en" : "ru";
}

std::string currentLanguage() {
    return normalizeLanguage(currentChatState().language);
}

bool isEnglishLanguage() {
    return currentLanguage() == "en";
}

std::vector<std::pair<long, std::string>> extractUpdateSlices(const std::string& response) {
    std::vector<std::pair<long, std::string>> updates;
    std::regex updateIdRe("\"update_id\"\\s*:\\s*(\\d+)");
    std::vector<std::smatch> matches;

    auto begin = std::sregex_iterator(response.begin(), response.end(), updateIdRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        matches.push_back(*it);
    }

    for (size_t i = 0; i < matches.size(); ++i) {
        const size_t start = static_cast<size_t>(matches[i].position());
        const size_t stop = (i + 1 < matches.size())
                                ? static_cast<size_t>(matches[i + 1].position())
                                : response.size();
        updates.push_back({std::stol(matches[i][1].str()), response.substr(start, stop - start)});
    }

    return updates;
}

bool updateIsStartCommandForConfiguredChat(const std::string& updateSlice) {
    if (!containsText(updateSlice, "\"text\":\"/start") && !containsText(updateSlice, "\"text\":\"\\/start")) {
        return false;
    }

    const std::vector<long> ids = extractLongFields(updateSlice, "id");
    const long configuredChatId = std::stol(CHAT_ID);
    return std::find(ids.begin(), ids.end(), configuredChatId) != ids.end();
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    for (char c : value) {
        if (c == '\\' || c == '"') {
            result += '\\';
            result += c;
        } else if (c == '\n') {
            result += "\\n";
        } else {
            result += c;
        }
    }
    return result;
}

void loadBotStateFromFile() {
    std::ifstream file(BOT_STATE_FILE);
    if (!file) {
        std::cout << "No " << BOT_STATE_FILE << " found, starting with empty bot state" << std::endl;
        return;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    extractLongField(content, "last_update_id", g_botState.last_update_id);

    std::regex chatEntry("\"([^\"]+)\"\\s*:\\s*\\{([^{}]*)\\}");
    auto begin = std::sregex_iterator(content.begin(), content.end(), chatEntry);
    auto end = std::sregex_iterator();

    int loaded = 0;
    for (auto it = begin; it != end; ++it) {
        const std::string chatId = (*it)[1].str();
        if (chatId == "chats") continue;

        const std::string object = (*it)[2].str();
        ChatBotState chatState;
        extractLongField(object, "live_dashboard_message_id", chatState.live_dashboard_message_id);
        extractLongField(object, "last_alert_text_message_id", chatState.last_alert_text_message_id);
        extractLongField(object, "language_prompt_message_id", chatState.language_prompt_message_id);
        chatState.language = extractStringField(object, "language");
        g_botState.chats[chatId] = chatState;
        ++loaded;
    }

    std::cout << "Loaded " << loaded << " chat state record(s) from " << BOT_STATE_FILE
              << ", last_update_id=" << g_botState.last_update_id << std::endl;
}

bool saveBotStateToFile() {
    const std::string tmpFile = BOT_STATE_FILE + ".tmp";
    std::ofstream file(tmpFile, std::ios::trunc);
    if (!file) {
        std::cout << "Failed to open " << tmpFile << " for bot state write" << std::endl;
        return false;
    }

    file << "{\n"
         << "  \"last_update_id\": " << g_botState.last_update_id << ",\n"
         << "  \"chats\": {\n";
    bool first = true;
    for (const auto& entry : g_botState.chats) {
        if (!first) file << ",\n";
        first = false;
        file << "    \"" << jsonEscape(entry.first) << "\": {\n"
             << "      \"live_dashboard_message_id\": " << entry.second.live_dashboard_message_id << ",\n"
             << "      \"last_alert_text_message_id\": " << entry.second.last_alert_text_message_id << ",\n"
             << "      \"language_prompt_message_id\": " << entry.second.language_prompt_message_id << ",\n"
             << "      \"language\": \"" << jsonEscape(entry.second.language) << "\"\n"
             << "    }";
    }
    file << "\n  }\n}\n";
    file.close();

    if (!file) {
        std::cout << "Failed to flush " << tmpFile << std::endl;
        std::remove(tmpFile.c_str());
        return false;
    }

    if (std::rename(tmpFile.c_str(), BOT_STATE_FILE.c_str()) != 0) {
        std::cout << "Failed to replace " << BOT_STATE_FILE << " with " << tmpFile << std::endl;
        std::remove(tmpFile.c_str());
        return false;
    }

    return true;
}

ChatBotState& currentChatState() {
    return g_botState.chats[CHAT_ID];
}

bool ensureRenderDir() {
    if (mkdir(RENDER_DIR.c_str(), 0755) == 0) return true;

    struct stat st {};
    if (stat(RENDER_DIR.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }

    std::cout << "Failed to create render directory: " << RENDER_DIR << std::endl;
    return false;
}

std::string currentTimestamp();

std::string htmlEscape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

std::string temperatureClass(float temp) {
    if (temp >= TEMP_ALARM) return "hot";
    if (temp >= TEMP_WARNING) return "warn";
    return "ok";
}

std::string temperatureLabel(float temp) {
    if (isEnglishLanguage()) {
        if (temp >= TEMP_ALARM) return "HIGH";
        if (temp >= TEMP_WARNING) return "ELEVATED";
        return "NORMAL";
    }

    if (temp >= TEMP_ALARM) return "ВЫСОКАЯ";
    if (temp >= TEMP_WARNING) return "ПОВЫШЕНА";
    return "НОРМА";
}

std::string temperatureColorName(float temp) {
    if (temp >= TEMP_ALARM) return "red";
    if (temp >= TEMP_WARNING) return "yellow";
    return "green";
}

TemperatureState temperatureState(float temp) {
    if (temp >= TEMP_ALARM) return TemperatureState::High;
    if (temp >= TEMP_WARNING) return TemperatureState::Elevated;
    return TemperatureState::Normal;
}

std::string temperatureStateName(TemperatureState state) {
    switch (state) {
        case TemperatureState::Normal: return "NORMAL";
        case TemperatureState::Elevated: return "ELEVATED";
        case TemperatureState::High: return "HIGH";
        default: return "UNKNOWN";
    }
}

std::chrono::steady_clock::duration reportIntervalForState(TemperatureState state) {
    if (state == TemperatureState::Elevated || state == TemperatureState::High) {
        return ELEVATED_REPORT_INTERVAL;
    }

    return REPORT_INTERVAL;
}

std::string buildDashboardHtml(float temp) {
    const std::string cls = temperatureClass(temp);
    const std::string label = temperatureLabel(temp);
    const std::string title = isEnglishLanguage() ? "Server CPU Temperature" : "Температура CPU сервера";

    std::ostringstream value;
    value << std::fixed << std::setprecision(1) << temp;

    std::ostringstream html;
    html << "<!doctype html>\n"
         << "<html><head><meta charset=\"utf-8\">\n"
         << "<style>\n"
         << "html,body{margin:0;width:900px;height:400px;background:#0b0d10;font-family:Inter,Arial,sans-serif;color:#eef2f6;}\n"
         << ".wrap{width:900px;height:400px;box-sizing:border-box;padding:42px;background:linear-gradient(145deg,#11151b,#090b0e);}\n"
         << ".panel{height:316px;border:1px solid #242b34;border-radius:28px;background:#151a21;box-shadow:0 24px 70px rgba(0,0,0,.45),inset 0 1px 0 rgba(255,255,255,.05);padding:38px;box-sizing:border-box;}\n"
         << ".top{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:24px;}\n"
         << ".title{font-size:30px;font-weight:800;letter-spacing:0;color:#f6f8fb;}\n"
         << ".subtitle{margin-top:8px;font-size:18px;color:#9ba7b4;}\n"
         << ".badge{font-size:18px;font-weight:800;letter-spacing:1px;border-radius:999px;padding:12px 18px;border:1px solid #343c47;background:#20262f;}\n"
         << ".badge.ok{color:#55e28a;border-color:#245f3b;background:#13251b;}.badge.warn{color:#ffd166;border-color:#725b22;background:#2a2314;}.badge.hot{color:#ff5b5b;border-color:#7a2b2b;background:#2b1717;}\n"
         << ".temp{display:flex;align-items:flex-end;gap:18px;margin-top:50px;}\n"
         << ".num{font-size:142px;line-height:.86;font-weight:900;letter-spacing:0;font-variant-numeric:tabular-nums;}\n"
         << ".num.ok{color:#55e28a;text-shadow:0 0 34px rgba(85,226,138,.18);}.num.warn{color:#ffd166;text-shadow:0 0 34px rgba(255,209,102,.18);}.num.hot{color:#ff5b5b;text-shadow:0 0 34px rgba(255,91,91,.2);}\n"
         << ".unit{font-size:46px;line-height:1;font-weight:800;color:#c9d2dc;padding-bottom:10px;}\n"
         << "</style></head><body>\n"
         << "<div class=\"wrap\"><div class=\"panel\">\n"
         << "<div class=\"top\"><div><div class=\"title\">" << htmlEscape(title) << "</div><div class=\"subtitle\">by entropia5</div></div><div class=\"badge " << cls << "\">" << htmlEscape(label) << "</div></div>\n"
         << "<div class=\"temp\"><div class=\"num " << cls << "\">" << value.str() << "</div><div class=\"unit\">°C</div></div>\n"
         << "</div></div></body></html>\n";

    std::cout << "Dashboard temperature color=" << temperatureColorName(temp) << std::endl;
    return html.str();
}

void cleanupRenderedDashboardFiles() {
    std::remove(DASHBOARD_HTML_FILE.c_str());
    std::remove(DASHBOARD_IMAGE_FILE.c_str());
    std::remove(DASHBOARD_IMAGE_TMP_FILE.c_str());
}

bool renderDashboardImage(float temp, std::string& imagePath) {
    if (!ensureRenderDir()) return false;

    std::remove(DASHBOARD_HTML_FILE.c_str());
    std::remove(DASHBOARD_IMAGE_FILE.c_str());
    std::remove(DASHBOARD_IMAGE_TMP_FILE.c_str());

    {
        std::ofstream html(DASHBOARD_HTML_FILE, std::ios::trunc);
        if (!html) {
            std::cout << "Failed to open " << DASHBOARD_HTML_FILE << " for render" << std::endl;
            return false;
        }
        html << buildDashboardHtml(temp);
    }

    const std::string command =
        "/usr/bin/wkhtmltoimage --quiet --format jpg --quality 92 --width 900 \"" +
        DASHBOARD_HTML_FILE + "\" \"" + DASHBOARD_IMAGE_TMP_FILE + "\"";
    const int result = std::system(command.c_str());
    if (result != 0) {
        std::cout << "wkhtmltoimage failed with exit code " << result << std::endl;
        cleanupRenderedDashboardFiles();
        return false;
    }

    if (std::rename(DASHBOARD_IMAGE_TMP_FILE.c_str(), DASHBOARD_IMAGE_FILE.c_str()) != 0) {
        std::cout << "Failed to replace rendered dashboard image" << std::endl;
        cleanupRenderedDashboardFiles();
        return false;
    }

    imagePath = DASHBOARD_IMAGE_FILE;
    return true;
}

bool sendTelegramPhotoAndGetId(const std::string& imagePath, long& messageId) {
    TelegramApiResult result = callTelegramMultipartApi("sendPhoto", {
        {"chat_id", CHAT_ID},
        {"disable_notification", "true"}
    }, "photo", imagePath, "image/jpeg");

    if (!telegramApiOk(result) || result.messageId < 0) {
        logTelegramFailure("sendPhoto", result);
        return false;
    }

    messageId = result.messageId;
    return true;
}

EditOutcome editTelegramPhoto(long messageId, const std::string& imagePath) {
    TelegramApiResult result = callTelegramMultipartApi("editMessageMedia", {
        {"chat_id", CHAT_ID},
        {"message_id", std::to_string(messageId)},
        {"media", "{\"type\":\"photo\",\"media\":\"attach://dashboard\"}"}
    }, "dashboard", imagePath, "image/jpeg");

    if (telegramApiOk(result)) {
        std::cout << "Telegram live dashboard photo edited. message_id=" << messageId << std::endl;
        return EditOutcome::Success;
    }

    if (isMessageNotModified(result)) {
        std::cout << "Telegram live dashboard photo unchanged. message_id=" << messageId << std::endl;
        return EditOutcome::Success;
    }

    logTelegramFailure("editMessageMedia", result);
    return isPermanentEditFailure(result) ? EditOutcome::PermanentFailure : EditOutcome::TransientFailure;
}

bool sendTelegramTextAndGetId(const std::string& text, long& messageId) {
    TelegramApiResult result = callTelegramApi("sendMessage", {
        {"chat_id", CHAT_ID},
        {"text", text},
        {"disable_notification", "true"}
    });

    if (!telegramApiOk(result) || result.messageId < 0) {
        logTelegramFailure("sendMessage(status text)", result);
        return false;
    }

    messageId = result.messageId;
    return true;
}

bool sendTelegramTextWithMarkupAndGetId(
    const std::string& text,
    const std::string& replyMarkup,
    long& messageId
) {
    TelegramApiResult result = callTelegramApi("sendMessage", {
        {"chat_id", CHAT_ID},
        {"text", text},
        {"reply_markup", replyMarkup},
        {"disable_notification", "true"}
    });

    if (!telegramApiOk(result) || result.messageId < 0) {
        logTelegramFailure("sendMessage(language prompt)", result);
        return false;
    }

    messageId = result.messageId;
    return true;
}

void answerCallbackQuery(const std::string& callbackQueryId) {
    if (callbackQueryId.empty()) return;

    TelegramApiResult result = callTelegramApi("answerCallbackQuery", {
        {"callback_query_id", callbackQueryId}
    });

    if (!telegramApiOk(result)) {
        logTelegramFailure("answerCallbackQuery", result);
    }
}

EditOutcome editTelegramText(long messageId, const std::string& text) {
    TelegramApiResult result = callTelegramApi("editMessageText", {
        {"chat_id", CHAT_ID},
        {"message_id", std::to_string(messageId)},
        {"text", text}
    });

    if (telegramApiOk(result)) {
        std::cout << "Telegram status text edited. message_id=" << messageId << std::endl;
        return EditOutcome::Success;
    }

    if (isMessageNotModified(result)) {
        std::cout << "Telegram status text unchanged. message_id=" << messageId << std::endl;
        return EditOutcome::Success;
    }

    logTelegramFailure("editMessageText(status text)", result);
    return isPermanentEditFailure(result) ? EditOutcome::PermanentFailure : EditOutcome::TransientFailure;
}

std::string buildTemperatureStatusText(float temp) {
    if (isEnglishLanguage()) {
        if (temp >= TEMP_ALARM) {
            return "Warning. CPU overheating.";
        }

        if (temp >= TEMP_WARNING) {
            return "Temperature is elevated.";
        }

        return "Temperature is within normal range.";
    }

    if (temp >= TEMP_ALARM) return "Внимание. Перегрев ЦПУ.";
    if (temp >= TEMP_WARNING) return "Повышенная температура.";
    return "Температура в пределах нормы.";
}

DeleteOutcome deleteTelegramMessage(long messageId, const std::string& label) {
    TelegramApiResult result = callTelegramApi("deleteMessage", {
        {"chat_id", CHAT_ID},
        {"message_id", std::to_string(messageId)}
    });

    if (telegramApiOk(result)) {
        std::cout << label << " deleted. message_id=" << messageId << std::endl;
        return DeleteOutcome::Success;
    }

    if (isPermanentDeleteFailure(result)) {
        std::cout << label << " already unavailable. message_id=" << messageId << std::endl;
        return DeleteOutcome::PermanentFailure;
    }

    logTelegramFailure("deleteMessage(" + label + ")", result);
    return DeleteOutcome::TransientFailure;
}

void deleteStoredMessageIfAny(long& messageId, const std::string& label) {
    if (messageId < 0) return;

    const long oldMessageId = messageId;
    DeleteOutcome result = deleteTelegramMessage(oldMessageId, label);

    if (result == DeleteOutcome::Success || result == DeleteOutcome::PermanentFailure) {
        messageId = -1;
        saveBotStateToFile();
    }
}

void deleteTrackedBotMessagesForLanguageFlow(bool includeLanguagePrompt) {
    ChatBotState& chatState = currentChatState();
    deleteStoredMessageIfAny(chatState.live_dashboard_message_id, "live dashboard");
    deleteStoredMessageIfAny(chatState.last_alert_text_message_id, "status text");

    if (includeLanguagePrompt) {
        deleteStoredMessageIfAny(chatState.language_prompt_message_id, "language prompt");
    }
}

void deleteStoredHelperMessageIfAny() {
    ChatBotState& chatState = currentChatState();
    deleteStoredMessageIfAny(chatState.last_alert_text_message_id, "helper message");
}

bool upsertLiveDashboardPhoto(const std::string& imagePath, bool freshLiveOnStartup = false) {
    ChatBotState& chatState = currentChatState();
    if (freshLiveOnStartup && chatState.live_dashboard_message_id >= 0) {
        const long oldMessageId = chatState.live_dashboard_message_id;
        DeleteOutcome deleteResult = deleteTelegramMessage(oldMessageId, "startup live dashboard");

        if (deleteResult == DeleteOutcome::Success || deleteResult == DeleteOutcome::PermanentFailure) {
            chatState.live_dashboard_message_id = -1;
            saveBotStateToFile();
        } else {
            std::cout << "Startup live dashboard delete was transient; editing saved message_id="
                      << oldMessageId << " instead of creating a new chat message" << std::endl;
        }
    }

    if (chatState.live_dashboard_message_id >= 0) {
        EditOutcome editResult = editTelegramPhoto(chatState.live_dashboard_message_id, imagePath);
        if (editResult == EditOutcome::Success) {
            return true;
        }

        if (editResult == EditOutcome::TransientFailure) {
            std::cout << "Keeping saved live dashboard message_id="
                      << chatState.live_dashboard_message_id
                      << " after transient edit failure" << std::endl;
            return false;
        }

        std::cout << "Resetting unavailable live dashboard message_id="
                  << chatState.live_dashboard_message_id << std::endl;
        chatState.live_dashboard_message_id = -1;
        saveBotStateToFile();
    }

    long newMessageId = -1;
    if (!sendTelegramPhotoAndGetId(imagePath, newMessageId)) {
        return false;
    }

    chatState.live_dashboard_message_id = newMessageId;
    saveBotStateToFile();
    std::cout << "Telegram live dashboard photo created. message_id=" << newMessageId << std::endl;
    return true;
}

bool upsertTemperatureStatusText(const std::string& text) {
    ChatBotState& chatState = currentChatState();
    if (chatState.last_alert_text_message_id >= 0) {
        EditOutcome editResult = editTelegramText(chatState.last_alert_text_message_id, text);
        if (editResult == EditOutcome::Success) {
            return true;
        }

        if (editResult == EditOutcome::TransientFailure) {
            std::cout << "Keeping saved status text message_id="
                      << chatState.last_alert_text_message_id
                      << " after transient edit failure" << std::endl;
            return false;
        }

        std::cout << "Resetting unavailable status text message_id="
                  << chatState.last_alert_text_message_id << std::endl;
        chatState.last_alert_text_message_id = -1;
        saveBotStateToFile();
    }

    long newMessageId = -1;
    if (!sendTelegramTextAndGetId(text, newMessageId)) {
        return false;
    }

    chatState.last_alert_text_message_id = newMessageId;
    saveBotStateToFile();
    std::cout << "Telegram status text created. message_id=" << newMessageId << std::endl;
    return true;
}

std::string currentTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_r(&now, &localTime);

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
    return buffer;
}

bool publishTemperatureDashboard(float temp, bool freshLiveOnStartup) {
    if (freshLiveOnStartup) {
        deleteStoredHelperMessageIfAny();
    }

    std::string imagePath;
    if (!renderDashboardImage(temp, imagePath)) {
        return false;
    }

    const bool photoOk = upsertLiveDashboardPhoto(imagePath, freshLiveOnStartup);
    if (photoOk) {
        upsertTemperatureStatusText(buildTemperatureStatusText(temp));
    }

    cleanupRenderedDashboardFiles();
    return photoOk;
}

bool languageSelectionPending() {
    return currentChatState().language_prompt_message_id >= 0;
}

std::string buildLanguageSelectionReplyMarkup() {
    return R"({"inline_keyboard":[[{"text":"Русский","callback_data":"lang:ru"}],[{"text":"English","callback_data":"lang:en"}]]})";
}

bool sendLanguageSelectionPrompt() {
    long messageId = -1;
    if (!sendTelegramTextWithMarkupAndGetId(
            "Выберите язык / Choose language",
            buildLanguageSelectionReplyMarkup(),
            messageId
        )) {
        return false;
    }

    ChatBotState& chatState = currentChatState();
    chatState.language_prompt_message_id = messageId;
    saveBotStateToFile();
    std::cout << "Language prompt created. message_id=" << messageId << std::endl;
    return true;
}

void handleStartCommand(const std::string& updateSlice) {
    long startMessageId = -1;
    extractLongField(updateSlice, "message_id", startMessageId);

    std::cout << "/start received from configured chat. Showing language prompt." << std::endl;
    deleteTrackedBotMessagesForLanguageFlow(true);
    sendLanguageSelectionPrompt();

    if (startMessageId >= 0) {
        deleteTelegramMessage(startMessageId, "start command");
    }
}

std::string extractCallbackQueryId(const std::string& updateSlice) {
    std::regex re("\"callback_query\"\\s*:\\s*\\{\\s*\"id\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch match;
    if (std::regex_search(updateSlice, match, re) && match.size() > 1) {
        return match[1].str();
    }

    return "";
}

std::string extractCallbackData(const std::string& updateSlice) {
    return extractStringField(updateSlice, "data");
}

bool updateBelongsToConfiguredChat(const std::string& updateSlice) {
    const std::vector<long> ids = extractLongFields(updateSlice, "id");
    const long configuredChatId = std::stol(CHAT_ID);
    return std::find(ids.begin(), ids.end(), configuredChatId) != ids.end();
}

bool handleLanguageCallback(const std::string& updateSlice) {
    if (!updateBelongsToConfiguredChat(updateSlice)) return false;

    const std::string callbackData = extractCallbackData(updateSlice);
    if (callbackData != "lang:ru" && callbackData != "lang:en") return false;

    answerCallbackQuery(extractCallbackQueryId(updateSlice));

    long callbackMessageId = -1;
    extractLongField(updateSlice, "message_id", callbackMessageId);

    ChatBotState& chatState = currentChatState();
    if (callbackMessageId >= 0) {
        chatState.language_prompt_message_id = callbackMessageId;
    }
    chatState.language = (callbackData == "lang:en") ? "en" : "ru";
    saveBotStateToFile();

    std::cout << "Language selected: " << chatState.language << std::endl;
    deleteTrackedBotMessagesForLanguageFlow(true);
    if (chatState.language_prompt_message_id >= 0) {
        chatState.language_prompt_message_id = -1;
        saveBotStateToFile();
    }
    return true;
}

struct PollResult {
    bool publishFreshDashboard = false;
};

TelegramApiResult getTelegramUpdates(long offset, int limit = 10) {
    return callTelegramApi("getUpdates", {
        {"offset", std::to_string(offset)},
        {"limit", std::to_string(limit)},
        {"timeout", "0"},
        {"allowed_updates", "[\"message\",\"callback_query\"]"}
    });
}

void syncTelegramUpdatesOffsetOnFirstRun() {
    if (g_botState.last_update_id >= 0) return;

    TelegramApiResult result = getTelegramUpdates(-1, 1);
    if (!telegramApiOk(result)) {
        logTelegramFailure("getUpdates(initial sync)", result);
        return;
    }

    long maxUpdateId = -1;
    for (const auto& update : extractUpdateSlices(result.response)) {
        maxUpdateId = std::max(maxUpdateId, update.first);
    }

    g_botState.last_update_id = maxUpdateId;
    saveBotStateToFile();
    std::cout << "Telegram update offset initialized. last_update_id="
              << g_botState.last_update_id << std::endl;
}

PollResult pollTelegramUpdates() {
    PollResult pollResult;
    const long offset = (g_botState.last_update_id >= 0) ? g_botState.last_update_id + 1 : 0;
    TelegramApiResult result = getTelegramUpdates(offset);
    if (!telegramApiOk(result)) {
        logTelegramFailure("getUpdates", result);
        return pollResult;
    }

    const auto updates = extractUpdateSlices(result.response);
    if (updates.empty()) return pollResult;

    long maxUpdateId = g_botState.last_update_id;
    for (const auto& update : updates) {
        maxUpdateId = std::max(maxUpdateId, update.first);

        if (updateIsStartCommandForConfiguredChat(update.second)) {
            handleStartCommand(update.second);
            continue;
        }

        if (handleLanguageCallback(update.second)) {
            pollResult.publishFreshDashboard = true;
        }
    }

    if (maxUpdateId > g_botState.last_update_id) {
        g_botState.last_update_id = maxUpdateId;
        saveBotStateToFile();
    }

    return pollResult;
}

//фу-ция загрузки дял .env
void loadEnv(const std::string& filename = ".env") {
    std::ifstream file(filename);
    std::string line;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) continue;

        std::string key = line.substr(0, equalsPos);
        std::string value = line.substr(equalsPos + 1);

        if (key == "BOT_TOKEN") {
            BOT_TOKEN = value;
        } else if (key == "CHAT_ID") {
            CHAT_ID = value;
        }
    }
}


int main() {
    loadEnv();

    if (BOT_TOKEN.empty() || CHAT_ID.empty()) {
        std::cerr << "Missing BOT_TOKEN or CHAT_ID in .env" << std::endl;
        return 1;
    }

    loadBotStateFromFile();
    currentChatState();
    syncTelegramUpdatesOffsetOnFirstRun();

    bool freshLiveOnStartup = true;
    bool publishFreshDashboardNow = false;
    TemperatureState lastPublishedState = TemperatureState::Unknown;
    auto lastReportTime = std::chrono::steady_clock::now() - REPORT_INTERVAL;


    std::cout << "Temperature monitoring:" << std::endl;
    std::cout << "Alarm temp: " << TEMP_ALARM << "°C" << std::endl;
    std::cout << "Warning temp: " << TEMP_WARNING << "°C" << std::endl;
    std::cout << "Report interval: 5 minutes" << std::endl;
    std::cout << "Elevated/high interval: 15 seconds" << std::endl;
    std::cout << "Monitoring started..." << std::endl;

    while (true) {
        PollResult pollResult = pollTelegramUpdates();
        if (pollResult.publishFreshDashboard) {
            publishFreshDashboardNow = true;
        }

        if (languageSelectionPending()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        float temp = getCPUTemperature();
        if (temp < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        const TemperatureState currentState = temperatureState(temp);
        const bool stateChanged = (currentState != lastPublishedState);
        const auto elapsed = now - lastReportTime;
        const bool intervalElapsed = (elapsed >= reportIntervalForState(currentState));

        if (stateChanged) {
            std::cout << "Temperature state changed to " << temperatureStateName(currentState)
                      << ": " << temp << "°C" << std::endl;
        }

        if (publishFreshDashboardNow || stateChanged || intervalElapsed) {
            if (publishTemperatureDashboard(temp, freshLiveOnStartup || publishFreshDashboardNow)) {
                freshLiveOnStartup = false;
                publishFreshDashboardNow = false;
                lastPublishedState = currentState;
                lastReportTime = now;
            }
        }

        // статус в консоль.
        static int counter = 0;
        if (counter++ % 12 == 0) {  // каждые s6 минут
            std::cout << "Current: " << temp << "°C" << std::endl;
        }

        for (int i = 0; i < SENSOR_INTERVAL.count(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            PollResult sleepPollResult = pollTelegramUpdates();
            if (sleepPollResult.publishFreshDashboard) {
                publishFreshDashboardNow = true;
                break;
            }

            if (languageSelectionPending()) {
                break;
            }
        }
    }
    return 0;
}
