// Spotify Soloist: Spotify's own Connect player, run hidden as Mango's
// speaker and steered over its local WebSocket API.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net.h"
#include "nlohmann/json.hpp"

namespace mango {

struct Entity {
    std::string uri, type, name, image;   // image: a mid-size cover
    std::string parentUri, parentName;    // album for a track
    std::vector<std::pair<std::string, std::string>> creators;  // uri, name
    int64_t durationMs = 0;
};

struct QueueEntry {
    std::string source;  // context, queue, autoplay
    Entity item;
};

struct PlayerState {
    enum class Run { Missing, Starting, Running, Expired, Failed } run = Run::Missing;
    std::string runError;   // why it isn't running
    bool connected = false; // WebSocket open
    bool loggedIn = false;
    bool active = false;
    std::string deviceName;
    std::string status = "idle";  // idle, playing, paused, buffering
    Entity item, context;
    int64_t positionMs = 0, positionAt = 0;  // anchor: position at Unix ms
    double speed = 0;
    int volume = -1;
    bool shuffle = false;
    std::string repeat = "off";
    std::vector<QueueEntry> upcoming;
    uint64_t version = 0;  // bumps on every change

    int64_t PositionNow(int64_t nowMs) const;
};

// ParseEntity and Apply are separate so they can be tested.
Entity ParseEntity(const nlohmann::json& j);
// Apply updates state from one event message; returns false if unknown.
bool Apply(PlayerState& s, const nlohmann::json& msg);

class Soloist {
public:
    // exe: soloist.exe; dataDir: its credentials and ws.port.
    Soloist(std::wstring exe, std::wstring dataDir, std::function<void()> wake);
    ~Soloist();

    // Start runs it with this API key (restarting if it was running).
    void Start(const std::string& apiKey, const std::string& deviceName, int volume);
    void Stop();

    PlayerState State();
    void Command(nlohmann::json cmd);  // {"command": ...}; "type" is added

    // Convenience.
    void Play(const std::string& uri = {});
    void Pause();
    void Next();
    void Prev();
    void Seek(int64_t ms);
    void SetVolume(int v);
    void SetShuffle(bool on);
    void SetRepeat(const std::string& mode);  // off, context, track
    void Queue(const std::string& uri);
    void Activate();
    void AskQueue(int limit);

private:
    void run();
    bool launch(std::string& error);
    void kill();
    void readLoop();

    std::wstring exe_, dataDir_;
    std::function<void()> wake_;
    std::string key_, name_;
    int volume_ = 40;

    std::mutex mu_;
    std::condition_variable cv_;
    PlayerState state_;
    bool stop_ = false;
    bool restart_ = false;
    std::thread thread_;
    WebSocket ws_;
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
};

}  // namespace mango
