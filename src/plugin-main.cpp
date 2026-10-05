#include <obs-module.h>
#include <plugin-support.h>

// Disable some warnings for external headers
#pragma warning(push)
#pragma warning(disable: 4244 4267)
#define NOMINMAX
#include "httplib.h"
#include "json.hpp"
#pragma warning(pop)

#include <thread>
#include <atomic>
#include <string>
#include <mutex>
#include <vector>
#include <fstream>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;
using json = nlohmann::json;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

std::atomic<bool> g_stop(false);
std::thread g_server_thread;
httplib::Server g_server;

// Global state
std::mutex g_media_mutex;
json g_media_state = json::object();
json g_settings = json::object();

// Helper: base64 encode
std::string EncodeBase64(const std::vector<uint8_t>& data) {
    if (data.empty()) return "";
    DWORD len = 0;
    CryptBinaryToStringA(data.data(), (DWORD)data.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::string result(len, '\0');
    CryptBinaryToStringA(data.data(), (DWORD)data.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &result[0], &len);
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

// Media polling thread
std::thread g_media_poll_thread;
void MediaPollWorker() {
    init_apartment();
    
    while (!g_stop) {
        try {
            auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            
            // Get all sessions
            auto sessionList = manager.GetSessions();
            std::vector<std::string> apps;
            for (auto&& s : sessionList) {
                apps.push_back(to_string(s.SourceAppUserModelId()));
            }

            // Target session
            std::string targetSource = "auto";
            {
                std::lock_guard<std::mutex> lock(g_media_mutex);
                if (g_settings.contains("source") && g_settings["source"].is_string()) {
                    targetSource = g_settings["source"].get<std::string>();
                }
            }

            GlobalSystemMediaTransportControlsSession session = nullptr;
            if (targetSource != "auto" && !targetSource.empty()) {
                for (auto&& s : sessionList) {
                    if (to_string(s.SourceAppUserModelId()) == targetSource) {
                        session = s;
                        break;
                    }
                }
            }
            if (!session) {
                session = manager.GetCurrentSession();
            }
            
            json newState = json::object();
            newState["title"] = "";
            newState["artist"] = "";
            newState["status"] = "Paused";
            newState["albumArt"] = nullptr;
            newState["availableApps"] = apps;
            newState["app"] = "";
            
            if (session) {
                newState["app"] = to_string(session.SourceAppUserModelId());
                auto properties = session.TryGetMediaPropertiesAsync().get();
                if (properties) {
                    newState["title"] = to_string(properties.Title());
                    newState["artist"] = to_string(properties.Artist());
                    
                    auto thumb = properties.Thumbnail();
                    if (thumb) {
                            auto stream = thumb.OpenReadAsync().get();
                            if (stream) {
                                uint32_t size = (uint32_t)stream.Size();
                                Buffer buffer(size);
                                stream.ReadAsync(buffer, size, InputStreamOptions::None).get();
                                
                                auto reader = DataReader::FromBuffer(buffer);
                                std::vector<uint8_t> bytes(buffer.Length());
                                reader.ReadBytes(bytes);
                                
                                newState["albumArt"] = EncodeBase64(bytes);
                            }
                    }
                }
                
                auto playback = session.GetPlaybackInfo();
                if (playback) {
                    if (playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                        newState["status"] = "Playing";
                    }
                }
            }
            
            {
                std::lock_guard<std::mutex> lock(g_media_mutex);
                g_media_state = newState;
            }
            
        } catch (...) {
            // Ignore errors (e.g. session disconnected) and continue
        }
        
        for (int i = 0; i < 30 && !g_stop; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

// Control function
void SendMediaCommand(const std::string& cmd) {
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        std::string targetSource = "auto";
        {
            std::lock_guard<std::mutex> lock(g_media_mutex);
            if (g_settings.contains("source") && g_settings["source"].is_string()) {
                targetSource = g_settings["source"].get<std::string>();
            }
        }

        GlobalSystemMediaTransportControlsSession session = nullptr;
        if (targetSource != "auto" && !targetSource.empty()) {
            auto sessionList = manager.GetSessions();
            for (auto&& s : sessionList) {
                if (to_string(s.SourceAppUserModelId()) == targetSource) {
                    session = s;
                    break;
                }
            }
        }
        if (!session) {
            session = manager.GetCurrentSession();
        }

        if (session) {
            if (cmd == "playpause") session.TryTogglePlayPauseAsync().get();
            else if (cmd == "next") session.TrySkipNextAsync().get();
            else if (cmd == "prev") session.TrySkipPreviousAsync().get();
        }
    } catch (...) {}
}

std::string g_data_path;

// Server Worker
void ServerWorker() {
    // API: Get Media
    g_server.Get("/api/media", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_media_mutex);
        res.set_content(g_media_state.dump(), "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });
    
    // API: Get Settings
    g_server.Get("/api/settings", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(g_settings.dump(), "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });
    
    // API: Post Settings
    g_server.Post("/api/settings", [](const httplib::Request& req, httplib::Response& res) {
        try {
            g_settings = json::parse(req.body);
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } catch (...) {
            res.status = 400;
        }
        res.set_header("Access-Control-Allow-Origin", "*");
    });
    
    // API: Control
    g_server.Get(R"(/api/control/(.*))", [](const httplib::Request& req, httplib::Response& res) {
        std::string cmd = req.matches[1];
        SendMediaCommand(cmd);
        res.set_content("{\"status\":\"ok\"}", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    // Serve static HTML from the dynamic plugin data directory
    if (!g_data_path.empty()) {
        g_server.set_mount_point("/", g_data_path);
    }
    
    g_server.listen("127.0.0.1", 18789);
}

bool obs_module_load(void)
{
    // Initialize default settings so HTML doesn't break
    g_settings = json::parse(R"({"source": "auto", "skin": "02", "bgColor": "#161b22", "opacity": 80, "brightness": 100, "barColor": "#ff4757", "autoShow": true, "showDuration": 5, "animationDir": "left", "ambilight": true, "borderRadius": 14})");

    char* path = obs_module_file("");
    if (path) {
        g_data_path = path;
        bfree(path);
    }

	obs_log(LOG_INFO, "obs-music-dock-plugin loaded successfully. Data path: %s", g_data_path.c_str());
	
	g_media_poll_thread = std::thread(MediaPollWorker);
	g_server_thread = std::thread(ServerWorker);

	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "obs-music-dock-plugin unloading...");
	g_stop = true;
	g_server.stop();
	
	if (g_server_thread.joinable()) g_server_thread.join();
	if (g_media_poll_thread.joinable()) g_media_poll_thread.join();
}
