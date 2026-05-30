#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <regex>


std::string BOT_TOKEN;
std::string CHAT_ID;


const float TEMP_ALARM = 50.0f;
const float TEMP_NORMAL = 50.0f;
const auto REPORT_INTERVAL = std::chrono::minutes(5);

//Фун-ции

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

bool sendTelegramMessage(const std::string& text) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage";
    std::string postFields = "chat_id=" + CHAT_ID + "&parse_mode=MarkdownV2&text=" + text;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK);
}

size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

long extractMessageId(const std::string& response) {
    std::regex re("\"message_id\"\\s*:\\s*(\\d+)");
    std::smatch match;
    if (std::regex_search(response, match, re) && match.size() > 1) {
        return std::stol(match[1].str());
    }
    return -1;
}

bool sendTelegramMessageAndGetId(const std::string& text, long& messageId) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string response;
    std::string url = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage";
    std::string postFields = "chat_id=" + CHAT_ID + "&parse_mode=MarkdownV2&text=" + text;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return false;

    long parsedId = extractMessageId(response);
    if (parsedId < 0) return false;

    messageId = parsedId;
    return true;
}

bool editTelegramMessage(long messageId, const std::string& text) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = "https://api.telegram.org/bot" + BOT_TOKEN + "/editMessageText";
    std::string postFields =
        "chat_id=" + CHAT_ID +
        "&message_id=" + std::to_string(messageId) +
        "&parse_mode=MarkdownV2&text=" + text;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK);
}

std::string buildFancyReport(float temp) {
    std::ostringstream oss;

    oss << std::left << std::setw(12) << "Server CPU Temperature:" << " ▸ " << std::fixed << std::setprecision(1) << temp << "°C\n";



    return "```cpp\n" + escape(oss.str()) + "\n```";
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


    bool isOverheated = false;
    long persistentMessageId = -1;
    auto lastReportTime = std::chrono::steady_clock::now() - REPORT_INTERVAL;


    std::cout << "Temperature monitoring:" << std::endl;
    std::cout << "Alarm temp: " << TEMP_ALARM << "°C" << std::endl;
    std::cout << "Normal temp: " << TEMP_NORMAL << "°C" << std::endl;
    std::cout << "Report interval: 5 minutes" << std::endl;
    std::cout << "Monitoring started..." << std::endl;

    while (true) {
        float temp = getCPUTemperature();
        if (temp < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        bool needSend = false;

        if (temp > TEMP_ALARM) {
            if (!isOverheated) {
                isOverheated = true;
                needSend = true;
                std::cout << "🔴 ALERT: Temperature " << temp << "°C" << std::endl;
            }
        }
        else if (temp <= TEMP_NORMAL) {
            if (isOverheated) {
                isOverheated = false;
                needSend = true;
                std::cout << "🟢 Temperature normal: " << temp << "°C" << std::endl;
            }
        }

        // уведомления в тг
        if (needSend) {
            const std::string report = buildFancyReport(temp);
            if (persistentMessageId < 0) {
                if (sendTelegramMessageAndGetId(report, persistentMessageId)) {
                    std::cout << "Telegram window created. message_id=" << persistentMessageId << std::endl;
                } else {
                    std::cout << "Failed to create Telegram window" << std::endl;
                }
            } else if (!editTelegramMessage(persistentMessageId, report)) {
                std::cout << "Failed to update Telegram window, creating a new one..." << std::endl;
                if (!sendTelegramMessageAndGetId(report, persistentMessageId)) {
                    std::cout << "Failed to create fallback Telegram window" << std::endl;
                }
            }
            lastReportTime = now;
        }
        else {
            auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - lastReportTime);
            if (elapsed >= REPORT_INTERVAL) {
                const std::string report = buildFancyReport(temp);
                if (persistentMessageId < 0) {
                    if (!sendTelegramMessageAndGetId(report, persistentMessageId)) {
                        std::cout << "Failed to create Telegram window" << std::endl;
                    }
                } else if (!editTelegramMessage(persistentMessageId, report)) {
                    std::cout << "Failed to update Telegram window, creating a new one..." << std::endl;
                    if (!sendTelegramMessageAndGetId(report, persistentMessageId)) {
                        std::cout << "Failed to create fallback Telegram window" << std::endl;
                    }
                }
                lastReportTime = now;
            }
        }

        // статус в консоль.
        static int counter = 0;
        if (counter++ % 12 == 0) {  // каждые s6 минут
            std::cout << "Current: " << temp << "°C" << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
    return 0;
}
