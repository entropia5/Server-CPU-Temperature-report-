#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <cstdlib>

const std::string BOT_TOKEN = "BOT_TOKEN";
const std::string CHAT_ID = "CHAT_ID";

const float TEMP_ALARM = 50.0f;
const float TEMP_NORMAL = 40.0f;

const std::string ESP32_IP = "ESP32_IP";

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

void sendToESP32(int fanSpeed) {
    if (fanSpeed < 0) fanSpeed = 0;
    if (fanSpeed > 255) fanSpeed = 255;

    std::string cmd = "curl -s \"http://" + ESP32_IP + "/fan?speed=" + std::to_string(fanSpeed) + "\" > /dev/null 2>&1";
    int result = system(cmd.c_str());

    if (result == 0) {
        std::cout << "ESP32: fan speed set to " << fanSpeed << std::endl;
    } else {
        std::cout << "Failed to send command to ESP32" << std::endl;
    }
}

std::string buildFancyReport(float temp, int fanSpeed) {
    std::ostringstream oss;

    oss << std::left << std::setw(12) << "Server CPU Temperature:" << " > " << std::fixed << std::setprecision(1) << temp << "C\n";
    oss << std::left << std::setw(12) << "Fan speed:" << " > " << fanSpeed << "/255\n\n";

    if (temp >= TEMP_ALARM) {
        oss << "ALERT: Overheating!\n";
        oss << "Fan is running at MAX speed";
    } else if (temp >= TEMP_NORMAL) {
        oss << "WARNING: Temperature is elevated\n";
        oss << "Fan is running at MEDIUM speed";
    } else {
        oss << "OK: Temperature is normal\n";
        oss << "Fan is OFF";
    }

    return "```cpp\n" + escape(oss.str()) + "\n```";
}

int main() {
    bool isOverheated = false;
    auto lastReportTime = std::chrono::steady_clock::now() - std::chrono::hours(6);
    int lastFanSpeed = -1;

    std::cout << "Temperature Monitor + Fan Control" << std::endl;
    std::cout << "ESP32 IP: " << ESP32_IP << std::endl;
    std::cout << "Alarm temp: " << TEMP_ALARM << "C" << std::endl;
    std::cout << "Normal temp: " << TEMP_NORMAL << "C" << std::endl;
    std::cout << "Monitoring started..." << std::endl;

    while (true) {
        float temp = getCPUTemperature();
        if (temp < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        bool needSend = false;
        int targetFanSpeed = lastFanSpeed;

        if (temp >= TEMP_ALARM) {
            targetFanSpeed = 255;
            if (!isOverheated) {
                isOverheated = true;
                needSend = true;
                std::cout << "Temperature high! Fan ON" << std::endl;
            }
        }
        else if (temp <= TEMP_NORMAL) {
            targetFanSpeed = 0;
            if (isOverheated) {
                isOverheated = false;
                needSend = true;
                std::cout << "Temperature normal! Fan OFF" << std::endl;
            }
        }
        else {
            targetFanSpeed = (int)((temp - TEMP_NORMAL) / (TEMP_ALARM - TEMP_NORMAL) * 255);
            if (targetFanSpeed < 50) targetFanSpeed = 50;
            if (targetFanSpeed > 200) targetFanSpeed = 200;

            if (!isOverheated && targetFanSpeed > 0) {
                needSend = true;
                std::cout << "Temperature " << temp << "C, fan speed " << targetFanSpeed << std::endl;
            }
        }

        if (targetFanSpeed != lastFanSpeed) {
            sendToESP32(targetFanSpeed);
            lastFanSpeed = targetFanSpeed;
        }

        if (needSend) {
            sendTelegramMessage(buildFancyReport(temp, targetFanSpeed));
        }
        else {
            auto elapsed = std::chrono::duration_cast<std::chrono::hours>(now - lastReportTime).count();
            if (elapsed >= 6) {
                sendTelegramMessage(buildFancyReport(temp, targetFanSpeed));
                lastReportTime = now;
            }
        }

        static int counter = 0;
        if (counter++ % 12 == 0) {
            std::cout << "Current: " << temp << "C, Fan: " << targetFanSpeed << "/255" << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
    return 0;
}
