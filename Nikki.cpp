#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <memory>

#pragma comment(lib, "ws2_32.lib")

std::atomic<int> g_TotalServers(0);
std::atomic<int> g_QueriedServers(0);
std::atomic<int> g_OnlineServers(0);
std::atomic<bool> g_QueryInProgress(false);
std::atomic<bool> g_InitialQueryComplete(false);
std::chrono::system_clock::time_point g_QueryStartTime;

std::atomic<int> g_RetryTotal(0);
std::atomic<int> g_RetryQueried(0);
std::atomic<int> g_RetryOnline(0);
std::atomic<bool> g_RetryInProgress(false);

const uint8_t A2S_INFO_RESPONSE = 0x49;
const uint8_t A2S_PLAYER_RESPONSE = 0x44;
const uint8_t A2S_INFO_RESPONSE_LEGACY = 0x6D;
const uint8_t A2S_CHALLENGE_RESPONSE = 0x41;


struct ServerInfo {
    std::string name;
    std::string map;
    uint8_t players;
    uint8_t max_players;
    std::string game;
    bool is_valid;
    double ping;
    uint8_t bot_count;
    bool password_protected;
    bool vac_enabled;
    std::string version;

    ServerInfo() : players(0), max_players(0), is_valid(false), ping(0.0),
        bot_count(0), password_protected(false), vac_enabled(false) {
    }
};
struct Player {
    std::string name;
    int32_t score;
    float duration;
    uint8_t index;

    Player() : score(0), duration(0.0f), index(0) {}
};

std::string jsonEscape(const std::string& str) {
    std::string escaped;
    for (unsigned char c : str) {
        if (c == '"') {
            escaped += "\\\"";
        }
        else if (c == '\\') {
            escaped += "\\\\";
        }
        else if (c == '\b') {
            escaped += "\\b";
        }
        else if (c == '\f') {
            escaped += "\\f";
        }
        else if (c == '\n') {
            escaped += "\\n";
        }
        else if (c == '\r') {
            escaped += "\\r";
        }
        else if (c == '\t') {
            escaped += "\\t";
        }
        else if (c < 32 || c == 127) {
            char buf[7];
            sprintf_s(buf, "\\u%04x", (unsigned int)c);
            escaped += buf;
        }
        else {
            escaped += c;
        }
    }
    return escaped;
}
struct AppConfig {
    bool auto_refresh_enabled;
    int refresh_interval;
    bool auto_retry_enabled;
    int retry_interval;
    int max_retry_attempts;
    int query_timeout;
    int display_limit;
    int max_concurrent_threads;
    int retry_concurrent_threads;

    AppConfig() : auto_refresh_enabled(false), refresh_interval(60),
        auto_retry_enabled(false), retry_interval(120),
        max_retry_attempts(2), query_timeout(2000),
        display_limit(100), max_concurrent_threads(200),
        retry_concurrent_threads(100) {
    }

    std::string to_json() const {
        std::string json = "{";
        json += "\"auto_refresh_enabled\":" + std::string(auto_refresh_enabled ? "true" : "false") + ",";
        json += "\"refresh_interval\":" + std::to_string(refresh_interval) + ",";
        json += "\"auto_retry_enabled\":" + std::string(auto_retry_enabled ? "true" : "false") + ",";
        json += "\"retry_interval\":" + std::to_string(retry_interval) + ",";
        json += "\"max_retry_attempts\":" + std::to_string(max_retry_attempts) + ",";
        json += "\"query_timeout\":" + std::to_string(query_timeout) + ",";
        json += "\"display_limit\":" + std::to_string(display_limit) + ",";
        json += "\"max_concurrent_threads\":" + std::to_string(max_concurrent_threads) + ",";
        json += "\"retry_concurrent_threads\":" + std::to_string(retry_concurrent_threads);
        json += "}";
        return json;
    }

    static AppConfig from_json(const std::string& json) {
        AppConfig cfg;
        size_t pos;

        pos = json.find("\"auto_refresh_enabled\":");
        if (pos != std::string::npos) {
            cfg.auto_refresh_enabled = json.find("true", pos) < json.find("false", pos);
        }

        pos = json.find("\"refresh_interval\":");
        if (pos != std::string::npos) {
            cfg.refresh_interval = std::stoi(json.substr(pos + 19));
        }

        pos = json.find("\"auto_retry_enabled\":");
        if (pos != std::string::npos) {
            cfg.auto_retry_enabled = json.find("true", pos) < json.find("false", pos);
        }

        pos = json.find("\"retry_interval\":");
        if (pos != std::string::npos) {
            cfg.retry_interval = std::stoi(json.substr(pos + 17));
        }

        pos = json.find("\"max_retry_attempts\":");
        if (pos != std::string::npos) {
            cfg.max_retry_attempts = std::stoi(json.substr(pos + 21));
        }

        pos = json.find("\"query_timeout\":");
        if (pos != std::string::npos) {
            cfg.query_timeout = std::stoi(json.substr(pos + 16));
        }

        pos = json.find("\"display_limit\":");
        if (pos != std::string::npos) {
            cfg.display_limit = std::stoi(json.substr(pos + 16));
        }

        pos = json.find("\"max_concurrent_threads\":");
        if (pos != std::string::npos) {
            cfg.max_concurrent_threads = std::stoi(json.substr(pos + 25));
        }

        pos = json.find("\"retry_concurrent_threads\":");
        if (pos != std::string::npos) {
            cfg.retry_concurrent_threads = std::stoi(json.substr(pos + 27));
        }

        return cfg;
    }
};

AppConfig g_AppConfig;
std::mutex configMutex;
void saveAppConfig() {
    std::lock_guard<std::mutex> lock(configMutex);
    std::ofstream file("config.json");
    if (file.good()) {
        file << g_AppConfig.to_json();
        std::cout << "Config saved to config.json\n";
    }
}

void loadAppConfig() {
    std::ifstream file("config.json");
    if (!file.good()) {
        std::cout << "No config.json found, using defaults\n";
        saveAppConfig();
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();

    std::lock_guard<std::mutex> lock(configMutex);
    g_AppConfig = AppConfig::from_json(json);

    std::cout << "Config loaded from config.json\n";
    std::cout << "  Auto-refresh: " << (g_AppConfig.auto_refresh_enabled ? "ON" : "OFF") << "\n";
    std::cout << "  Refresh interval: " << g_AppConfig.refresh_interval << "s\n";
    std::cout << "  Query timeout: " << g_AppConfig.query_timeout << "ms\n";
    std::cout << "  Max concurrent threads: " << g_AppConfig.max_concurrent_threads << "\n";
}
struct ServerResult {
    std::string ip;
    ServerInfo info;
    std::vector<Player> players;
    std::string error;

    std::string to_json() const {
        std::string json = "{";
        json += "\"ip\":\"" + jsonEscape(ip) + "\"";

        if (error.empty()) {
            json += ",\"error\":null";
            json += ",\"info\":{";
            json += "\"name\":\"" + jsonEscape(info.name) + "\"";
            json += ",\"map\":\"" + jsonEscape(info.map) + "\"";
            json += ",\"players\":" + std::to_string(info.players);
            json += ",\"max_players\":" + std::to_string(info.max_players);
            json += ",\"ping\":" + std::to_string(info.ping);
            json += "}";

            json += ",\"players\":[";
            for (size_t i = 0; i < players.size(); i++) {
                if (i > 0) json += ",";
                json += "{";
                json += "\"name\":\"" + jsonEscape(players[i].name) + "\",";
                json += "\"score\":" + std::to_string(players[i].score) + ",";
                json += "\"duration\":" + std::to_string(players[i].duration);
                json += "}";
            }
            json += "]";
        }
        else {
            json += ",\"error\":\"" + jsonEscape(error) + "\"";
        }

        json += "}";
        return json;
    }
};

std::map<std::string, ServerResult> serverCache;
std::mutex cacheMutex;

std::string readNullTerminatedString(const uint8_t*& data, size_t& remaining) {
    std::string result;
    while (remaining > 0 && *data != 0) {
        result += (char)*data;
        data++;
        remaining--;
    }
    if (remaining > 0) {
        data++;
        remaining--;
    }
    return result;
}

std::string sanitizeString(const std::string& str) {
    std::string result;
    for (unsigned char c : str) {
        if (c >= 32 && c < 127) {
            result += c;
        }
        else if (c >= 128 && c <= 255) {
            result += c;
        }
    }
    return result;
}
template<typename T>
T readValue(const uint8_t*& data, size_t& remaining) {
    if (remaining < sizeof(T)) return T();
    T value;
    memcpy(&value, data, sizeof(T));
    data += sizeof(T);
    remaining -= sizeof(T);
    return value;
}


std::vector<std::string> loadServers(const std::string& filename) {
    std::vector<std::string> servers;
    std::ifstream file(filename);

    if (!file.good()) {
        std::cout << "Warning: " << filename << " not found!\n";
        return servers;
    }

    std::string line;
    int lineNum = 0;

    while (std::getline(file, line)) {
        lineNum++;

        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end = line.find_last_not_of(" \t\r\n");

        if (start == std::string::npos) continue;

        line = line.substr(start, end - start + 1);

        if (line[0] == '#') continue;

        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            std::cout << "Warning: Line " << lineNum << " has invalid format (missing colon): " << line << "\n";
            continue;
        }

        std::string ip = line.substr(0, colonPos);
        std::string portStr = line.substr(colonPos + 1);

        try {
            int port = std::stoi(portStr);
            if (port < 1 || port > 65535) {
                std::cout << "Warning: Line " << lineNum << " has invalid port: " << port << "\n";
                continue;
            }
        }
        catch (...) {
            std::cout << "Warning: Line " << lineNum << " has invalid port: " << portStr << "\n";
            continue;
        }

        servers.push_back(line);
    }

    return servers;
}
std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}
ServerInfo queryServerInfo(const std::string& host, int port, int timeout_ms) {
    ServerInfo info;
    info.is_valid = false;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return info;

    DWORD timeout = timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());

    auto pingStart = std::chrono::high_resolution_clock::now();

    std::vector<uint8_t> challenge(4, 0xFF);
    uint8_t buffer[4096];
    int received = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        std::vector<uint8_t> payload;
        payload.push_back(0xFF);
        payload.push_back(0xFF);
        payload.push_back(0xFF);
        payload.push_back(0xFF);
        payload.push_back(0x54); // A2S_INFO

        std::string query = "Source Engine Query";
        payload.insert(payload.end(), query.begin(), query.end());
        payload.push_back(0x00);

        if (challenge.size() == 4 && challenge[0] != 0xFF) {
            payload.insert(payload.end(), challenge.begin(), challenge.end());
        }

        sendto(sock, (char*)payload.data(), (int)payload.size(), 0, (sockaddr*)&addr, sizeof(addr));

        received = recvfrom(sock, (char*)buffer, sizeof(buffer), 0, nullptr, nullptr);

        if (received < 5) continue;

        if (buffer[4] == A2S_CHALLENGE_RESPONSE && received >= 9) {
            challenge.assign(buffer + 5, buffer + 9);
            continue;
        }

        if (buffer[4] == A2S_INFO_RESPONSE || buffer[4] == A2S_INFO_RESPONSE_LEGACY) {
            break;
        }
    }

    closesocket(sock);

    if (received < 6) return info;

    auto pingEnd = std::chrono::high_resolution_clock::now();
    info.ping = std::chrono::duration<double>(pingEnd - pingStart).count();

    const uint8_t* data = buffer + 4;
    size_t remaining = received - 4;

    uint8_t responseType = readValue<uint8_t>(data, remaining);

    if (responseType != A2S_INFO_RESPONSE && responseType != A2S_INFO_RESPONSE_LEGACY) {
        return info;
    }

    info.is_valid = true;

    if (responseType == A2S_INFO_RESPONSE) {
        // Modern response format
        uint8_t protocol = readValue<uint8_t>(data, remaining);

        info.name = sanitizeString(readNullTerminatedString(data, remaining));
        info.map = sanitizeString(readNullTerminatedString(data, remaining));
        std::string folder = readNullTerminatedString(data, remaining); // folder
        info.game = sanitizeString(readNullTerminatedString(data, remaining));

        uint16_t appID = readValue<uint16_t>(data, remaining);
        info.players = readValue<uint8_t>(data, remaining);
        info.max_players = readValue<uint8_t>(data, remaining);
        info.bot_count = readValue<uint8_t>(data, remaining);
        uint8_t server_type = readValue<uint8_t>(data, remaining);
        uint8_t platform = readValue<uint8_t>(data, remaining);

        uint8_t password = readValue<uint8_t>(data, remaining);
        uint8_t vac = readValue<uint8_t>(data, remaining);
        info.password_protected = password != 0;
        info.vac_enabled = vac != 0;

        info.version = sanitizeString(readNullTerminatedString(data, remaining));
    }
    else {
        // Legacy response format
        readNullTerminatedString(data, remaining);
        info.name = sanitizeString(readNullTerminatedString(data, remaining));
        info.map = sanitizeString(readNullTerminatedString(data, remaining));
        std::string folder = readNullTerminatedString(data, remaining);
        info.game = sanitizeString(readNullTerminatedString(data, remaining));

        info.players = readValue<uint8_t>(data, remaining);
        info.max_players = readValue<uint8_t>(data, remaining);

        uint8_t protocol = readValue<uint8_t>(data, remaining);
        uint8_t server_type = readValue<uint8_t>(data, remaining);
        uint8_t platform = readValue<uint8_t>(data, remaining);

        uint8_t password = readValue<uint8_t>(data, remaining);
        info.password_protected = password != 0;
    }

    if (info.name.empty()) {
        info.name = "Unknown Server";
    }
    if (info.map.empty()) {
        info.map = "Unknown Map";
    }

    return info;
}

std::vector<Player> queryPlayers(const std::string& host, int port, int timeout_ms) {
    std::vector<Player> players;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return players;

    DWORD timeout = timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());

    std::vector<uint8_t> challenge;
    challenge.push_back(0xFF);
    challenge.push_back(0xFF);
    challenge.push_back(0xFF);
    challenge.push_back(0xFF);

    for (int attempt = 0; attempt < 3; attempt++) {
        std::vector<uint8_t> payload;
        payload.push_back(0xFF);
        payload.push_back(0xFF);
        payload.push_back(0xFF);
        payload.push_back(0xFF);
        payload.push_back(0x55);
        payload.insert(payload.end(), challenge.begin(), challenge.end());

        sendto(sock, (char*)payload.data(), (int)payload.size(), 0, (sockaddr*)&addr, sizeof(addr));

        uint8_t buffer[4096];
        int received = recvfrom(sock, (char*)buffer, sizeof(buffer), 0, nullptr, nullptr);

        if (received < 5) continue;

        if (buffer[4] == A2S_CHALLENGE_RESPONSE && received >= 9) {
            challenge.assign(buffer + 5, buffer + 9);
            continue;
        }
        else if (buffer[4] == A2S_PLAYER_RESPONSE) {
            const uint8_t* data = buffer + 5;
            size_t remaining = received - 5;

            uint8_t playerCount = readValue<uint8_t>(data, remaining);

            for (int i = 0; i < playerCount && remaining > 0; i++) {
                Player player;
                player.index = readValue<uint8_t>(data, remaining);
                player.name = sanitizeString(readNullTerminatedString(data, remaining));

                if (remaining >= 4) {
                    player.score = readValue<int32_t>(data, remaining);
                }
                if (remaining >= 4) {
                    player.duration = readValue<float>(data, remaining);
                }

                players.push_back(player);
            }
            break;
        }
    }

    closesocket(sock);
    return players;
}
ServerResult queryServer(const std::string& serverIP) {
    ServerResult result;
    result.ip = serverIP;

    auto parts = split(serverIP, ':');
    if (parts.size() != 2) {
        result.error = "Invalid Format";
        return result;
    }

    std::string host = parts[0];
    int port = std::stoi(parts[1]);

    result.info = queryServerInfo(host, port, 1500);
    if (!result.info.is_valid) {
        result.error = "No Response";
        return result;
    }

    if (result.info.players > 0) {
        result.players = queryPlayers(host, port, 2000);
    }

    return result;
}
void queryAllServersBackground(const std::vector<std::string>& servers) {
    g_QueryInProgress = true;
    g_TotalServers = servers.size();
    g_QueriedServers = 0;
    g_OnlineServers = 0;
    g_QueryStartTime = std::chrono::system_clock::now();

    std::cout << "\n========================================\n";
    std::cout << "Starting background server query...\n";
    std::cout << "========================================\n\n";

    const int MAX_CONCURRENT = g_AppConfig.max_concurrent_threads;
    std::vector<std::thread> threads;

    for (const auto& serverIP : servers) {
        threads.emplace_back([serverIP]() {
            auto result = queryServer(serverIP);

            cacheMutex.lock();
            serverCache[result.ip] = result;
            cacheMutex.unlock();

            if (result.error.empty()) {
                g_OnlineServers++;
            }

            int done = ++g_QueriedServers;
            if (done % 50 == 0 || done == g_TotalServers.load()) {
                auto elapsed = std::chrono::system_clock::now() - g_QueryStartTime;
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
                std::cout << "Progress: " << done << "/" << g_TotalServers.load()
                    << " (" << g_OnlineServers.load() << " online) - "
                    << seconds << "s elapsed\n";
            }
            });

        if (threads.size() >= MAX_CONCURRENT) {
            for (auto& t : threads) {
                if (t.joinable()) t.join();
            }
            threads.clear();
        }
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    auto endTime = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(endTime - g_QueryStartTime).count();

    std::cout << "\n========================================\n";
    std::cout << "Query complete in " << duration << " seconds!\n";
    std::cout << "Total: " << serverCache.size() << " servers\n";
    std::cout << "Online: " << g_OnlineServers.load() << " servers\n";
    std::cout << "Offline: " << (serverCache.size() - g_OnlineServers.load()) << " servers\n";
    std::cout << "========================================\n\n";

    g_QueryInProgress = false;
    g_InitialQueryComplete = true;
}
void sendHTTPResponse(SOCKET client, const std::string& content, const std::string& contentType) {
    std::string response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + std::to_string(content.length()) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += content;

    send(client, response.c_str(), (int)response.length(), 0);
}
std::string getHTMLPage() {
    std::string html;
    html += "<!DOCTYPE html>";
    html += "<html>";
    html += "<head>";
    html += "<meta charset=\"UTF-8\">";
    html += "<title>Withered Tracker</title>";
    html += "<style>";

    html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
    html += "body { font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', system-ui, sans-serif; background: #0d0d0d; color: #e0e0e0; }";
    html += "body::before { content: ''; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: url('/background') center center fixed; background-size: cover; opacity: 0.12; z-index: -1; }";

    html += ".container { max-width: 1400px; margin: 0 auto; padding: 20px; }";

    html += ".header { background: #1a1a1a; padding: 24px 30px; border-radius: 8px; margin-bottom: 20px; border: 1px solid #2a2a2a; }";
    html += ".header h1 { font-size: 28px; font-weight: 600; color: #ffffff; margin-bottom: 6px; letter-spacing: -0.5px; }";
    html += ".header p { color: #888; font-size: 14px; font-weight: 400; }";

    html += ".stats-bar { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 12px; margin-bottom: 20px; }";
    html += ".stat-card { background: #1a1a1a; padding: 18px; border-radius: 6px; border: 1px solid #2a2a2a; }";
    html += ".stat-card h3 { font-size: 11px; text-transform: uppercase; color: #666; margin-bottom: 8px; letter-spacing: 0.5px; font-weight: 500; }";
    html += ".stat-card p { font-size: 24px; font-weight: 600; color: #ffffff; }";

    html += ".tabs { display: flex; gap: 4px; margin-bottom: 20px; background: #1a1a1a; padding: 4px; border-radius: 6px; border: 1px solid #2a2a2a; }";
    html += ".tab { flex: 1; padding: 10px 20px; background: transparent; border: none; color: #888; cursor: pointer; font-size: 14px; font-weight: 500; border-radius: 4px; transition: all 0.2s; }";
    html += ".tab:hover { color: #e0e0e0; background: #222; }";
    html += ".tab.active { color: #ffffff; background: #2a2a2a; }";

    html += ".tab-content { display: none; }";
    html += ".tab-content.active { display: block; }";

    html += ".card { background: #1a1a1a; border: 1px solid #2a2a2a; border-radius: 6px; padding: 20px; margin-bottom: 12px; transition: all 0.2s; }";
    html += ".card:hover { border-color: #3a3a3a; }";
    html += ".card.online { border-left: 3px solid #10b981; }";
    html += ".card.offline { border-left: 3px solid #ef4444; }";

    html += ".server-name { font-size: 16px; font-weight: 600; color: #ffffff; margin-bottom: 10px; }";
    html += ".server-info { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 8px; margin-top: 10px; }";
    html += ".info-item { font-size: 13px; color: #888; }";
    html += ".info-item strong { color: #e0e0e0; font-weight: 500; }";

    html += "input, select, textarea { width: 100%; padding: 10px 12px; background: #0d0d0d; border: 1px solid #2a2a2a; border-radius: 4px; color: #e0e0e0; font-size: 14px; transition: all 0.2s; font-family: inherit; }";
    html += "input:focus, select:focus, textarea:focus { outline: none; border-color: #4a4a4a; background: #1a1a1a; }";

    html += ".checkbox-container { display: flex; align-items: center; gap: 10px; margin-bottom: 16px; }";
    html += ".checkbox-container input[type='checkbox'] { width: auto; }";
    html += ".checkbox-container label { color: #e0e0e0; font-size: 14px; cursor: pointer; }";

    html += ".btn { padding: 10px 20px; background: #ffffff; color: #0d0d0d; border: none; border-radius: 4px; cursor: pointer; font-size: 14px; font-weight: 500; transition: all 0.2s; font-family: inherit; }";
    html += ".btn:hover { background: #e0e0e0; transform: translateY(-1px); }";
    html += ".btn:active { transform: translateY(0); }";
    html += ".btn-secondary { background: #2a2a2a; color: #e0e0e0; }";
    html += ".btn-secondary:hover { background: #3a3a3a; }";

    html += ".form-group { margin-bottom: 20px; }";
    html += ".form-group label { display: block; margin-bottom: 8px; color: #888; font-size: 13px; font-weight: 500; }";

    html += ".grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(380px, 1fr)); gap: 12px; }";

    html += ".badge { display: inline-block; padding: 4px 10px; border-radius: 3px; font-size: 11px; font-weight: 500; text-transform: uppercase; letter-spacing: 0.3px; }";
    html += ".badge-success { background: rgba(16, 185, 129, 0.15); color: #10b981; }";
    html += ".badge-danger { background: rgba(239, 68, 68, 0.15); color: #ef4444; }";
    html += ".badge-info { background: rgba(100, 100, 100, 0.2); color: #aaa; }";

    html += ".search-box { display: flex; gap: 10px; margin-bottom: 20px; }";
    html += ".search-box input { flex: 1; }";

    html += ".loading { text-align: center; padding: 40px; color: #666; }";
    html += ".spinner { display: inline-block; width: 32px; height: 32px; border: 3px solid #2a2a2a; border-top-color: #ffffff; border-radius: 50%; animation: spin 0.8s linear infinite; }";
    html += "@keyframes spin { to { transform: rotate(360deg); } }";

    html += ".filters { display: flex; gap: 8px; margin-bottom: 16px; flex-wrap: wrap; }";
    html += ".filter-btn { padding: 8px 14px; background: #1a1a1a; border: 1px solid #2a2a2a; border-radius: 4px; color: #888; cursor: pointer; font-size: 13px; transition: all 0.2s; font-weight: 500; }";
    html += ".filter-btn.active { background: #ffffff; color: #0d0d0d; border-color: #ffffff; }";

    html += ".player-list { margin-top: 12px; padding-top: 12px; border-top: 1px solid #2a2a2a; }";
    html += ".player-item { padding: 8px 10px; background: #0d0d0d; border-radius: 4px; margin-bottom: 4px; font-size: 13px; display: flex; justify-content: space-between; border: 1px solid #1a1a1a; }";

    html += ".config-section { margin-bottom: 30px; }";
    html += ".config-section h3 { font-size: 14px; color: #ffffff; margin-bottom: 16px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; }";

    html += "</style>";
    html += "</head>";
    html += "<body>";

    html += "<div class=\"container\">";

    html += "<div class=\"header\">";
    html += "<h1>Withered Tracker</h1>";
    html += "<p>For cheaters, from cheaters</p>";
    html += "</div>";

    html += "<div class=\"stats-bar\">";
    html += "<div class=\"stat-card\">";
    html += "<h3>Total Servers</h3>";
    html += "<p id=\"stat-total\">-</p>";
    html += "</div>";
    html += "<div class=\"stat-card\">";
    html += "<h3>Online</h3>";
    html += "<p id=\"stat-online\">-</p>";
    html += "</div>";
    html += "<div class=\"stat-card\">";
    html += "<h3>Total Players</h3>";
    html += "<p id=\"stat-players\">-</p>";
    html += "</div>";
    html += "<div class=\"stat-card\">";
    html += "<h3>Last Update</h3>";
    html += "<p id=\"stat-update\" style=\"font-size: 14px;\">-</p>";
    html += "</div>";
    html += "</div>";

    html += "<div class=\"tabs\">";
    html += "<button class=\"tab active\" onclick=\"switchTab('overview')\">Overview</button>";
    html += "<button class=\"tab\" onclick=\"switchTab('search')\">Find Players</button>";
    html += "<button class=\"tab\" onclick=\"switchTab('query')\">Server Query</button>";
    html += "<button class=\"tab\" onclick=\"switchTab('config')\">Settings</button>";
    html += "</div>";

    html += "<div id=\"tab-overview\" class=\"tab-content active\">";

    html += "<div id=\"loading-overlay\" style=\"display: none; background: #1a1a1a; border: 1px solid #2a2a2a; border-radius: 8px; padding: 40px; text-align: center; margin-bottom: 20px;\">";
    html += "<div class=\"spinner\" style=\"margin: 0 auto 20px;\"></div>";
    html += "<h2 style=\"font-size: 18px; margin-bottom: 10px;\">Querying Servers</h2>";
    html += "<p id=\"progress-text\" style=\"color: #888; font-size: 24px; font-weight: 600; margin-bottom: 10px;\">0 / 0</p>";
    html += "<p id=\"progress-status\" style=\"color: #666; font-size: 14px;\">Starting...</p>";
    html += "<div style=\"background: #0d0d0d; border-radius: 4px; height: 8px; margin-top: 20px; overflow: hidden;\">";
    html += "<div id=\"progress-bar\" style=\"background: linear-gradient(90deg, #667eea, #764ba2); height: 100%; width: 0%; transition: width 0.3s;\"></div>";
    html += "</div>";
    html += "</div>";

    html += "<div id=\"servers-container\">";
    html += "<div class=\"filters\">";
    html += "<button class=\"filter-btn active\" onclick=\"filterServers('all')\">All</button>";
    html += "<button class=\"filter-btn\" onclick=\"filterServers('online')\">Online</button>";
    html += "<button class=\"filter-btn\" onclick=\"filterServers('offline')\">Offline</button>";
    html += "<button class=\"filter-btn\" onclick=\"filterServers('players')\">Has Players</button>";
    html += "<button class=\"btn btn-secondary\" style=\"margin-left: auto; padding: 8px 14px;\" onclick=\"retryOfflineServers()\">Retry Offline</button>";
    html += "<select onchange=\"sortServers(this.value)\" style=\"width: auto; margin-left: auto;\">";
    html += "<option value=\"default\">Sort by</option>";
    html += "<option value=\"players-desc\">Most Players</option>";
    html += "<option value=\"players-asc\">Least Players</option>";
    html += "<option value=\"ping-asc\">Best Ping</option>";
    html += "<option value=\"ping-desc\">Worst Ping</option>";
    html += "<option value=\"name-asc\">Name A-Z</option>";
    html += "</select>";
    html += "</div>";
    html += "<div id=\"servers-list\" class=\"grid\"></div>";
    html += "</div>";
    html += "</div>";

    html += "<div id=\"tab-search\" class=\"tab-content\">";
    html += "<div class=\"search-box\">";
    html += "<input type=\"text\" id=\"searchInput\" placeholder=\"Enter player name\" onkeypress=\"if(event.key==='Enter')searchPlayer()\">";
    html += "<button class=\"btn\" onclick=\"searchPlayer()\">Search</button>";
    html += "</div>";
    html += "<div id=\"searchResults\"></div>";
    html += "</div>";

    html += "<div id=\"tab-query\" class=\"tab-content\">";
    html += "<div class=\"card\">";
    html += "<h2 style=\"margin-bottom: 20px; font-size: 18px;\">Query Specific Server</h2>";
    html += "<div class=\"form-group\">";
    html += "<label>Server Address</label>";
    html += "<input type=\"text\" id=\"queryIP\" placeholder=\"IP:PORT (e.g., 127.0.0.1:27015)\">";
    html += "</div>";
    html += "<button class=\"btn\" onclick=\"querySpecificServer()\">Query Server</button>";
    html += "</div>";
    html += "<div id=\"queryResult\" style=\"margin-top: 20px;\"></div>";
    html += "</div>";

    html += "<div id=\"tab-config\" class=\"tab-content\">";
    html += "<div class=\"card\">";
    html += "<h2 style=\"margin-bottom: 24px; font-size: 18px;\">Settings</h2>";

    html += "<div class=\"config-section\">";
    html += "<h3>Auto Refresh</h3>";
    html += "<div class=\"checkbox-container\">";
    html += "<input type=\"checkbox\" id=\"autoRefreshEnabled\" onchange=\"updateAutoRefresh()\">";
    html += "<label for=\"autoRefreshEnabled\">Enable auto-refresh</label>";
    html += "</div>";
    html += "<div class=\"form-group\">";
    html += "<label>Refresh Interval (seconds)</label>";
    html += "<input type=\"number\" id=\"refreshInterval\" value=\"60\" min=\"10\" max=\"600\">";
    html += "</div>";
    html += "</div>";

    html += "<div class=\"config-section\">";
    html += "<h3>Auto Retry</h3>";
    html += "<div class=\"checkbox-container\">";
    html += "<input type=\"checkbox\" id=\"autoRetryEnabled\" onchange=\"updateAutoRetry()\">";
    html += "<label for=\"autoRetryEnabled\">Enable auto-retry for offline servers</label>";
    html += "</div>";
    html += "<div class=\"form-group\">";
    html += "<label>Retry Interval (seconds)</label>";
    html += "<input type=\"number\" id=\"retryInterval\" value=\"120\" min=\"30\" max=\"600\">";
    html += "</div>";
    html += "<div class=\"form-group\">";
    html += "<label>Max Retry Attempts</label>";
    html += "<input type=\"number\" id=\"maxRetryAttempts\" value=\"2\" min=\"1\" max=\"5\">";
    html += "</div>";
    html += "<div class=\"form-group\">";
    html += "<label>Retry Concurrent Threads</label>";
    html += "<input type=\"number\" id=\"retryConcurrent\" value=\"100\" min=\"10\" max=\"500\">";
    html += "</div>";
    html += "</div>";

    html += "<div class=\"config-section\">";
    html += "<h3>Performance</h3>";
    html += "<div class=\"form-group\">";
    html += "<label>Query Timeout (milliseconds)</label>";
    html += "<input type=\"number\" id=\"queryTimeout\" value=\"2000\" min=\"500\" max=\"10000\">";
    html += "</div>";
    html += "<div class=\"form-group\">";
    html += "<label>Display Limit (servers per page)</label>";
    html += "<input type=\"number\" id=\"displayLimit\" value=\"100\" min=\"10\" max=\"1000\">";
    html += "</div>";
    html += "</div>";
    html += "<div class=\"form-group\">";
    html += "<label>Max Concurrent Threads</label>";
    html += "<input type=\"number\" id=\"maxConcurrent\" value=\"200\" min=\"50\" max=\"500\">";
    html += "</div>";
    html += "<button class=\"btn\" onclick=\"saveConfig()\">Save Settings</button>";
    html += "<button class=\"btn btn-secondary\" style=\"margin-left: 10px;\" onclick=\"refreshAllServers()\">Refresh All Now</button>";
    html += "</div>";
    html += "</div>";

    html += "</div>";

    html += "<script>";
    html += "let allServers = [];";
    html += "let currentFilter = 'all';";
    html += "let autoRefreshTimer = null;";
    html += "let autoRetryTimer = null;";

    html += "function checkProgress() {";
    html += "fetch('/api/progress')";
    html += ".then(r => r.json())";
    html += ".then(data => {";
    html += "if(data.in_progress || data.retry_in_progress) {";
    html += "document.getElementById('loading-overlay').style.display = 'block';";
    html += "document.getElementById('servers-container').style.display = 'none';";

    html += "if(data.retry_in_progress) {";
    html += "document.querySelector('#loading-overlay h2').textContent = 'Retrying Offline Servers';";
    html += "const percent = data.retry_total > 0 ? Math.round((data.retry_queried / data.retry_total) * 100) : 0;";
    html += "document.getElementById('progress-text').textContent = data.retry_queried + ' / ' + data.retry_total;";
    html += "document.getElementById('progress-status').textContent = data.retry_online + ' servers came online - ' + percent + '%';";
    html += "document.getElementById('progress-bar').style.width = percent + '%';";
    html += "} else {";
    html += "document.querySelector('#loading-overlay h2').textContent = 'Querying Servers';";
    html += "const percent = data.total > 0 ? Math.round((data.queried / data.total) * 100) : 0;";
    html += "document.getElementById('progress-text').textContent = data.queried + ' / ' + data.total;";
    html += "document.getElementById('progress-status').textContent = data.online + ' servers online - ' + percent + '%';";
    html += "document.getElementById('progress-bar').style.width = percent + '%';";
    html += "}";

    html += "if(!progressTimer) {";
    html += "progressTimer = setInterval(checkProgress, 500);";
    html += "}";
    html += "} else {";
    html += "document.getElementById('loading-overlay').style.display = 'none';";
    html += "document.getElementById('servers-container').style.display = 'block';";
    html += "if(progressTimer) {";
    html += "clearInterval(progressTimer);";
    html += "progressTimer = null;";
    html += "}";
    html += "if(data.initial_complete) {";
    html += "loadServers();";
    html += "}";
    html += "}";
    html += "});";
    html += "}";
    html += "let config = {};";
    html += "function retryOfflineServers() {";
    html += "const offline = allServers.filter(s => s.error);";
    html += "if(offline.length === 0) {";
    html += "alert('No offline servers to retry');";
    html += "return;";
    html += "}";
    html += "if(!confirm('Retry ' + offline.length + ' offline servers?')) return;";
    html += "fetch('/api/retry-offline', {method: 'POST'})";
    html += ".then(r => r.json())";
    html += ".then(data => {";
    html += "console.log('Retry started');";
    html += "});";
    html += "}";

    html += "function loadConfig() {";
    html += "fetch('/api/config')";
    html += ".then(r => r.json())";
    html += ".then(data => {";
    html += "config = data;";
    html += "document.getElementById('autoRefreshEnabled').checked = config.auto_refresh_enabled;";
    html += "document.getElementById('refreshInterval').value = config.refresh_interval;";
    html += "document.getElementById('autoRetryEnabled').checked = config.auto_retry_enabled;";
    html += "document.getElementById('retryInterval').value = config.retry_interval;";
    html += "document.getElementById('maxRetryAttempts').value = config.max_retry_attempts;";
    html += "document.getElementById('queryTimeout').value = config.query_timeout;";
    html += "document.getElementById('displayLimit').value = config.display_limit;";
    html += "document.getElementById('maxConcurrent').value = config.max_concurrent_threads;";
    html += "document.getElementById('retryConcurrent').value = config.retry_concurrent_threads;";
    html += "if(config.auto_refresh_enabled) startAutoRefresh();";
    html += "if(config.auto_retry_enabled) startAutoRetry();";
    html += "});";
    html += "}";

    html += "function saveConfig() {";
    html += "config.auto_refresh_enabled = document.getElementById('autoRefreshEnabled').checked;";
    html += "config.refresh_interval = parseInt(document.getElementById('refreshInterval').value);";
    html += "config.auto_retry_enabled = document.getElementById('autoRetryEnabled').checked;";
    html += "config.retry_interval = parseInt(document.getElementById('retryInterval').value);";
    html += "config.max_retry_attempts = parseInt(document.getElementById('maxRetryAttempts').value);";
    html += "config.query_timeout = parseInt(document.getElementById('queryTimeout').value);";
    html += "config.display_limit = parseInt(document.getElementById('displayLimit').value);";
    html += "config.max_concurrent_threads = parseInt(document.getElementById('maxConcurrent').value);";
    html += "config.retry_concurrent_threads = parseInt(document.getElementById('retryConcurrent').value);";
    html += "fetch('/api/config', {";
    html += "method: 'POST',";
    html += "headers: {'Content-Type': 'application/json'},";
    html += "body: JSON.stringify(config)";
    html += "})";
    html += ".then(r => r.json())";
    html += ".then(data => {";
    html += "alert('Settings saved to config.json');";
    html += "if(config.auto_refresh_enabled) { stopAutoRefresh(); startAutoRefresh(); } else { stopAutoRefresh(); }";
    html += "if(config.auto_retry_enabled) { stopAutoRetry(); startAutoRetry(); } else { stopAutoRetry(); }";
    html += "});";
    html += "}";

    html += "function updateAutoRefresh() {";
    html += "config.autoRefreshEnabled = document.getElementById('autoRefreshEnabled').checked;";
    html += "if(config.autoRefreshEnabled) startAutoRefresh(); else stopAutoRefresh();";
    html += "}";

    html += "function startAutoRefresh() {";
    html += "stopAutoRefresh();";
    html += "autoRefreshTimer = setInterval(() => {";
    html += "console.log('Auto-refreshing servers...');";
    html += "loadServers();";
    html += "}, config.refreshInterval * 1000);";
    html += "}";

    html += "function stopAutoRefresh() {";
    html += "if(autoRefreshTimer) clearInterval(autoRefreshTimer);";
    html += "}";

    html += "function updateAutoRetry() {";
    html += "config.autoRetryEnabled = document.getElementById('autoRetryEnabled').checked;";
    html += "if(config.autoRetryEnabled) startAutoRetry(); else stopAutoRetry();";
    html += "}";

    html += "function startAutoRetry() {";
    html += "stopAutoRetry();";
    html += "autoRetryTimer = setInterval(() => {";
    html += "console.log('Auto-retrying offline servers...');";
    html += "const offline = allServers.filter(s => s.error);";
    html += "console.log('Found ' + offline.length + ' offline servers');";
    html += "}, config.retryInterval * 1000);";
    html += "}";

    html += "function stopAutoRetry() {";
    html += "if(autoRetryTimer) clearInterval(autoRetryTimer);";
    html += "}";

    html += "function switchTab(tabName) {";
    html += "document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));";
    html += "document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));";
    html += "event.target.classList.add('active');";
    html += "document.getElementById('tab-' + tabName).classList.add('active');";
    html += "}";

    html += "function loadServers() {";
    html += "fetch('/api/servers')";
    html += ".then(r => r.json())";
    html += ".then(data => {";
    html += "allServers = data;";
    html += "updateStats();";
    html += "displayServers(data);";
    html += "});";
    html += "}";

    html += "function updateStats() {";
    html += "const online = allServers.filter(s => !s.error).length;";
    html += "const totalPlayers = allServers.reduce((sum, s) => sum + (s.info ? s.info.players : 0), 0);";
    html += "document.getElementById('stat-total').textContent = allServers.length;";
    html += "document.getElementById('stat-online').textContent = online;";
    html += "document.getElementById('stat-players').textContent = totalPlayers;";
    html += "document.getElementById('stat-update').textContent = new Date().toLocaleTimeString();";
    html += "}";

    html += "function displayServers(servers) {";
    html += "const container = document.getElementById('servers-list');";
    html += "container.innerHTML = '';";
    html += "const limit = config.display_limit || 100;";
    html += "const limited = servers.slice(0, limit);";
    html += "if(limited.length === 0) {";
    html += "container.innerHTML = '<p class=\"loading\">No servers to display</p>';";
    html += "return;";
    html += "}";
    html += "limited.forEach(s => {";
    html += "const card = document.createElement('div');";
    html += "card.className = 'card ' + (s.error ? 'offline' : 'online');";
    html += "if(s.error) {";
    html += "card.innerHTML = `";
    html += "<div class='server-name'>${s.ip}</div>";
    html += "<span class='badge badge-danger'>Offline</span>";
    html += "<p style='color: #666; margin-top: 10px; font-size: 13px;'>${s.error}</p>";
    html += "`;";
    html += "} else {";
    html += "let playersList = '';";
    html += "if(s.players && s.players.length > 0) {";
    html += "playersList = '<div class=\"player-list\">';";
    html += "s.players.forEach(p => {";
    html += "const duration = p.duration < 60 ? Math.floor(p.duration) + 's' : Math.floor(p.duration/60) + 'm';";
    html += "playersList += `<div class='player-item'><span>${p.name}</span><span style='color: #666;'>${duration} - ${p.score}</span></div>`;";
    html += "});";
    html += "playersList += '</div>';";
    html += "}";
    html += "card.innerHTML = `";
    html += "<div class='server-name'>${s.info.name || 'Unknown'}</div>";
    html += "<span class='badge badge-success'>Online</span>";
    html += "<div class='server-info'>";
    html += "<div class='info-item'><strong>IP:</strong> ${s.ip}</div>";
    html += "<div class='info-item'><strong>Map:</strong> ${s.info.map || 'N/A'}</div>";
    html += "<div class='info-item'><strong>Players:</strong> ${s.info.players}/${s.info.max_players}</div>";
    html += "<div class='info-item'><strong>Ping:</strong> ${Math.round(s.info.ping * 1000)}ms</div>";
    html += "</div>";
    html += "${playersList}";
    html += "`;";
    html += "}";
    html += "container.appendChild(card);";
    html += "});";
    html += "if(servers.length > limit) {";
    html += "container.innerHTML += `<p style='text-align: center; color: #666; padding: 20px;'>Showing ${limit} of ${servers.length} servers. Increase display limit in Settings.</p>`;";
    html += "}";
    html += "}";

    html += "function filterServers(filter) {";
    html += "currentFilter = filter;";
    html += "document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));";
    html += "event.target.classList.add('active');";
    html += "let filtered = allServers;";
    html += "if(filter === 'online') filtered = allServers.filter(s => !s.error);";
    html += "else if(filter === 'offline') filtered = allServers.filter(s => s.error);";
    html += "else if(filter === 'players') filtered = allServers.filter(s => !s.error && s.info.players > 0);";
    html += "displayServers(filtered);";
    html += "}";

    html += "function sortServers(sort) {";
    html += "let sorted = [...allServers];";
    html += "if(sort === 'players-desc') sorted.sort((a,b) => (b.info?.players || 0) - (a.info?.players || 0));";
    html += "else if(sort === 'players-asc') sorted.sort((a,b) => (a.info?.players || 0) - (b.info?.players || 0));";
    html += "else if(sort === 'ping-asc') sorted.sort((a,b) => (a.info?.ping || 999) - (b.info?.ping || 999));";
    html += "else if(sort === 'ping-desc') sorted.sort((a,b) => (b.info?.ping || 0) - (a.info?.ping || 0));";
    html += "else if(sort === 'name-asc') sorted.sort((a,b) => (a.info?.name || '').localeCompare(b.info?.name || ''));";
    html += "allServers = sorted;";
    html += "displayServers(sorted);";
    html += "}";

    html += "function searchPlayer() {";
    html += "const name = document.getElementById('searchInput').value;";
    html += "if(!name) return;";
    html += "document.getElementById('searchResults').innerHTML = '<div class=\"loading\"><div class=\"spinner\"></div></div>';";
    html += "fetch('/api/search?name=' + encodeURIComponent(name))";
    html += ".then(r => r.json())";
    html += ".then(data => {";
    html += "let html = '<h2 style=\"margin-bottom: 20px; font-size: 18px;\">Found ' + data.length + ' matches</h2><div class=\"grid\">';";
    html += "data.forEach(p => {";
    html += "html += `<div class='card online'>`;";
    html += "html += `<div class='server-name'>${p.player_name}</div>`;";
    html += "html += `<span class='badge badge-info'>Playing</span>`;";
    html += "html += `<div class='server-info'>`;";
    html += "html += `<div class='info-item'><strong>Server:</strong> ${p.server_name}</div>`;";
    html += "html += `<div class='info-item'><strong>IP:</strong> ${p.server_ip}</div>`;";
    html += "html += `<div class='info-item'><strong>Map:</strong> ${p.map}</div>`;";
    html += "html += `<div class='info-item'><strong>Players:</strong> ${p.players}/${p.max_players}</div>`;";
    html += "html += `</div></div>`;";
    html += "});";
    html += "html += '</div>';";
    html += "if(data.length === 0) html = '<p class=\"loading\">No players found</p>';";
    html += "document.getElementById('searchResults').innerHTML = html;";
    html += "});";
    html += "}";

    html += "function querySpecificServer() {";
    html += "const ip = document.getElementById('queryIP').value;";
    html += "if(!ip) return;";
    html += "document.getElementById('queryResult').innerHTML = '<div class=\"loading\"><div class=\"spinner\"></div></div>';";
    html += "fetch('/api/query?ip=' + encodeURIComponent(ip))";
    html += ".then(r => r.json())";
    html += ".then(data => {";
    html += "let html = '<div class=\"card ' + (data.error ? 'offline' : 'online') + '\">';";
    html += "if(data.error) {";
    html += "html += `<div class='server-name'>${data.ip}</div>`;";
    html += "html += `<span class='badge badge-danger'>Offline</span>`;";
    html += "html += `<p style='color: #666; margin-top: 10px;'>${data.error}</p>`;";
    html += "} else {";
    html += "html += `<div class='server-name'>${data.info.name}</div>`;";
    html += "html += `<span class='badge badge-success'>Online</span>`;";
    html += "html += `<div class='server-info'>`;";
    html += "html += `<div class='info-item'><strong>IP:</strong> ${data.ip}</div>`;";
    html += "html += `<div class='info-item'><strong>Map:</strong> ${data.info.map}</div>`;";
    html += "html += `<div class='info-item'><strong>Players:</strong> ${data.info.players}/${data.info.max_players}</div>`;";
    html += "html += `<div class='info-item'><strong>Ping:</strong> ${Math.round(data.info.ping * 1000)}ms</div>`;";
    html += "html += `</div>`;";
    html += "if(data.players && data.players.length > 0) {";
    html += "html += '<div class=\"player-list\">';";
    html += "data.players.forEach(p => {";
    html += "const duration = p.duration < 60 ? Math.floor(p.duration) + 's' : Math.floor(p.duration/60) + 'm';";
    html += "html += `<div class='player-item'><span>${p.name}</span><span style='color: #666;'>${duration} - ${p.score}</span></div>`;";
    html += "});";
    html += "html += '</div>';";
    html += "}";
    html += "}";
    html += "html += '</div>';";
    html += "document.getElementById('queryResult').innerHTML = html;";
    html += "});";
    html += "}";

    html += "function refreshAllServers() {";
    html += "loadServers();";
    html += "}";

    html += "loadConfig();";
    html += "loadServers();";
    html += "checkProgress();";
    html += "setInterval(checkProgress, 2000);";

    html += "</script>";
    html += "</body>";
    html += "</html>";

    return html;
}
std::string getServersJSON() {
    std::lock_guard<std::mutex> lock(cacheMutex);

    std::string json = "[";
    bool first = true;

    for (const auto& pair : serverCache) {
        if (!first) json += ",";
        json += "{";
        json += "\"ip\":\"" + jsonEscape(pair.second.ip) + "\"";

        if (pair.second.error.empty()) {
            json += ",\"error\":null";
            json += ",\"info\":{";
            json += "\"name\":\"" + jsonEscape(pair.second.info.name) + "\"";
            json += ",\"map\":\"" + jsonEscape(pair.second.info.map) + "\"";
            json += ",\"players\":" + std::to_string(pair.second.info.players);
            json += ",\"max_players\":" + std::to_string(pair.second.info.max_players);
            json += ",\"ping\":" + std::to_string(pair.second.info.ping);
            json += "}";
        }
        else {
            json += ",\"error\":\"" + jsonEscape(pair.second.error) + "\"";
        }

        json += "}";
        first = false;
    }

    json += "]";
    return json;
}

std::string urlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); i++) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int value;
            std::istringstream iss(str.substr(i + 1, 2));
            if (iss >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            }
            else {
                result += str[i];
            }
        }
        else if (str[i] == '+') {
            result += ' ';
        }
        else {
            result += str[i];
        }
    }
    return result;
}

std::string getPlayerSearchJSON(const std::string& searchName) {
    std::lock_guard<std::mutex> lock(cacheMutex);

    std::string searchLower = sanitizeString(searchName);
    std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);

    std::string json = "[";
    bool first = true;

    for (const auto& pair : serverCache) {
        if (pair.second.error.empty() && !pair.second.players.empty()) {
            for (const auto& player : pair.second.players) {
                std::string playerNameLower = player.name;
                std::transform(playerNameLower.begin(), playerNameLower.end(),
                    playerNameLower.begin(), ::tolower);

                if (playerNameLower.find(searchLower) != std::string::npos) {
                    if (!first) json += ",";
                    json += "{";
                    json += "\"player_name\":\"" + jsonEscape(player.name) + "\",";
                    json += "\"server_name\":\"" + jsonEscape(pair.second.info.name) + "\",";
                    json += "\"server_ip\":\"" + jsonEscape(pair.second.ip) + "\",";
                    json += "\"map\":\"" + jsonEscape(pair.second.info.map) + "\",";
                    json += "\"players\":" + std::to_string(pair.second.info.players) + ",";
                    json += "\"max_players\":" + std::to_string(pair.second.info.max_players);
                    json += "}";
                    first = false;
                }
            }
        }
    }

    json += "]";
    return json;
}

void sendBackgroundImage(SOCKET client) {
    std::vector<std::string> extensions = { ".png", ".jpg", ".jpeg", ".gif" };
    std::string foundFile;

    for (const auto& ext : extensions) {
        std::string filename = "background" + ext;
        std::ifstream file(filename, std::ios::binary);
        if (file.good()) {
            foundFile = filename;
            break;
        }
    }

    if (foundFile.empty()) {
        std::string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(client, response.c_str(), (int)response.length(), 0);
        return;
    }

    std::ifstream file(foundFile, std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    file.read(buffer.data(), size);

    std::string contentType = "image/jpeg";
    if (foundFile.find(".png") != std::string::npos) contentType = "image/png";
    else if (foundFile.find(".gif") != std::string::npos) contentType = "image/gif";

    std::string header = "HTTP/1.1 200 OK\r\n";
    header += "Content-Type: " + contentType + "\r\n";
    header += "Content-Length: " + std::to_string(size) + "\r\n";
    header += "Connection: close\r\n\r\n";

    send(client, header.c_str(), (int)header.length(), 0);
    send(client, buffer.data(), (int)buffer.size(), 0);
}
void handleClient(SOCKET client) {
    char buffer[4096];
    int received = recv(client, buffer, sizeof(buffer) - 1, 0);

    if (received > 0) {
        buffer[received] = '\0';
        std::string request(buffer);

        if (request.find("GET / ") == 0) {
            sendHTTPResponse(client, getHTMLPage(), "text/html");
        }
        else if (request.find("GET /api/servers") == 0) {
            sendHTTPResponse(client, getServersJSON(), "application/json");
        }
        else if (request.find("GET /api/progress") == 0) {
            std::string json = "{";
            json += "\"in_progress\":" + std::string(g_QueryInProgress ? "true" : "false") + ",";
            json += "\"initial_complete\":" + std::string(g_InitialQueryComplete ? "true" : "false") + ",";
            json += "\"total\":" + std::to_string(g_TotalServers.load()) + ",";
            json += "\"queried\":" + std::to_string(g_QueriedServers.load()) + ",";
            json += "\"online\":" + std::to_string(g_OnlineServers.load()) + ",";

            json += "\"retry_in_progress\":" + std::string(g_RetryInProgress ? "true" : "false") + ",";
            json += "\"retry_total\":" + std::to_string(g_RetryTotal.load()) + ",";
            json += "\"retry_queried\":" + std::to_string(g_RetryQueried.load()) + ",";
            json += "\"retry_online\":" + std::to_string(g_RetryOnline.load());

            json += "}";
            sendHTTPResponse(client, json, "application/json");
        }
        else if (request.find("GET /api/search?name=") == 0) {
            size_t start = request.find("name=") + 5;
            size_t end = request.find(" ", start);
            if (end == std::string::npos) end = request.find("&", start);
            if (end == std::string::npos) end = request.find("\r", start);

            std::string searchName = urlDecode(request.substr(start, end - start));
            sendHTTPResponse(client, getPlayerSearchJSON(searchName), "application/json");
        }
        else if (request.find("GET /api/query?ip=") == 0) {
            size_t start = request.find("ip=") + 3;
            size_t end = request.find(" ", start);
            if (end == std::string::npos) end = request.find("&", start);
            if (end == std::string::npos) end = request.find("\r", start);

            std::string serverIP = urlDecode(request.substr(start, end - start));
            auto result = queryServer(serverIP);
            sendHTTPResponse(client, result.to_json(), "application/json");
        }
        else if (request.find("GET /api/config") == 0) {
            std::lock_guard<std::mutex> lock(configMutex);
            sendHTTPResponse(client, g_AppConfig.to_json(), "application/json");
        }
        else if (request.find("POST /api/config") == 0) {
            size_t bodyStart = request.find("\r\n\r\n");
            if (bodyStart != std::string::npos) {
                std::string body = request.substr(bodyStart + 4);

                configMutex.lock();
                g_AppConfig = AppConfig::from_json(body);
                configMutex.unlock();

                saveAppConfig();

                sendHTTPResponse(client, "{\"status\":\"success\"}", "application/json");
            }
            else {
                sendHTTPResponse(client, "{\"status\":\"error\"}", "application/json");
            }
        }
        else if (request.find("GET /background") == 0) {
            sendBackgroundImage(client);
            closesocket(client);
            return;
        }
        else if (request.find("POST /api/retry-offline") == 0) {
            std::thread([]() {
                g_RetryInProgress = true;

                std::vector<std::string> offlineServers;
                {
                    std::lock_guard<std::mutex> lock(cacheMutex);
                    for (const auto& pair : serverCache) {
                        if (!pair.second.error.empty()) {
                            offlineServers.push_back(pair.second.ip);
                        }
                    }
                }

                g_RetryTotal = offlineServers.size();
                g_RetryQueried = 0;
                g_RetryOnline = 0;

                std::cout << "\nRetrying " << offlineServers.size() << " offline servers...\n";

                const int MAX_CONCURRENT = g_AppConfig.retry_concurrent_threads;
                std::vector<std::thread> threads;

                for (const auto& serverIP : offlineServers) {
                    threads.emplace_back([serverIP]() {
                        auto result = queryServer(serverIP);

                        cacheMutex.lock();
                        serverCache[result.ip] = result;
                        cacheMutex.unlock();

                        if (result.error.empty()) {
                            g_RetryOnline++;
                        }

                        g_RetryQueried++;
                        });

                    if (threads.size() >= MAX_CONCURRENT) {
                        for (auto& t : threads) {
                            if (t.joinable()) t.join();
                        }
                        threads.clear();
                    }
                }

                for (auto& t : threads) {
                    if (t.joinable()) t.join();
                }

                std::cout << "Retry complete: " << g_RetryOnline.load() << "/" << offlineServers.size() << " came online\n";
                g_RetryInProgress = false;
                }).detach();

            sendHTTPResponse(client, "{\"status\":\"started\"}", "application/json");
        }
        else {
            sendHTTPResponse(client, "404 Not Found", "text/plain");
        }
    }

    closesocket(client);
}

int main() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    loadAppConfig();

    std::cout << "Loading servers from servers.txt...\n";
    auto servers = loadServers("servers.txt");

    if (servers.empty()) {
        std::cout << "ERROR: No valid servers found in servers.txt!\n";
        system("pause");
        return 1;
    }

    std::cout << "Loaded " << servers.size() << " servers\n";
    std::cout << "Max concurrent threads: " << g_AppConfig.max_concurrent_threads << "\n";
    std::cout << "Query timeout: " << g_AppConfig.query_timeout << "ms\n\n";

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(8080);

    bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(listenSocket, 10);

    std::cout << "Web server started on http://localhost:8080\n";
    std::cout << "Open your browser to watch progress\n";

    std::thread queryThread([servers]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        queryAllServersBackground(servers);
        });
    queryThread.detach();

    while (true) {
        SOCKET client = accept(listenSocket, nullptr, nullptr);
        if (client != INVALID_SOCKET) {
            std::thread(handleClient, client).detach();
        }
    }

    closesocket(listenSocket);
    WSACleanup();
    return 0;
}