#include "soloist.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "util.h"

namespace mango {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kDownload = "https://soloist-builds.spotifycdn.com/soloist_release_windows_x86_64.zip";
constexpr DWORD kExpiredExit = 10;  // Soloist builds stop working 90 days after they're built

std::string str(const json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

const json& sub(const json& j, const char* key) {
    static const json none;
    if (!j.is_object()) return none;
    auto it = j.find(key);
    return it == j.end() ? none : *it;
}

std::string quoteArg(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    return out + "\"";
}

std::string tail(const fs::path& p, size_t bytes) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    auto size = size_t(in.tellg());
    in.seekg(size > bytes ? size - bytes : 0);
    std::string s((std::istreambuf_iterator<char>(in)), {});
    return s;
}

}  // namespace

int64_t PlayerState::PositionNow(int64_t nowMs) const {
    int64_t p = positionMs;
    if (speed > 0 && positionAt > 0) p += int64_t(double(nowMs - positionAt) * speed);
    if (item.durationMs > 0) p = std::min(p, item.durationMs);
    return std::max<int64_t>(p, 0);
}

Entity ParseEntity(const json& j) {
    Entity e;
    if (!j.is_object()) return e;
    e.uri = str(j, "uri");
    e.type = str(j, "entity_type");
    const json& d = sub(j, "decorations");
    e.name = str(sub(d, "identity"), "name");
    const json& covers = sub(sub(d, "visual_identity"), "cover");
    if (covers.is_array()) {
        // "default" is about 300px; fall back through the others.
        for (const char* want : {"default", "large", "small", "xlarge"}) {
            for (const auto& c : covers)
                if (str(c, "size") == want) {
                    e.image = str(c, "url");
                    break;
                }
            if (!e.image.empty()) break;
        }
        if (e.image.empty() && !covers.empty()) e.image = str(covers[0], "url");
    }
    const json& parent = sub(sub(d, "parent"), "entity");
    e.parentUri = str(parent, "uri");
    e.parentName = str(sub(sub(parent, "decorations"), "identity"), "name");
    const json& creators = sub(d, "creators");
    if (creators.is_array())
        for (const auto& c : creators) {
            const json& ce = sub(c, "entity");
            e.creators.push_back({str(ce, "uri"), str(sub(sub(ce, "decorations"), "identity"), "name")});
        }
    const json& dur = sub(sub(d, "playback"), "duration_ms");
    if (dur.is_number()) e.durationMs = dur.get<int64_t>();
    return e;
}

static void applyPosition(PlayerState& s, const json& p) {
    if (!p.is_object()) return;
    if (sub(p, "position_ms").is_number()) s.positionMs = sub(p, "position_ms").get<int64_t>();
    if (sub(p, "timestamp_ms").is_number()) s.positionAt = sub(p, "timestamp_ms").get<int64_t>();
    if (sub(p, "speed").is_number()) s.speed = sub(p, "speed").get<double>();
}

static void applyOptions(PlayerState& s, const json& o) {
    if (!o.is_object()) return;
    if (sub(o, "shuffle").is_boolean()) s.shuffle = sub(o, "shuffle").get<bool>();
    if (sub(o, "repeat").is_string()) s.repeat = sub(o, "repeat").get<std::string>();
}

bool Apply(PlayerState& s, const json& m) {
    std::string type = str(m, "type");
    if (type == "auth_state") {
        if (sub(m, "logged_in").is_boolean()) s.loggedIn = sub(m, "logged_in").get<bool>();
        if (sub(m, "is_active").is_boolean()) s.active = sub(m, "is_active").get<bool>();
        if (sub(m, "device_name").is_string()) s.deviceName = str(m, "device_name");
    } else if (type == "playback_state") {
        s.status = str(m, "status");
        s.item = ParseEntity(sub(m, "item"));
        s.context = ParseEntity(sub(m, "context"));
        applyPosition(s, sub(m, "position"));
        if (sub(m, "volume").is_number()) s.volume = sub(m, "volume").get<int>();
        if (sub(m, "is_active").is_boolean()) s.active = sub(m, "is_active").get<bool>();
        applyOptions(s, sub(m, "options"));
    } else if (type == "track_changed") {
        s.item = ParseEntity(sub(m, "item"));
    } else if (type == "playback_changed") {
        s.status = str(m, "status");
    } else if (type == "volume_changed") {
        if (sub(m, "volume").is_number()) s.volume = sub(m, "volume").get<int>();
    } else if (type == "context_changed") {
        s.context = ParseEntity(sub(m, "context"));
    } else if (type == "options_changed") {
        applyOptions(s, sub(m, "options"));
    } else if (type == "device_changed") {
        if (sub(m, "is_active").is_boolean()) s.active = sub(m, "is_active").get<bool>();
        if (sub(m, "device_name").is_string()) s.deviceName = str(m, "device_name");
    } else if (type == "position_sync") {
        applyPosition(s, sub(m, "position"));
    } else if (type == "queue_changed") {
        s.upcoming.clear();
        const json& up = sub(m, "upcoming");
        if (up.is_array())
            for (const auto& q : up) s.upcoming.push_back({str(q, "source"), ParseEntity(sub(q, "item"))});
    } else {
        return false;
    }
    s.version++;
    return true;
}

Soloist::Soloist(std::wstring exe, std::wstring dataDir, std::function<void()> wake)
    : exe_(std::move(exe)), dataDir_(std::move(dataDir)), wake_(std::move(wake)) {
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));
    thread_ = std::thread(&Soloist::run, this);
}

Soloist::~Soloist() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    ws_.Close();
    kill();
    thread_.join();
    if (job_) CloseHandle(job_);
}

void Soloist::Start(const std::string& apiKey, const std::string& deviceName, int volume) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        key_ = apiKey;
        name_ = deviceName;
        if (volume >= 0) volume_ = volume;
        restart_ = true;
    }
    cv_.notify_all();
    ws_.Close();
    kill();
}

void Soloist::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        key_.clear();
        restart_ = true;
    }
    cv_.notify_all();
    ws_.Close();
    kill();
}

PlayerState Soloist::State() {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

void Soloist::Command(json cmd) {
    cmd["type"] = "command";
    ws_.Send(cmd.dump());
}

void Soloist::Play(const std::string& uri) {
    json c{{"command", "play"}};
    if (!uri.empty()) c["uri"] = uri;
    Command(c);
}
void Soloist::Pause() { Command({{"command", "pause"}}); }
void Soloist::Next() { Command({{"command", "skip_next"}}); }
void Soloist::Prev() { Command({{"command", "skip_prev"}}); }
void Soloist::Seek(int64_t ms) { Command({{"command", "seek"}, {"position_ms", ms}}); }
void Soloist::SetVolume(int v) {
    Command({{"command", "set_volume"}, {"volume", std::clamp(v, 0, 100)}});
    std::lock_guard<std::mutex> lock(mu_);
    volume_ = v;
}
void Soloist::SetShuffle(bool on) { Command({{"command", "set_shuffle"}, {"enabled", on}}); }
void Soloist::SetRepeat(const std::string& mode) {
    if (mode == "track") {
        Command({{"command", "set_repeat_context"}, {"enabled", false}});
        Command({{"command", "set_repeat_track"}, {"enabled", true}});
    } else {
        Command({{"command", "set_repeat_track"}, {"enabled", false}});
        Command({{"command", "set_repeat_context"}, {"enabled", mode == "context"}});
    }
}
void Soloist::Queue(const std::string& uri) { Command({{"command", "add_to_queue"}, {"uri", uri}}); }
void Soloist::Activate() { Command({{"command", "activate"}}); }
void Soloist::AskQueue(int limit) { Command({{"command", "get_queue"}, {"limit", limit}}); }

void Soloist::kill() {
    std::lock_guard<std::mutex> lock(mu_);
    if (process_) {
        TerminateProcess(process_, 0);
        CloseHandle(process_);
        process_ = nullptr;
    }
}

// download fetches the current Windows build next to exe_ (used when it's
// missing or has expired). Spotify asks that builds aren't redistributed,
// so each install fetches its own from Spotify.
static bool download(const std::wstring& exe, std::string& error) {
    fs::path dir = fs::path(exe).parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    Http http;
    auto r = http.Request("GET", kDownload, {}, {}, 200 << 20);
    if (!r.ok()) {
        error = "couldn't download Soloist: " + r.error;
        return false;
    }
    fs::path zip = dir / "soloist-download.zip";
    {
        std::ofstream out(zip, std::ios::binary);
        out.write(r.body.data(), std::streamsize(r.body.size()));
    }
    // Windows' own tar reads zips.
    std::wstring cmd = L"tar.exe -xf \"" + zip.wstring() + L"\" -C \"" + dir.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error = "couldn't unpack Soloist";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    fs::remove(zip, ec);
    if (code != 0 || !fs::exists(exe)) {
        error = "couldn't unpack Soloist";
        return false;
    }
    return true;
}

bool Soloist::launch(std::string& error) {
    std::string key, name;
    int volume;
    {
        std::lock_guard<std::mutex> lock(mu_);
        key = key_;
        name = name_;
        volume = volume_;
    }
    if (!fs::exists(exe_)) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            state_.run = PlayerState::Run::Starting;
            state_.runError = "downloading Spotify Soloist\xE2\x80\xA6";
            state_.version++;
        }
        wake_();
        if (!download(exe_, error)) return false;
    }
    std::error_code ec;
    fs::create_directories(dataDir_, ec);
    fs::path portFile = fs::path(dataDir_) / "ws.port";
    fs::remove(portFile, ec);

    std::string args = quoteArg(Narrow(exe_)) + " -n " + quoteArg(name) + " -k " + quoteArg(key) + " -D " +
                       quoteArg(Narrow(dataDir_)) + " -w 127.0.0.1:0 -i " + std::to_string(std::clamp(volume, 0, 100));
    std::wstring cmd = Widen(args);

    fs::path log = fs::path(dataDir_) / "soloist.log";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE logH = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = logH;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             fs::path(exe_).parent_path().c_str(), &si, &pi);
    if (logH != INVALID_HANDLE_VALUE) CloseHandle(logH);
    if (!ok) {
        error = "couldn't start Soloist (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    AssignProcessToJobObject(job_, pi.hProcess);
    // Below normal, like Mango itself: never compete with the game.
    SetPriorityClass(pi.hProcess, BELOW_NORMAL_PRIORITY_CLASS);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lock(mu_);
        process_ = pi.hProcess;
    }

    // Wait for it to write the port it took.
    for (int i = 0; i < 150; i++) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_ || restart_ || !process_) return false;
            if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) break;
        }
        std::ifstream in(portFile);
        int port = 0;
        if (in >> port && port > 0) {
            if (ws_.Connect("127.0.0.1", port, "/", error)) return true;
        }
        Sleep(100);
    }
    DWORD code = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (process_) GetExitCodeProcess(process_, &code);
    }
    if (code == kExpiredExit) error = "expired";
    else if (code != STILL_ACTIVE) error = "Soloist stopped (exit " + std::to_string(code) + "): " + tail(log, 300);
    else error = "Soloist didn't open its control port";
    return false;
}

void Soloist::readLoop() {
    ws_.Send(R"({"type":"command","command":"get_auth_state"})");
    std::string msg;
    while (ws_.Receive(msg)) {
        json j;
        try {
            j = json::parse(msg);
        } catch (...) {
            continue;
        }
        bool loggedInNow = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            bool was = state_.loggedIn;
            Apply(state_, j);
            loggedInNow = !was && state_.loggedIn;
        }
        if (loggedInNow) {
            ws_.Send(R"({"type":"command","command":"get_state"})");
            ws_.Send(R"({"type":"command","command":"get_queue","limit":50})");
        }
        wake_();
    }
}

void Soloist::run() {
    int failures = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !key_.empty(); });
            if (stop_) return;
            restart_ = false;
            state_ = PlayerState{};
            state_.run = PlayerState::Run::Starting;
            state_.version++;
        }
        wake_();
        std::string error;
        bool up = launch(error);
        if (!up && error == "expired") {
            // Fetch the current build and go again.
            kill();
            std::string derr;
            std::error_code ec;
            fs::remove(exe_, ec);
            if (download(exe_, derr)) continue;
            error = "this Soloist build has expired and a new one couldn't be fetched: " + derr;
        }
        if (up) {
            failures = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                state_.run = PlayerState::Run::Running;
                state_.connected = true;
                state_.version++;
            }
            wake_();
            readLoop();
            ws_.Close();
            DWORD code = STILL_ACTIVE;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (process_) {
                    WaitForSingleObject(process_, 2000);
                    GetExitCodeProcess(process_, &code);
                }
                state_.connected = false;
                state_.version++;
            }
            if (code == kExpiredExit) {
                kill();
                std::string derr;
                std::error_code ec;
                fs::remove(exe_, ec);
                download(exe_, derr);
                continue;
            }
            error = code == STILL_ACTIVE ? "lost touch with Soloist" : "Soloist stopped (exit " + std::to_string(code) + ")";
        }
        kill();
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            if (restart_) continue;
            state_.run = PlayerState::Run::Failed;
            state_.runError = error;
            state_.connected = false;
            state_.version++;
        }
        wake_();
        // Back off: 2s, 4s, ... up to a minute.
        failures++;
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::seconds(std::min(60, 1 << std::min(failures, 6))), [&] { return stop_ || restart_; });
        if (stop_) return;
    }
}

}  // namespace mango
