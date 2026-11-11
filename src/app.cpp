#include "app.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "imgui_internal.h"
#include "util.h"

namespace mango {

namespace fs = std::filesystem;

namespace {

// Mango's Spotify app (developer dashboard). Not a secret: PKCE needs none.
constexpr const char* kClientId = "592084482f66443eb7c95a930cb5af24";
constexpr const char* kDeviceName = "Mango";
constexpr int kHotPlay = 1, kHotNext = 2, kHotPrev = 3;

// Font sizes, before DPI scaling.
constexpr float kBody = 14, kSmall = 12.5f, kLabel = 11, kTitle = 16;

ImVec4 hex(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

// kitchensink's colours, plus mango for the name.
struct Palette {
    ImVec4 bg, card, sidebar, border, text, muted, faint, accent, hover, frame, accentText, warn, bad, mango;
};
const Palette kDark = {hex(0x15171C), hex(0x1E2128), hex(0x1A1D23), hex(0x2B2F38), hex(0xE6E8EC), hex(0x8C929D),
                       hex(0x5C626D), hex(0x5AA8FF), hex(0x262A33), hex(0x22252D), hex(0xFFFFFF), hex(0xD9A23A),
                       hex(0xF8645A), hex(0xF59E2B)};
const Palette kLight = {hex(0xF2F3F5), hex(0xFFFFFF), hex(0xEAECEF), hex(0xD5D9DF), hex(0x1F2328), hex(0x656D76),
                        hex(0x9AA1AA), hex(0x0969DA), hex(0xE4E7EB), hex(0xEEF0F3), hex(0xFFFFFF), hex(0x9A6700),
                        hex(0xCF222E), hex(0xD9750B)};
const Palette* pal = &kDark;

ImU32 col(const ImVec4& v, float a = 1) { return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, v.w * a)); }

namespace icon {
const char* Play = "\xEE\x9D\xA8";
const char* Pause = "\xEE\x9D\xA9";
const char* Prev = "\xEE\xA2\x92";
const char* Next = "\xEE\xA2\x93";
const char* Shuffle = "\xEE\xA2\xB1";
const char* Repeat = "\xEE\xA3\xAE";
const char* RepeatOne = "\xEE\xA3\xAD";
const char* Volume = "\xEE\x9D\xA7";
const char* Mute = "\xEE\x9D\x8F";
const char* Heart = "\xEE\xAD\x91";
const char* HeartFill = "\xEE\xAD\x92";
const char* Back = "\xEE\x9C\xAB";
const char* Search = "\xEE\x9C\xA1";
const char* Settings = "\xEE\x9C\x93";
const char* Minimise = "\xEE\xA4\xA1";
const char* Close = "\xEE\xA2\xBB";
const char* Queue = "\xEE\xA3\xBD";
const char* Music = "\xEE\xA3\x96";
const char* Refresh = "\xEE\x9C\xAC";
const char* Speaker = "\xEE\xA6\x95";
const char* Person = "\xEE\x9D\xBB";
const char* Filter = "\xEE\x9C\x9D";
}  // namespace icon

std::string fontPath(const char* file) {
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    return std::string(dir) + "\\Fonts\\" + file;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::vector<char>* fontData(const std::string& path) {
    static std::map<std::string, std::vector<char>> files;
    auto it = files.find(path);
    if (it != files.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<char>& data = files[path];
    std::ifstream f(path, std::ios::binary);
    if (f) data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return data.empty() ? nullptr : &data;
}

ImFont* addFont(const std::string& path, ImFontConfig cfg, const ImWchar* ranges = nullptr) {
    std::vector<char>* data = fontData(path);
    if (!data) return nullptr;
    cfg.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data->data(), int(data->size()), kBody, &cfg, ranges);
}

// Scripts whose fonts load only when a title needs them (each is big).
enum Script { Han = 1, Hangul = 2 };
int wantedScripts = 0, loadedScripts = 0;

void noteText(std::string_view text) {
    if ((wantedScripts | loadedScripts) == (Han | Hangul)) return;
    for (size_t i = 0; i + 2 < text.size(); i++) {
        unsigned char b = (unsigned char)text[i];
        if (b < 0xE1 || b > 0xEF) continue;
        uint32_t cp = ((b & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
        if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0xAC00 && cp <= 0xD7AF))
            wantedScripts |= Hangul;
        else if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF))
            wantedScripts |= Han;
        i += 2;
    }
}

void noteTracks(const std::vector<Track>& ts) {
    for (const auto& t : ts) {
        noteText(t.name);
        noteText(t.album.name);
        for (const auto& a : t.artists) noteText(a.name);
    }
}

std::string joinNames(const std::vector<Ref>& refs) {
    std::string s;
    for (const auto& r : refs) s += (s.empty() ? "" : ", ") + r.name;
    return s;
}

std::string idOf(const std::string& uri) {
    size_t c = uri.rfind(':');
    return c == std::string::npos ? uri : uri.substr(c + 1);
}

bool uriIs(const std::string& uri, const char* type) { return uri.rfind(std::string("spotify:") + type + ":", 0) == 0; }

std::string songs(int n) { return std::to_string(n) + (n == 1 ? " song" : " songs"); }

const char* albumKind(const std::string& type) {
    if (type == "single") return "Single";
    if (type == "compilation") return "Compilation";
    return "Album";
}

std::string ellipsize(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    const char* dots = "\xE2\x80\xA6";
    float dw = ImGui::CalcTextSize(dots).x;
    // Cut at UTF-8 boundaries until it fits.
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > 0 && (s[mid] & 0xC0) == 0x80) mid--;
        if (mid <= lo) break;
        if (ImGui::CalcTextSize(s.c_str(), s.c_str() + mid).x + dw <= width) lo = mid;
        else hi = mid - 1;
    }
    return s.substr(0, lo) + dots;
}

// textAt draws one line, cut to fit.
void textAt(ImVec2 pos, const std::string& s, ImVec4 colour, float width) {
    std::string t = ellipsize(s, width);
    ImGui::GetWindowDrawList()->AddText(pos, col(colour), t.c_str());
}

}  // namespace

// ---------------------------------------------------------------- workers

void Inbox::Post(std::function<void()> f) {
    {
        std::lock_guard<std::mutex> lock(mu);
        if (closed) return;
        items.push_back(std::move(f));
    }
    if (wake) wake();
}

Workers::Workers(int n) : s_(std::make_shared<Shared>()) {
    for (int i = 0; i < n; i++) {
        std::thread([s = s_] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(s->mu);
                    s->cv.wait(lock, [&] { return s->stop || !s->jobs.empty(); });
                    if (s->stop) return;
                    job = std::move(s->jobs.front());
                    s->jobs.pop_front();
                }
                job();
            }
        }).detach();
    }
}

Workers::~Workers() {
    {
        std::lock_guard<std::mutex> lock(s_->mu);
        s_->stop = true;
        s_->jobs.clear();
    }
    s_->cv.notify_all();
}

void Workers::Run(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(s_->mu);
        s_->jobs.push_back(std::move(job));
    }
    s_->cv.notify_one();
}

// ---------------------------------------------------------------- setup

namespace {

struct DeviceCache {
    std::mutex mu;
    std::string id;
};

// findDevice looks up Soloist among the account's Connect devices, asking
// it to become active if it isn't listed yet.
std::string findDevice(Api& api, Soloist& sol, DeviceCache& cache, bool fresh, std::string& error) {
    std::lock_guard<std::mutex> lock(cache.mu);
    if (!fresh && !cache.id.empty()) return cache.id;
    cache.id.clear();
    std::string name = Lower(sol.State().deviceName.empty() ? kDeviceName : sol.State().deviceName);
    for (int attempt = 0; attempt < 3; attempt++) {
        std::vector<Device> ds;
        if (!api.Devices(ds, error)) return {};
        for (const auto& d : ds)
            if (Lower(d.name) == name) return cache.id = d.id;
        sol.Activate();
        Sleep(1200);
    }
    error = "Spotify doesn't list Mango's player yet";
    return {};
}

std::shared_ptr<DeviceCache> devices = std::make_shared<DeviceCache>();

}  // namespace

App::App(HWND hwnd, ID3D11Device* device, std::function<void()> wake)
    : hwnd_(hwnd), wake_(wake), inbox_(std::make_shared<Inbox>()), workers_(3) {
    inbox_->wake = wake;
    if (const wchar_t* d = _wgetenv(L"MANGO_DATA"); d && *d) {
        dir_ = Narrow(d);
    } else {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)))
            dir_ = (fs::path(p) / "mango").u8string();
        CoTaskMemFree(p);
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(dir_), ec);
    loadConfig();
    volumeShown_ = cfg_.volume;

    api_ = std::make_shared<Api>(kClientId);
    Tokens t;
    t.refresh = Unprotect(cfg_.refresh);
    api_->SetTokens(t);
    auto inbox = inbox_;
    api_->OnTokens([inbox, this](const Tokens& t) {
        std::string refresh = t.refresh;
        inbox->Post([this, refresh] {
            std::string sealed = refresh.empty() ? "" : Protect(refresh);
            if (sealed != cfg_.refresh) {
                cfg_.refresh = sealed;
                saveConfig();
            }
        });
    });

    images_ = std::make_unique<ImageCache>(device, wake);

    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    std::wstring base = local ? local : L"";
    std::wstring exe = base + L"\\Programs\\mango\\soloist\\soloist.exe";
    std::wstring data = _wgetenv(L"MANGO_DATA") && *_wgetenv(L"MANGO_DATA") ? Widen(dir_) + L"\\soloist"
                                                                             : base + L"\\mango\\soloist";
    soloist_ = std::make_shared<Soloist>(exe, data, wake);
    startSoloist();

    // Fonts: Segoe UI with Segoe's icons and symbols merged in.
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    std::string iconFont = fontPath("SegoeIcons.ttf");
    if (!exists(iconFont)) iconFont = fontPath("segmdl2.ttf");
    auto add = [&](const char* file) -> ImFont* {
        ImFont* f = addFont(fontPath(file), ImFontConfig());
        if (!f) return io.Fonts->AddFontDefault();
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.GlyphOffset = ImVec2(0, 1);
        static const ImWchar range[] = {0xE700, 0xF8FF, 0};
        addFont(iconFont, merge, range);
        ImFontConfig sym;
        sym.MergeMode = true;
        addFont(fontPath("seguisym.ttf"), sym);
        return f;
    };
    regular_ = add("segoeui.ttf");
    semibold_ = add("seguisb.ttf");
    bold_ = add("segoeuib.ttf");
    io.FontDefault = regular_;
    applyColours();

    if (const wchar_t* d = _wgetenv(L"MANGO_DEMO"); d && *d) {
        demo();
        return;
    }
    if (signedIn()) {
        loadMe();
        loadPlaylists();
        openKey(cfg_.lastView.empty() ? "liked" : cfg_.lastView);
    }
}

// demo fills Mango with made-up music and a playing song, for checking
// the layout without an account (MANGO_DEMO=1; touches no network).
void App::demo() {
    demo_ = true;
    meId_ = "demo";
    meName_ = "Demo";
    cfg_.soloistKey = "x";
    cfg_.queueOpen = _wgetenv(L"MANGO_DEMO")[0] == L'q';
    const char* names[] = {"Night Drive", "Gym", "Sunday Morning", "Old Favourites", "Guitar Heroes", "Focus",
                           "Road Trip 2026", "Chill", "Discover the Archive", "Band Practice", "Rainy Day", "Party"};
    for (int i = 0; i < 12; i++) {
        Playlist p;
        p.id = "p" + std::to_string(i);
        p.uri = "spotify:playlist:" + p.id;
        p.name = names[i];
        p.owner = i % 4 == 3 ? "Someone Else" : "Demo";
        p.ownerId = i % 4 == 3 ? "else" : "demo";
        p.total = 10 + i * 7;
        playlists_.push_back(p);
    }
    const char* songs[] = {"Through the Fire and Flames", "Hallowed Be Thy Name", "Paranoid Android", "Bohemian Rhapsody",
                           "Comfortably Numb", "Stairway to Heaven", "Everlong", "Black Hole Sun",
                           "\xE5\xA4\x9C\xE3\x81\xAB\xE9\xA7\x86\xE3\x81\x91\xE3\x82\x8B", "Mr. Brightside"};
    const char* bands[] = {"DragonForce", "Iron Maiden", "Radiohead", "Queen", "Pink Floyd", "Led Zeppelin",
                           "Foo Fighters", "Soundgarden", "YOASOBI", "The Killers"};
    auto v = std::make_shared<View>();
    v->kind = Kind::Liked;
    v->key = "liked";
    v->title = "Liked Songs";
    v->kindLabel = "Playlist";
    for (int i = 0; i < 60; i++) {
        Track t;
        t.id = "t" + std::to_string(i);
        t.uri = "spotify:track:" + t.id;
        t.name = songs[(i + 8) % 10];
        t.artists = {{"a" + std::to_string(i % 10), "spotify:artist:a", bands[(i + 8) % 10]}};
        if (i % 7 == 2) t.artists.push_back({"g", "spotify:artist:g", "Guest Star"});
        t.album = {"al" + std::to_string(i % 10), "spotify:album:al", std::string("Greatest Hits Vol. ") + std::to_string(i % 4 + 1)};
        t.durationMs = 150000 + i * 7919 % 240000;
        t.explicit_ = i % 5 == 1;
        v->tracks.push_back(t);
    }
    noteTracks(v->tracks);
    v->total = 60;
    v->done = true;
    cache_["liked"] = v;
    view_ = v;
    player_.run = PlayerState::Run::Running;
    player_.connected = player_.loggedIn = player_.active = true;
    player_.status = "playing";
    player_.item.uri = "spotify:track:t2";
    player_.item.type = "track";
    player_.item.name = songs[2];
    player_.item.parentUri = "spotify:album:al2";
    player_.item.parentName = "OK Computer";
    player_.item.creators = {{"spotify:artist:a2", bands[2]}};
    player_.item.durationMs = 383000;
    player_.positionMs = 121000;
    player_.positionAt = NowMs();
    player_.speed = 1;
    player_.volume = 55;
    volumeShown_ = 55;
    player_.context.uri = "spotify:user:demo:collection";
    for (int i = 3; i < 9; i++) {
        QueueEntry q;
        q.source = i == 3 ? "queue" : "context";
        q.item.name = songs[i];
        q.item.creators = {{"spotify:artist:x", bands[i]}};
        player_.upcoming.push_back(q);
    }
    likedUri_ = player_.item.uri;
    likedState_ = 1;
}

App::~App() {
    {
        std::lock_guard<std::mutex> lock(inbox_->mu);
        inbox_->closed = true;
        inbox_->items.clear();
    }
    if (cancelSignIn_) *cancelSignIn_ = true;
    if (hotkeys_) {
        UnregisterHotKey(hwnd_, kHotPlay);
        UnregisterHotKey(hwnd_, kHotNext);
        UnregisterHotKey(hwnd_, kHotPrev);
    }
    saveConfig();
}

void App::loadConfig() {
    std::ifstream in(fs::u8path(dir_) / "config.json");
    if (!in) return;
    try {
        auto j = nlohmann::json::parse(in);
        if (j.contains("window")) {
            const auto& w = j["window"];
            cfg_.x = w.value("x", 0);
            cfg_.y = w.value("y", 0);
            cfg_.w = w.value("w", 0);
            cfg_.h = w.value("h", 0);
            cfg_.placed = cfg_.w > 0 && cfg_.h > 0;
        }
        cfg_.shown = j.value("shown", true);
        cfg_.refresh = j.value("refresh", "");
        cfg_.soloistKey = j.value("soloistKey", "");
        cfg_.volume = std::clamp(j.value("volume", 40), 0, 100);
        cfg_.queueOpen = j.value("queueOpen", false);
        cfg_.lastView = j.value("lastView", "");
    } catch (...) {
    }
}

void App::saveConfig() {
    if (demo_) return;
    nlohmann::json j;
    if (cfg_.placed) j["window"] = {{"x", cfg_.x}, {"y", cfg_.y}, {"w", cfg_.w}, {"h", cfg_.h}};
    j["shown"] = cfg_.shown;
    j["refresh"] = cfg_.refresh;
    j["soloistKey"] = cfg_.soloistKey;
    j["volume"] = cfg_.volume;
    j["queueOpen"] = cfg_.queueOpen;
    j["lastView"] = cfg_.lastView;
    fs::path tmp = fs::u8path(dir_) / "config.json.tmp";
    {
        std::ofstream out(tmp);
        out << j.dump(2) << "\n";
    }
    std::error_code ec;
    fs::rename(tmp, fs::u8path(dir_) / "config.json", ec);
}

bool App::Place(RECT& r) const {
    if (!cfg_.placed) return false;
    r = RECT{cfg_.x, cfg_.y, cfg_.x + cfg_.w, cfg_.y + cfg_.h};
    return true;
}

void App::SavePlace(const RECT& r) {
    cfg_.placed = true;
    cfg_.x = r.left;
    cfg_.y = r.top;
    cfg_.w = r.right - r.left;
    cfg_.h = r.bottom - r.top;
    saveConfig();
}

void App::Show(bool shown) {
    if (shown) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
    } else {
        ShowWindow(hwnd_, SW_HIDE);  // the music carries on
    }
    if (cfg_.shown == shown) return;
    cfg_.shown = shown;
    saveConfig();
}

void App::startSoloist() {
    std::string key = Unprotect(cfg_.soloistKey);
    if (key.empty()) soloist_->Stop();
    else soloist_->Start(key, kDeviceName, cfg_.volume);
}

void App::note(const std::string& text) {
    note_ = text;
    noteUntil_ = ImGui::GetTime() + 4;
}

void App::signIn() {
    if (signingIn_) return;
    signingIn_ = true;
    signInError_.clear();
    cancelSignIn_ = std::make_shared<std::atomic<bool>>(false);
    auto api = api_;
    auto cancel = cancelSignIn_;
    auto inbox = inbox_;
    workers_.Run([api, cancel, inbox, this] {
        std::string err;
        bool ok = api->SignIn(err, *cancel);
        inbox->Post([this, ok, err] {
            signingIn_ = false;
            if (!ok) {
                signInError_ = err == "cancelled" ? "" : err;
                return;
            }
            loadMe();
            loadPlaylists();
            openLiked();
        });
    });
}

void App::signOut() {
    api_->SetTokens({});
    cfg_.refresh.clear();
    cfg_.lastView.clear();
    saveConfig();
    playlists_.clear();
    cache_.clear();
    backStack_.clear();
    view_.reset();
    meId_.clear();
    meName_.clear();
}

void App::loadMe() {
    auto api = api_;
    auto inbox = inbox_;
    workers_.Run([api, inbox, this] {
        std::string id, name, err;
        if (!api->Me(id, name, err)) return;
        inbox->Post([this, id, name] {
            meId_ = id;
            meName_ = name;
        });
    });
}

void App::loadPlaylists(int offset) {
    if (offset == 0) {
        if (playlistsLoading_) return;
        playlistsError_.clear();
    }
    playlistsLoading_ = true;
    auto api = api_;
    auto inbox = inbox_;
    workers_.Run([api, inbox, offset, this] {
        Page<Playlist> page;
        std::string err;
        bool ok = api->MyPlaylists(offset, page, err);
        inbox->Post([this, ok, page, err, offset] {
            if (!ok) {
                playlistsLoading_ = false;
                playlistsError_ = err;
                return;
            }
            if (offset == 0) playlists_.clear();
            for (const auto& p : page.items) noteText(p.name);
            playlists_.insert(playlists_.end(), page.items.begin(), page.items.end());
            if (page.more) loadPlaylists(offset + int(page.items.size()));
            else playlistsLoading_ = false;
        });
    });
}

// ---------------------------------------------------------------- views

std::shared_ptr<View> App::cached(const std::string& key) {
    auto it = cache_.find(key);
    return it == cache_.end() ? nullptr : it->second;
}

void App::open(std::shared_ptr<View> v, bool push) {
    if (view_ == v) return;
    if (push && view_) {
        backStack_.push_back(view_);
        if (backStack_.size() > 50) backStack_.erase(backStack_.begin());
    }
    view_ = v;
    v->restoreScroll = true;
    if (v->kind != Kind::Search && cfg_.lastView != v->key) {
        cfg_.lastView = v->key;
        saveConfig();
    }
}

void App::back() {
    if (backStack_.empty()) return;
    view_ = backStack_.back();
    backStack_.pop_back();
    view_->restoreScroll = true;
    if (view_->kind == Kind::Search) {
        std::snprintf(search_, sizeof(search_), "%s", view_->id.c_str());
        searched_ = view_->id;
    }
}

void App::openKey(const std::string& key) {
    if (key == "liked") return openLiked();
    size_t c = key.find(':');
    if (c == std::string::npos) return openLiked();
    std::string kind = key.substr(0, c), id = key.substr(c + 1);
    if (kind == "playlist") openPlaylistId(id, "");
    else if (kind == "album") openAlbum(id, "");
    else if (kind == "artist") openArtist(id, "");
    else openLiked();
}

void App::openLiked() {
    if (auto v = cached("liked")) return open(v);
    auto v = std::make_shared<View>();
    v->kind = Kind::Liked;
    v->key = "liked";
    v->title = "Liked Songs";
    v->kindLabel = "Playlist";
    cache_[v->key] = v;
    open(v);
    loadTracks(v, 0);
}

void App::openPlaylist(const Playlist& p) {
    std::string key = "playlist:" + p.id;
    if (auto v = cached(key)) return open(v);
    auto v = std::make_shared<View>();
    v->kind = Kind::Playlist;
    v->key = key;
    v->id = p.id;
    v->title = p.name;
    v->kindLabel = p.collaborative ? "Collaborative playlist" : "Playlist";
    v->byline = p.owner;
    v->image = p.image;
    v->uri = p.uri;
    v->contextUri = p.uri.empty() ? "spotify:playlist:" + p.id : p.uri;
    v->total = p.total;
    cache_[key] = v;
    open(v);
    loadTracks(v, 0);
}

void App::openPlaylistId(const std::string& id, const std::string& name) {
    for (const auto& p : playlists_)
        if (p.id == id) return openPlaylist(p);
    Playlist p;
    p.id = id;
    p.name = name;
    p.uri = "spotify:playlist:" + id;
    std::string key = "playlist:" + id;
    bool had = cached(key) != nullptr;
    openPlaylist(p);
    if (had) return;
    // Fill in the header.
    auto v = view_;
    auto api = api_;
    auto inbox = inbox_;
    workers_.Run([api, inbox, v, id, this] {
        Playlist full;
        std::string err;
        if (!api->GetPlaylist(id, full, err)) return;
        inbox->Post([v, full] {
            v->title = full.name;
            v->byline = full.owner;
            v->image = full.image;
            v->total = full.total;
            if (full.collaborative) v->kindLabel = "Collaborative playlist";
        });
    });
}

void App::openAlbum(const std::string& id, const std::string& name) {
    std::string key = "album:" + id;
    if (auto v = cached(key)) return open(v);
    auto v = std::make_shared<View>();
    v->kind = Kind::Album;
    v->key = key;
    v->id = id;
    v->title = name;
    v->kindLabel = "Album";
    v->uri = v->contextUri = "spotify:album:" + id;
    cache_[key] = v;
    open(v);
    loadTracks(v, 0);
}

void App::openArtist(const std::string& id, const std::string& name) {
    std::string key = "artist:" + id;
    if (auto v = cached(key)) return open(v);
    auto v = std::make_shared<View>();
    v->kind = Kind::Artist;
    v->key = key;
    v->id = id;
    v->title = name;
    v->kindLabel = "Artist";
    v->uri = v->contextUri = "spotify:artist:" + id;
    v->loading = true;
    cache_[key] = v;
    open(v);
    auto api = api_;
    auto inbox = inbox_;
    workers_.Run([api, inbox, v, id] {
        Artist a;
        std::string err;
        bool ok = api->GetArtist(id, a, err);
        std::vector<Album> albums;
        int offset = 0;
        // Albums, singles and compilations: a few pages at most.
        for (int i = 0; ok && i < 6; i++) {
            Page<Album> page;
            if (!api->ArtistAlbums(id, offset, page, err)) break;
            albums.insert(albums.end(), page.items.begin(), page.items.end());
            offset += int(page.items.size());
            if (!page.more || page.items.empty()) break;
        }
        inbox->Post([v, ok, a, albums, err] {
            v->loading = false;
            v->done = true;
            if (!ok) {
                v->error = err;
                return;
            }
            v->title = a.name;
            v->image = a.image;
            std::string genres;
            for (size_t i = 0; i < a.genres.size() && i < 3; i++) genres += (i ? ", " : "") + a.genres[i];
            v->byline = genres;
            v->albums = albums;
            noteText(a.name);
            for (const auto& al : albums) noteText(al.name);
        });
    });
}

void App::runSearch(const std::string& q) {
    std::string text = q;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    searched_ = q;
    if (text.empty()) return;
    std::string key = "search:" + text;
    auto v = cached(key);
    bool replace = view_ && view_->kind == Kind::Search;
    if (v) return open(v, !replace);
    v = std::make_shared<View>();
    v->kind = Kind::Search;
    v->key = key;
    v->id = text;
    v->title = text;
    v->loading = true;
    cache_[key] = v;
    open(v, !replace);
    auto api = api_;
    auto inbox = inbox_;
    workers_.Run([api, inbox, v, text] {
        SearchResults r;
        std::string err;
        bool ok = api->Search(text, "track,album,artist,playlist", 0, r, err);
        inbox->Post([v, ok, r, err] {
            v->loading = false;
            v->done = true;
            if (!ok) {
                v->error = err;
                return;
            }
            v->search = r;
            v->tracks = r.tracks.items;
            noteTracks(v->tracks);
            for (const auto& a : r.albums.items) noteText(a.name);
            for (const auto& a : r.artists.items) noteText(a.name);
            for (const auto& p : r.playlists.items) noteText(p.name);
        });
    });
}

void App::moreSearch(std::shared_ptr<View> v, const char* type) {
    if (v->loading) return;
    std::string t = type;
    int offset = t == "track" ? int(v->tracks.size())
               : t == "album" ? int(v->search.albums.items.size())
               : t == "artist" ? int(v->search.artists.items.size())
                               : int(v->search.playlists.items.size());
    v->loading = true;
    auto api = api_;
    auto inbox = inbox_;
    std::string q = v->id;
    workers_.Run([api, inbox, v, q, t, offset] {
        SearchResults r;
        std::string err;
        bool ok = api->Search(q, t, offset, r, err);
        inbox->Post([v, ok, r, err, t] {
            v->loading = false;
            if (!ok) {
                v->error = err;
                return;
            }
            auto& s = v->search;
            if (t == "track") {
                v->tracks.insert(v->tracks.end(), r.tracks.items.begin(), r.tracks.items.end());
                s.tracks.more = r.tracks.more;
                noteTracks(r.tracks.items);
            } else if (t == "album") {
                s.albums.items.insert(s.albums.items.end(), r.albums.items.begin(), r.albums.items.end());
                s.albums.more = r.albums.more;
            } else if (t == "artist") {
                s.artists.items.insert(s.artists.items.end(), r.artists.items.begin(), r.artists.items.end());
                s.artists.more = r.artists.more;
            } else {
                s.playlists.items.insert(s.playlists.items.end(), r.playlists.items.begin(), r.playlists.items.end());
                s.playlists.more = r.playlists.more;
            }
        });
    });
}

void App::loadTracks(std::shared_ptr<View> v, int offset) {
    if (offset == 0) {
        v->tracks.clear();
        v->done = false;
        v->error.clear();
        v->notListed = false;
        v->loadGen++;
    }
    v->loading = true;
    uint64_t gen = v->loadGen;
    auto api = api_;
    auto inbox = inbox_;
    workers_.Run([api, inbox, v, offset, gen, this] {
        Page<Track> page;
        std::string err;
        int status = 0;
        bool ok = false;
        Album album;
        bool haveAlbum = false;
        switch (v->kind) {
        case Kind::Liked: ok = api->Liked(offset, page, err); break;
        case Kind::Playlist: ok = api->PlaylistItems(v->id, offset, page, err, &status); break;
        case Kind::Album:
            if (offset == 0) {
                ok = api->GetAlbum(v->id, album, page, err);
                haveAlbum = ok;
            } else {
                album.id = v->id;
                album.uri = v->uri;
                album.name = v->title;
                album.image = v->image;
                ok = api->AlbumTracks(album, offset, page, err);
            }
            break;
        default: return;
        }
        inbox->Post([this, v, ok, page, err, status, offset, gen, album, haveAlbum] {
            if (v->loadGen != gen) return;  // reloaded meanwhile
            if (!ok) {
                v->loading = false;
                v->done = true;
                // Spotify lists only playlists the user owns or collaborates on.
                if (v->kind == Kind::Playlist && (status == 403 || status == 404)) v->notListed = true;
                else v->error = err;
                return;
            }
            if (haveAlbum) {
                v->title = album.name;
                v->image = album.image;
                v->kindLabel = albumKind(album.type);
                v->artists = album.artists;
                v->byline = album.year;
                noteText(album.name);
            }
            noteTracks(page.items);
            v->tracks.insert(v->tracks.end(), page.items.begin(), page.items.end());
            if (page.total) v->total = page.total;
            if (page.more && !page.items.empty()) {
                loadTracks(v, offset + int(page.items.size()));
            } else {
                v->loading = false;
                v->done = true;
            }
        });
    });
}

void App::reload() {
    loadPlaylists();
    if (!view_) return;
    auto v = view_;
    if (v->kind == Kind::Search) {
        cache_.erase(v->key);
        view_.reset();
        std::string q = v->id;
        runSearch(q);
        return;
    }
    if (v->kind == Kind::Artist) {
        cache_.erase(v->key);
        view_.reset();
        openArtist(v->id, v->title);
        return;
    }
    loadTracks(v, 0);
}

// ---------------------------------------------------------------- playback

void App::playIn(const View& v, int index) {
    if (index < 0 || index >= int(v.tracks.size())) return;
    const Track& t = v.tracks[index];
    if (!t.playable) {
        note("That song can't be played here.");
        return;
    }
    std::string ctx = v.contextUri;
    if (v.kind == Kind::Liked && !meId_.empty()) ctx = "spotify:user:" + meId_ + ":collection";
    if (v.kind == Kind::Search || v.kind == Kind::Artist) ctx.clear();
    // Also a plain list from this song on, in case the context won't take.
    std::vector<std::string> uris;
    for (int i = index; i < int(v.tracks.size()) && uris.size() < 100; i++)
        if (v.tracks[i].playable && !v.tracks[i].uri.empty()) uris.push_back(v.tracks[i].uri);
    if (ctx.empty()) {
        playUris(uris, "");
        return;
    }
    if (!player_.loggedIn) {
        note("Mango's player isn't connected yet: see the card at the top.");
        return;
    }
    auto api = api_;
    auto sol = soloist_;
    auto dc = devices;
    std::string offset = t.uri;
    workers_.Run([api, sol, dc, ctx, offset, uris] {
        std::string err;
        std::string id = findDevice(*api, *sol, *dc, false, err);
        bool ok = !id.empty() && api->Play(id, ctx, offset, {}, err);
        if (!ok && !id.empty()) {
            // A stale device id, or a context Spotify won't start at a song.
            id = findDevice(*api, *sol, *dc, true, err);
            ok = !id.empty() && (api->Play(id, ctx, offset, {}, err) || api->Play(id, "", "", uris, err));
        }
        if (!ok) sol->Play(offset);
    });
}

void App::playUris(std::vector<std::string> uris, std::string offsetUri) {
    if (uris.empty()) return;
    if (!player_.loggedIn) {
        note("Mango's player isn't connected yet: see the card at the top.");
        return;
    }
    auto api = api_;
    auto sol = soloist_;
    auto dc = devices;
    workers_.Run([api, sol, dc, uris, offsetUri] {
        std::string err;
        std::string id = findDevice(*api, *sol, *dc, false, err);
        bool ok = !id.empty() && api->Play(id, "", offsetUri, uris, err);
        if (!ok && !id.empty()) {
            id = findDevice(*api, *sol, *dc, true, err);
            ok = !id.empty() && api->Play(id, "", offsetUri, uris, err);
        }
        if (!ok) sol->Play(offsetUri.empty() ? uris[0] : offsetUri);
    });
}

void App::playContext(const std::string& contextUri, bool shuffle) {
    if (!player_.loggedIn) {
        note("Mango's player isn't connected yet: see the card at the top.");
        return;
    }
    std::string ctx = contextUri;
    soloist_->SetShuffle(shuffle);
    auto api = api_;
    auto sol = soloist_;
    auto dc = devices;
    workers_.Run([api, sol, dc, ctx] {
        std::string err;
        std::string id = findDevice(*api, *sol, *dc, false, err);
        bool ok = !id.empty() && api->Play(id, ctx, "", {}, err);
        if (!ok && !id.empty()) {
            id = findDevice(*api, *sol, *dc, true, err);
            ok = !id.empty() && api->Play(id, ctx, "", {}, err);
        }
        if (!ok) sol->Play(ctx);
    });
}

void App::togglePlay() {
    if (!player_.loggedIn) return;
    if (player_.status == "playing" || player_.status == "buffering") soloist_->Pause();
    else soloist_->Play();
}

void App::MediaKey(int id) {
    if (id == kHotPlay) togglePlay();
    else if (id == kHotNext) soloist_->Next();
    else if (id == kHotPrev) soloist_->Prev();
}

// updateHotkeys takes the keyboard's media keys only while Mango has
// something to play, so other players get them otherwise.
void App::updateHotkeys() {
    bool want = player_.loggedIn && player_.active && !player_.item.uri.empty();
    if (want == hotkeys_) return;
    hotkeys_ = want;
    if (want) {
        RegisterHotKey(hwnd_, kHotPlay, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE);
        RegisterHotKey(hwnd_, kHotNext, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK);
        RegisterHotKey(hwnd_, kHotPrev, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK);
    } else {
        UnregisterHotKey(hwnd_, kHotPlay);
        UnregisterHotKey(hwnd_, kHotNext);
        UnregisterHotKey(hwnd_, kHotPrev);
    }
}

void App::checkLiked(const std::string& uri) {
    likedUri_ = uri;
    likedState_ = -1;
    if (uri.empty() || !uriIs(uri, "track") || !signedIn()) return;
    auto api = api_;
    auto inbox = inbox_;
    workers_.Run([api, inbox, uri, this] {
        std::vector<bool> saved;
        std::string err;
        if (!api->Saved({uri}, saved, err)) return;
        bool yes = !saved.empty() && saved[0];
        inbox->Post([this, uri, yes] {
            if (likedUri_ == uri) likedState_ = yes ? 1 : 0;
        });
    });
}

void App::setLiked(const std::string& uri, bool liked, const std::string& name) {
    auto api = api_;
    auto inbox = inbox_;
    if (likedUri_ == uri) likedState_ = liked ? 1 : 0;
    workers_.Run([api, inbox, uri, liked, name, this] {
        std::string err;
        bool ok = api->SetSaved(uri, liked, err);
        inbox->Post([this, ok, err, uri, liked, name] {
            if (!ok) {
                if (likedUri_ == uri) likedState_ = liked ? 0 : 1;
                note("Couldn't change Liked Songs: " + err);
                return;
            }
            note(liked ? "Added " + name + " to Liked Songs" : "Removed " + name + " from Liked Songs");
            if (auto v = cached("liked")) {
                if (!liked) {
                    v->tracks.erase(std::remove_if(v->tracks.begin(), v->tracks.end(),
                                                   [&](const Track& t) { return t.uri == uri; }),
                                    v->tracks.end());
                    v->filteredN = size_t(-1);
                    if (v->total > 0) v->total--;
                } else {
                    loadTracks(v, 0);
                }
            }
        });
    });
}

// ---------------------------------------------------------------- frame

void App::Pump() {
    std::vector<std::function<void()>> items;
    {
        std::lock_guard<std::mutex> lock(inbox_->mu);
        items.swap(inbox_->items);
    }
    for (auto& f : items) f();
    images_->Upload();

    PlayerState s = demo_ ? player_ : soloist_->State();
    if (!demo_ && s.version != player_.version) {
        std::string before = player_.item.uri;
        bool wasLoggedIn = player_.loggedIn;
        player_ = std::move(s);
        noteText(player_.item.name);
        noteText(player_.item.parentName);
        for (const auto& c : player_.item.creators) noteText(c.second);
        for (const auto& q : player_.upcoming) noteText(q.item.name);
        if (player_.item.uri != before) {
            checkLiked(player_.item.uri);
            if (cfg_.queueOpen && player_.loggedIn) soloist_->AskQueue(50);
        }
        if (!volumeDragging_ && player_.volume >= 0) volumeShown_ = player_.volume;
        if (player_.loggedIn && !wasLoggedIn) {
            // A fresh login: the device id may have changed.
            std::lock_guard<std::mutex> lock(devices->mu);
            devices->id.clear();
        }
    }
    updateHotkeys();
}

bool App::BeforeFrame() {
    int add = wantedScripts & ~loadedScripts;
    if (!add) return false;
    auto merge = [&](const char* file) {
        std::string path = fontPath(file);
        for (ImFont* dst : {regular_, semibold_, bold_}) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.DstFont = dst;
            addFont(path, cfg);
        }
    };
    if (add & Han) merge("msyh.ttc");
    if (add & Hangul) merge("malgun.ttf");
    loadedScripts |= add;
    return true;
}

bool App::Animating() const {
    if (player_.status == "playing" || player_.status == "buffering") return true;
    if (signingIn_ || playlistsLoading_ || (view_ && view_->loading)) return true;
    if (player_.run == PlayerState::Run::Starting) return true;
    return !note_.empty() && ImGui::GetTime() < noteUntil_;
}

bool App::InCaption(float x, float y) const {
    if (x >= barMax_.x || y >= barMax_.y) return false;
    for (const auto& [a, b] : hot_)
        if (x >= a.x && y >= a.y && x < b.x && y < b.y) return false;
    return true;
}

void App::SetScale(float scale) {
    scale_ = scale;
    applyColours();
}

void App::SetDark(bool dark) {
    dark_ = dark;
    pal = dark ? &kDark : &kLight;
    applyColours();
}

void App::applyColours() {
    const Palette& p = *pal;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(6, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(6, 0);
    s.FrameRounding = s.GrabRounding = s.PopupRounding = s.ScrollbarRounding = s.WindowRounding = s.ChildRounding = 0;
    s.WindowBorderSize = s.ChildBorderSize = s.FrameBorderSize = 0;
    s.PopupBorderSize = 1;
    s.ScrollbarSize = 9;
    s.GrabMinSize = 10;
    s.FontSizeBase = kBody;
    s.ScaleAllSizes(scale_);
    s.FontScaleDpi = scale_;
    ImVec4* c = s.Colors;
    ImVec4 none(0, 0, 0, 0);
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.muted;
    c[ImGuiCol_WindowBg] = p.bg;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = p.card;
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_FrameBg] = p.frame;
    c[ImGuiCol_FrameBgHovered] = p.hover;
    c[ImGuiCol_FrameBgActive] = p.hover;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = p.border;
    c[ImGuiCol_ScrollbarGrabHovered] = p.muted;
    c[ImGuiCol_ScrollbarGrabActive] = p.muted;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accent;
    c[ImGuiCol_Button] = p.frame;
    c[ImGuiCol_ButtonHovered] = p.hover;
    c[ImGuiCol_ButtonActive] = p.border;
    c[ImGuiCol_Header] = p.hover;
    c[ImGuiCol_HeaderHovered] = p.hover;
    c[ImGuiCol_HeaderActive] = p.border;
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = none;
    c[ImGuiCol_TableBorderLight] = p.border;
    c[ImGuiCol_TableBorderStrong] = p.border;
    c[ImGuiCol_TableHeaderBg] = none;
    c[ImGuiCol_TextSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f);
    c[ImGuiCol_NavCursor] = none;
    c[ImGuiCol_CheckMark] = p.accent;
}

bool App::iconButton(const char* glyph, const char* tip, bool on, float size, bool danger) {
    const Palette& p = *pal;
    const float k = scale_;
    if (size == 0) size = 30;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, danger ? hex(0xC42B1C) : p.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, danger ? hex(0xC42B1C) : p.border);
    ImGui::PushStyleColor(ImGuiCol_Text, on ? p.accent : p.muted);
    bool pressed = ImGui::Button(glyph, ImVec2(size * k, size * k * 0.87f));
    ImGui::PopStyleColor(4);
    hot_.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    if (tip && *tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    return pressed;
}

bool App::pillButton(const char* label, bool primary) {
    const Palette& p = *pal;
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? p.accent : p.frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? ImVec4(p.accent.x * 1.1f, p.accent.y * 1.1f, p.accent.z * 1.05f, 1) : p.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? p.accent : p.border);
    ImGui::PushStyleColor(ImGuiCol_Text, primary ? p.accentText : p.text);
    ImGui::PushFont(semibold_, kBody);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16 * scale_, 7 * scale_));
    bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return pressed;
}

void App::cover(const std::string& url, float size, const char* fallback) {
    const Palette& p = *pal;
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 b(a.x + size, a.y + size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Texture* t = images_->Get(url);
    if (t && t->width > 0 && t->height > 0) {
        // Centre-crop to a square (artists' photos aren't always square).
        float w = float(t->width), h = float(t->height);
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (w > h) {
            float c = (1 - h / w) / 2;
            uv0.x = c;
            uv1.x = 1 - c;
        } else if (h > w) {
            float c = (1 - w / h) / 2;
            uv0.y = c;
            uv1.y = 1 - c;
        }
        dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)t->srv), a, b, uv0, uv1);
    } else {
        dl->AddRectFilled(a, b, col(p.frame));
        const char* g = fallback ? fallback : icon::Music;
        float fs = std::max(12.0f, size * 0.36f);
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, g);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(a.x + (size - ts.x) / 2, a.y + (size - ts.y) / 2), col(p.faint), g);
    }
    ImGui::Dummy(ImVec2(size, size));
}

bool App::link(const std::string& text, ImVec4 colour, ImFont* font, float px) {
    if (font) ImGui::PushFont(font, px);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::CalcTextSize(text.c_str());
    ImGui::PushID(text.c_str());
    bool clicked = ImGui::InvisibleButton("##link", ImVec2(std::max(1.0f, sz.x), sz.y));
    ImGui::PopID();
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, col(hovered ? pal->text : colour), text.c_str());
    if (hovered) {
        dl->AddLine(ImVec2(pos.x, pos.y + sz.y - 1), ImVec2(pos.x + sz.x, pos.y + sz.y - 1), col(pal->text));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (font) ImGui::PopFont();
    return clicked;
}

void App::artistLinks(const std::vector<Ref>& artists, ImVec4 colour, float px) {
    for (size_t i = 0; i < artists.size(); i++) {
        if (i) {
            ImGui::SameLine(0, 0);
            ImGui::PushFont(regular_, px);
            ImGui::TextColored(colour, ", ");
            ImGui::PopFont();
            ImGui::SameLine(0, 0);
        }
        ImGui::PushID(int(i));
        if (link(artists[i].name, colour, regular_, px) && !artists[i].id.empty())
            openArtist(artists[i].id, artists[i].name);
        ImGui::PopID();
    }
}

// Layout: compact, like kitchensink. One line per song, playlist, album
// and artist; no artwork in lists; a slim header and player.
namespace {
constexpr float kRowH = 26;     // a list row
constexpr float kTitleH = 36;   // the title bar
constexpr float kPlayerH = 46;  // the player along the bottom
}  // namespace

void App::Frame() {
    hot_.clear();
    const Palette& p = *pal;
    ImGuiIO& io = ImGui::GetIO();
    const float k = scale_;

    bool typing = io.WantTextInput;
    if (!typing && ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_L) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K))
        focusSearch_ = true;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) focusFilter_ = true;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_LeftArrow) || ImGui::IsMouseClicked(3)) back();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_RightArrow)) soloist_->Next();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_LeftArrow)) soloist_->Prev();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_UpArrow)) soloist_->SetVolume(volumeShown_ = std::min(100, volumeShown_ + 5));
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_DownArrow)) soloist_->SetVolume(volumeShown_ = std::max(0, volumeShown_ - 5));
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) reload();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("mango", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    drawTitle(io.DisplaySize.x);

    const float top = barMax_.y;
    const float playerH = kPlayerH * k;
    const float sideW = std::clamp(io.DisplaySize.x * 0.2f, 160 * k, 240 * k);
    const float queueW = cfg_.queueOpen ? std::clamp(io.DisplaySize.x * 0.22f, 200 * k, 280 * k) : 0;
    const float bodyH = io.DisplaySize.y - top - playerH;
    drawSidebar(0, top, sideW, bodyH);
    drawMain(sideW, top, io.DisplaySize.x - sideW - queueW, bodyH);
    if (queueW > 0) drawQueue(io.DisplaySize.x - queueW, top, queueW, bodyH);
    drawPlayer(0, top + bodyH, io.DisplaySize.x, playerH);

    if (!note_.empty() && ImGui::GetTime() < noteUntil_) {
        ImGui::PushFont(regular_, kSmall);
        ImVec2 ts = ImGui::CalcTextSize(note_.c_str());
        ImVec2 a(sideW + 10 * k, top + bodyH - ts.y - 18 * k);
        ImVec2 b(a.x + ts.x + 18 * k, a.y + ts.y + 10 * k);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(a, b, col(p.card));
        dl->AddRect(a, b, col(p.border));
        dl->AddText(ImVec2(a.x + 9 * k, a.y + 5 * k), col(p.text), note_.c_str());
        ImGui::PopFont();
    }
    ImGui::End();
}

void App::drawTitle(float width) {
    const Palette& p = *pal;
    const float k = scale_;
    const float h = kTitleH * k;
    barMax_ = ImVec2(width, h);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(width, h), col(p.sidebar));

    ImGui::SetCursorPos(ImVec2(12 * k, 11 * k));
    ImGui::PushFont(bold_, kLabel);
    ImGui::TextColored(p.mango, "MANGO");
    ImGui::PopFont();

    const float sideW = std::clamp(ImGui::GetIO().DisplaySize.x * 0.2f, 160 * k, 240 * k);
    ImGui::SetCursorPos(ImVec2(sideW - 30 * k, 5 * k));
    ImGui::BeginDisabled(backStack_.empty());
    if (iconButton(icon::Back, "Back (Alt+Left)", false, 28)) back();
    ImGui::EndDisabled();

    if (signedIn()) {
        ImGui::SetCursorPos(ImVec2(sideW + 8 * k, 6 * k));
        ImGui::SetNextItemWidth(std::min(340 * k, width - sideW - 120 * k));
        if (focusSearch_) {
            ImGui::SetKeyboardFocusHere();
            focusSearch_ = false;
        }
        ImGui::PushFont(regular_, kSmall + 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * k, 4 * k));
        static const std::string hint = std::string(icon::Search) + "  Search (Ctrl+L)";
        bool enter = ImGui::InputTextWithHint("##search", hint.c_str(), search_, sizeof(search_),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleVar();
        ImGui::PopFont();
        hot_.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
        if (ImGui::IsItemEdited()) searchTyped_ = ImGui::GetTime();
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) search_[0] = 0;
        if (enter) runSearch(search_);
        else if (searchTyped_ > 0 && ImGui::GetTime() - searchTyped_ > 0.6 && searched_ != search_) {
            searchTyped_ = 0;
            if (std::string(search_).size() >= 2) runSearch(search_);
        }
    }

    ImGui::SetCursorPos(ImVec2(width - 3 * 28 * k - 4 * k, 5 * k));
    if (iconButton(icon::Settings, "Settings", false, 28)) ImGui::OpenPopup("##settings");
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Minimise, "Hide (the music keeps playing)", false, 28)) Show(false);
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Close, "Quit Mango (stops the music)", false, 28, true)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);

    if (ImGui::BeginPopup("##settings")) {
        ImGui::PushFont(regular_, kSmall);
        ImGui::Dummy(ImVec2(240 * k, 1 * k));
        ImGui::Indent(6 * k);
        if (signedIn()) ImGui::TextColored(p.muted, "Signed in%s%s", meName_.empty() ? "" : " as ", meName_.c_str());
        ImGui::TextColored(p.muted, "Player: %s",
                           player_.loggedIn ? "connected"
                           : player_.run == PlayerState::Run::Running ? "waiting to be picked once"
                           : player_.run == PlayerState::Run::Failed ? "stopped"
                           : cfg_.soloistKey.empty() ? "needs a Soloist key"
                                                     : "starting");
        ImGui::Unindent(6 * k);
        ImGui::Separator();
        if (ImGui::Selectable("  Reload (F5)")) reload();
        if (ImGui::Selectable("  Restart the player")) startSoloist();
        if (ImGui::Selectable("  Change the Soloist key")) {
            cfg_.soloistKey.clear();
            saveConfig();
            startSoloist();
        }
        if (ImGui::Selectable("  Open the player's log")) {
            const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
            std::wstring log = std::wstring(local ? local : L"") + L"\\mango\\soloist\\soloist.log";
            ShellExecuteW(nullptr, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (signedIn() && ImGui::Selectable("  Sign out of Spotify")) signOut();
        ImGui::PopFont();
        ImGui::EndPopup();
    }
}

// listRow is a clickable one-line row across width; it draws the hover and
// selection backgrounds and returns its rectangle's top-left. The caller
// draws the row's text with the draw list, then the cursor is left below.
static bool listRow(const char* id, float width, float h, bool selected, ImVec2& at, bool* hovered = nullptr,
                    bool* doubleClicked = nullptr, bool* rightClicked = nullptr) {
    const Palette& p = *pal;
    at = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::SetNextItemAllowOverlap();
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(width, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hov = ImGui::IsItemHovered();
    if (rightClicked) *rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    if (doubleClicked) *doubleClicked = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ImGui::PopID();
    if (hovered) *hovered = hov;
    if (selected || hov)
        ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + width, at.y + h), col(selected ? p.hover : p.frame));
    return clicked;
}

void App::drawSidebar(float x, float y, float w, float h) {
    const Palette& p = *pal;
    const float k = scale_;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col(p.sidebar));
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::BeginChild("##side", ImVec2(w, h), ImGuiChildFlags_None);
    if (!signedIn()) {
        ImGui::EndChild();
        return;
    }
    const float rowH = kRowH * k, pad = 4 * k, rw = w - 2 * pad - ImGui::GetStyle().ScrollbarSize * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string& ctx = player_.context.uri;
    auto row = [&](const char* id, const char* glyph, const std::string& name, const std::string& count, bool selected,
                   bool playing) {
        ImVec2 a;
        bool clicked = listRow(id, rw, rowH, selected, a);
        if (selected) dl->AddRectFilled(a, ImVec2(a.x + 2 * k, a.y + rowH), col(p.accent));
        ImGui::PushFont(regular_, kSmall + 0.5f);
        float fy = a.y + (rowH - ImGui::GetFontSize()) / 2;
        float tx = a.x + 10 * k;
        if (glyph) {
            dl->AddText(ImVec2(tx, fy + 1 * k), col(p.accent), glyph);
            tx += 20 * k;
        }
        float cw = ImGui::CalcTextSize(count.c_str()).x;
        float right = a.x + rw - 8 * k;
        if (playing) {
            dl->AddText(ImVec2(right - 14 * k, fy + 1 * k), col(p.accent), icon::Speaker);
            right -= 20 * k;
        } else if (!count.empty()) {
            dl->AddText(ImVec2(right - cw, fy), col(p.faint), count.c_str());
            right -= cw + 8 * k;
        }
        textAt(ImVec2(tx, fy), name, playing ? p.accent : selected ? p.text : p.text, right - tx);
        ImGui::PopFont();
        return clicked;
    };

    ImGui::SetCursorPos(ImVec2(pad, 6 * k));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
    {
        auto lv = cached("liked");
        bool playingLiked = ctx.size() > 11 && ctx.compare(ctx.size() - 11, 11, ":collection") == 0;
        if (row("liked", icon::HeartFill, "Liked Songs", lv && lv->total ? std::to_string(lv->total) : "",
                view_ && view_->key == "liked", playingLiked))
            openLiked();
    }
    ImGui::Dummy(ImVec2(0, 10 * k));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10 * k);
    ImGui::PushFont(semibold_, kLabel);
    ImGui::TextColored(p.muted, "PLAYLISTS");
    ImGui::Dummy(ImVec2(0, 4 * k));
    if (playlistsLoading_) {
        ImGui::SameLine();
        ImGui::TextColored(p.faint, "\xE2\x80\xA6");
    }
    ImGui::PopFont();
    if (!playlistsError_.empty()) {
        ImGui::PushFont(regular_, kSmall);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rw);
        ImGui::TextColored(p.bad, "Couldn't load playlists: %s", playlistsError_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    for (const auto& pl : playlists_) {
        bool sel = view_ && view_->key == "playlist:" + pl.id;
        bool mine = pl.ownerId.empty() || pl.ownerId == meId_ || pl.collaborative;
        if (row(pl.id.c_str(), nullptr, pl.name, mine ? std::to_string(pl.total) : "", sel, !ctx.empty() && ctx == pl.uri))
            openPlaylist(pl);
        if (!mine && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("by %s", pl.owner.c_str());
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, 6 * k));
    ImGui::EndGroup();
    ImGui::EndChild();
}

void App::drawMain(float x, float y, float w, float h) {
    const float k = scale_;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::BeginChild("##main", ImVec2(w, h), ImGuiChildFlags_None);
    const float pad = 12 * k;
    const float inner = w - 2 * pad - ImGui::GetStyle().ScrollbarSize;
    if (view_ && view_->restoreScroll) {
        ImGui::SetScrollY(view_->scroll);
        view_->restoreScroll = false;
    }
    ImGui::SetCursorPos(ImVec2(pad, 10 * k));
    ImGui::BeginGroup();
    drawSetup(inner);
    if (signedIn() && view_) {
        View& v = *view_;
        if (v.kind == Kind::Search) {
            drawSearch(v, inner);
        } else {
            drawHeader(v, inner);
            if (v.kind == Kind::Artist) {
                if (!v.error.empty()) ImGui::TextColored(pal->bad, "Couldn't load: %s", v.error.c_str());
                else if (v.loading) ImGui::TextColored(pal->muted, "Loading\xE2\x80\xA6");
                else drawCards(&v.albums, nullptr, nullptr, inner, "disc");
            } else {
                drawTracks(v, inner, true, v.kind != Kind::Album);
            }
        }
    } else if (signedIn()) {
        ImGui::TextColored(pal->muted, "Pick something on the left, or search.");
    }
    ImGui::Dummy(ImVec2(0, 12 * k));
    ImGui::EndGroup();
    if (view_) view_->scroll = ImGui::GetScrollY();
    ImGui::EndChild();
}

// drawHeader is one line: the name and a few facts, then Play, Shuffle and
// the filter on the right.
void App::drawHeader(View& v, float width) {
    const Palette& p = *pal;
    const float k = scale_;
    const float h = 30 * k;
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Right-hand controls first, so the text knows how much room it has.
    std::string ctx = v.kind == Kind::Liked && !meId_.empty() ? "spotify:user:" + meId_ + ":collection" : v.contextUri;
    bool canPlay = !v.contextUri.empty() || (v.kind == Kind::Liked && !v.tracks.empty());
    bool playingThis = !ctx.empty() && player_.context.uri == ctx;
    bool isPlaying = playingThis && (player_.status == "playing" || player_.status == "buffering");
    const float fw = (v.kind != Kind::Artist && !v.notListed) ? 170 * k : 0;
    const float bw = 28 * k;
    float rx = a.x + width - fw - (fw > 0 ? 8 * k : 0) - 2 * bw;

    ImGui::SetCursorScreenPos(ImVec2(rx, a.y + 1 * k));
    ImGui::BeginDisabled(!canPlay || !player_.loggedIn);
    if (iconButton(isPlaying ? icon::Pause : icon::Play, isPlaying ? "Pause" : "Play", true, 28)) {
        if (playingThis) togglePlay();
        else if (v.kind == Kind::Liked && !v.tracks.empty()) playIn(v, 0);
        else playContext(ctx, false);
    }
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Shuffle, "Shuffle play", false, 28)) {
        if (v.kind == Kind::Liked && !v.tracks.empty()) {
            soloist_->SetShuffle(true);
            playIn(v, int(GetTickCount64() % v.tracks.size()));
        } else {
            playContext(ctx, true);
        }
    }
    ImGui::EndDisabled();
    if (fw > 0) {
        ImGui::SameLine(0, 8 * k);
        ImGui::SetNextItemWidth(fw);
        if (focusFilter_) {
            ImGui::SetKeyboardFocusHere();
            focusFilter_ = false;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", v.filter.c_str());
        ImGui::PushFont(regular_, kSmall + 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * k, 4 * k));
        static const std::string hint = std::string(icon::Filter) + "  Filter (Ctrl+F)";
        if (ImGui::InputTextWithHint("##filter", hint.c_str(), buf, sizeof(buf))) v.filter = buf;
        ImGui::PopStyleVar();
        ImGui::PopFont();
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) v.filter.clear();
    }

    // Name, then the facts in muted text.
    float tx = a.x, room = rx - tx - 12 * k;
    ImGui::PushFont(semibold_, kTitle);
    std::string title = ellipsize(v.title.empty() ? "\xE2\x80\xA6" : v.title, room * 0.7f);
    float ty = a.y + (h - ImGui::GetFontSize()) / 2;
    dl->AddText(ImVec2(tx, ty), col(p.text), title.c_str());
    tx += ImGui::CalcTextSize(title.c_str()).x + 12 * k;
    float base = ty + ImGui::GetFontSize();
    ImGui::PopFont();

    ImGui::PushFont(regular_, kSmall);
    float sy = base - ImGui::GetFontSize() - 1 * k;
    std::string facts;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        facts += (facts.empty() ? "" : "  \xC2\xB7  ") + s;
    };
    if (!v.artists.empty()) {
        // Album artists, clickable.
        ImGui::SetCursorScreenPos(ImVec2(tx, sy));
        ImGui::PushClipRect(ImVec2(tx, a.y), ImVec2(rx - 8 * k, a.y + h), true);
        artistLinks(v.artists, p.muted, kSmall);
        ImGui::PopClipRect();
        tx = ImGui::GetItemRectMax().x + 10 * k;
    }
    add(v.kindLabel == "Playlist" || v.kindLabel == "Artist" ? "" : v.kindLabel);
    add(v.byline);
    if (v.kind != Kind::Artist) {
        int n = v.total ? v.total : int(v.tracks.size());
        if (n) {
            int64_t ms = 0;
            for (const auto& t : v.tracks) ms += t.durationMs;
            add(songs(n) + (v.done && !v.tracks.empty() ? ", " + LongDuration(ms) : ""));
        }
    } else if (!v.albums.empty()) {
        add(std::to_string(v.albums.size()) + " releases");
    }
    textAt(ImVec2(tx, sy), facts, p.muted, rx - 8 * k - tx);
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + 6 * k));
    ImGui::Dummy(ImVec2(width, 0));
}

void App::drawTracks(View& v, float width, bool numbered, bool showAlbum) {
    const Palette& p = *pal;
    const float k = scale_;
    if (v.notListed) {
        ImGui::PushFont(regular_, kSmall + 0.5f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextColored(p.muted, "Spotify only lets apps list the songs of playlists you made or collaborate on, "
                                    "so this one can't be shown. Play and Shuffle still work.");
        ImGui::PopTextWrapPos();
        if (player_.context.uri == v.contextUri && !player_.upcoming.empty()) {
            ImGui::Dummy(ImVec2(0, 4 * k));
            ImGui::TextColored(p.muted, "Up next from it:");
            for (const auto& q : player_.upcoming) {
                std::string who;
                for (const auto& c : q.item.creators) who += (who.empty() ? "" : ", ") + c.second;
                ImGui::TextUnformatted(ellipsize(q.item.name + (who.empty() ? "" : "  \xC2\xB7  " + who), width).c_str());
            }
        }
        ImGui::PopFont();
        return;
    }
    if (!v.error.empty()) {
        ImGui::TextColored(p.bad, "Couldn't load: %s", v.error.c_str());
        if (pillButton("Try again", false)) loadTracks(view_, 0);
        return;
    }
    if (v.tracks.empty()) {
        ImGui::TextColored(p.muted, v.loading ? "Loading\xE2\x80\xA6" : "Nothing here yet.");
        return;
    }
    if (v.filteredFor != v.filter || v.filteredN != v.tracks.size()) {
        v.filteredFor = v.filter;
        v.filteredN = v.tracks.size();
        v.shown.clear();
        std::string f = Lower(v.filter);
        for (int i = 0; i < int(v.tracks.size()); i++) {
            const Track& t = v.tracks[i];
            if (!f.empty()) {
                std::string hay = Lower(t.name + " " + joinNames(t.artists) + " " + t.album.name);
                if (hay.find(f) == std::string::npos) continue;
            }
            v.shown.push_back(i);
        }
    }

    // Columns: # | title | artist | album | time. Album goes on narrow windows.
    const float rowH = kRowH * k;
    const float numW = numbered ? 36 * k : 6 * k, timeW = 50 * k;
    if (width < 620 * k) showAlbum = false;
    const float rest = width - numW - timeW;
    const float titleW = rest * (showAlbum ? 0.42f : 0.58f);
    const float artistW = rest * (showAlbum ? 0.29f : 0.42f);
    const float albumW = showAlbum ? rest - titleW - artistW : 0;
    const float xTitle = numW, xArtist = numW + titleW, xAlbum = xArtist + artistW;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::PushFont(semibold_, kLabel);
    ImVec2 h0 = ImGui::GetCursorScreenPos();
    float hy = h0.y + 2 * k;
    if (numbered) dl->AddText(ImVec2(h0.x + 10 * k, hy), col(p.muted), "#");
    dl->AddText(ImVec2(h0.x + xTitle, hy), col(p.muted), "TITLE");
    dl->AddText(ImVec2(h0.x + xArtist, hy), col(p.muted), "ARTIST");
    if (showAlbum) dl->AddText(ImVec2(h0.x + xAlbum, hy), col(p.muted), "ALBUM");
    ImVec2 ts = ImGui::CalcTextSize("TIME");
    dl->AddText(ImVec2(h0.x + width - ts.x - 8 * k, hy), col(p.muted), "TIME");
    float lh = ImGui::GetFontSize();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(width, lh + 6 * k));
    ImVec2 lineA = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(lineA.x, lineA.y - 2 * k), ImVec2(lineA.x + width, lineA.y - 2 * k), col(p.border));

    if (v.shown.empty()) {
        ImGui::TextColored(p.muted, "No songs match \"%s\".", v.filter.c_str());
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
    ImGuiListClipper clipper;
    clipper.Begin(int(v.shown.size()), rowH);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
            int i = v.shown[r];
            const Track& t = v.tracks[i];
            ImGui::PushID(i);
            bool playing = !t.uri.empty() && t.uri == player_.item.uri;
            ImVec2 a;
            bool hovered = false, dbl = false, right = false;
            bool clicked = listRow("r", width, rowH, v.selected == i, a, &hovered, &dbl, &right);
            if (clicked) v.selected = i;
            if (dbl) playIn(v, i);
            if (right) {
                v.selected = i;
                ImGui::OpenPopup("##track");
            }
            ImVec2 after = ImGui::GetCursorScreenPos();
            ImGui::PushFont(regular_, kSmall + 0.5f);
            float fy = a.y + (rowH - ImGui::GetFontSize()) / 2;
            if (numbered) {
                if (hovered) {
                    ImGui::SetCursorScreenPos(ImVec2(a.x + 2 * k, a.y + (rowH - 22 * k) / 2));
                    if (iconButton(playing && player_.status == "playing" ? icon::Pause : icon::Play, nullptr, false, 25)) {
                        if (playing) togglePlay();
                        else playIn(v, i);
                    }
                } else if (playing) {
                    dl->AddText(ImVec2(a.x + 10 * k, fy + 1 * k), col(p.accent), icon::Speaker);
                } else {
                    std::string n = std::to_string(i + 1);
                    dl->AddText(ImVec2(a.x + 10 * k, fy), col(p.faint), n.c_str());
                }
            }
            float nameW = titleW - 12 * k;
            float nx = a.x + xTitle;
            if (t.explicit_) {
                // A small boxed E before the name.
                ImGui::PushFont(bold_, kLabel - 2);
                ImVec2 es = ImGui::CalcTextSize("E");
                float bw = es.x + 6 * k, bh = es.y + 1 * k, by = a.y + (rowH - bh) / 2;
                dl->AddRectFilled(ImVec2(nx, by), ImVec2(nx + bw, by + bh), col(p.muted));
                dl->AddText(ImVec2(nx + 3 * k, by + 0.5f * k), col(p.bg), "E");
                ImGui::PopFont();
                nx += bw + 6 * k;
                nameW -= bw + 6 * k;
            }
            ImGui::PushFont(playing ? semibold_ : regular_, kSmall + 0.5f);
            textAt(ImVec2(nx, fy), t.name, playing ? p.accent : t.playable ? p.text : p.faint, nameW);
            ImGui::PopFont();
            // Artists and album as links, cut at their columns.
            ImGui::SetCursorScreenPos(ImVec2(a.x + xArtist, fy));
            ImGui::PushClipRect(ImVec2(a.x + xArtist, a.y), ImVec2(a.x + xArtist + artistW - 12 * k, a.y + rowH), true);
            artistLinks(t.artists, p.muted, kSmall + 0.5f);
            ImGui::PopClipRect();
            if (showAlbum && !t.album.name.empty()) {
                ImGui::SetCursorScreenPos(ImVec2(a.x + xAlbum, fy));
                if (link(ellipsize(t.album.name, albumW - 12 * k), p.muted) && !t.album.id.empty())
                    openAlbum(t.album.id, t.album.name);
            }
            std::string d = Duration(t.durationMs);
            float dw = ImGui::CalcTextSize(d.c_str()).x;
            dl->AddText(ImVec2(a.x + width - dw - 8 * k, fy), col(p.muted), d.c_str());
            ImGui::PopFont();

            if (ImGui::BeginPopup("##track")) {
                trackMenu(t, &v);
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(after);
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && v.selected >= 0) playIn(v, v.selected);
    if (v.loading) {
        ImGui::PushFont(regular_, kSmall);
        ImGui::TextColored(p.muted, "Loading more\xE2\x80\xA6 (%d of %d)", int(v.tracks.size()), v.total);
        ImGui::PopFont();
    }
}

// drawCards lists albums, artists or playlists one a line (the name was
// kept from when they were cards): name | detail | year or kind. A click
// opens one; the play button on hover plays it.
void App::drawCards(const std::vector<Album>* albums, const std::vector<Artist>* artists,
                    const std::vector<Playlist>* playlists, float width, const char* id) {
    const Palette& p = *pal;
    const float k = scale_;
    const float rowH = kRowH * k;
    int n = albums ? int(albums->size()) : artists ? int(artists->size()) : int(playlists->size());
    bool searchList = std::string(id) != "disc";
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
    for (int i = 0; i < n; i++) {
        std::string name, detail, right, uri;
        if (albums) {
            const Album& a = (*albums)[i];
            name = a.name;
            detail = searchList ? joinNames(a.artists) : albumKind(a.type);
            right = a.year;
            uri = a.uri;
        } else if (artists) {
            name = (*artists)[i].name;
            for (size_t g = 0; g < (*artists)[i].genres.size() && g < 2; g++)
                detail += (g ? ", " : "") + (*artists)[i].genres[g];
            uri = (*artists)[i].uri;
        } else {
            const Playlist& pl = (*playlists)[i];
            name = pl.name;
            detail = "by " + pl.owner;
            right = pl.total ? std::to_string(pl.total) : "";
            uri = pl.uri;
        }
        ImGui::PushID(i);
        ImVec2 a;
        bool hovered = false;
        bool clicked = listRow("c", width, rowH, false, a, &hovered);
        ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::PushFont(regular_, kSmall + 0.5f);
        float fy = a.y + (rowH - ImGui::GetFontSize()) / 2;
        bool played = false;
        if (hovered) {
            ImGui::SetCursorScreenPos(ImVec2(a.x + 2 * k, a.y + (rowH - 22 * k) / 2));
            if (iconButton(icon::Play, "Play", false, 25)) {
                playContext(uri, false);
                played = true;
            }
        } else if (!uri.empty() && uri == player_.context.uri) {
            dl->AddText(ImVec2(a.x + 10 * k, fy + 1 * k), col(p.accent), icon::Speaker);
        }
        float x0 = a.x + 36 * k, nameW = (width - 36 * k - 60 * k) * 0.55f;
        textAt(ImVec2(x0, fy), name, p.text, nameW - 12 * k);
        textAt(ImVec2(x0 + nameW, fy), detail, p.muted, width - 36 * k - 60 * k - nameW - 12 * k);
        float rw = ImGui::CalcTextSize(right.c_str()).x;
        dl->AddText(ImVec2(a.x + width - rw - 8 * k, fy), col(p.muted), right.c_str());
        ImGui::PopFont();
        if (clicked && !played && !ImGui::IsItemHovered()) {
            if (albums) openAlbum((*albums)[i].id, (*albums)[i].name);
            else if (artists) openArtist((*artists)[i].id, (*artists)[i].name);
            else openPlaylist((*playlists)[i]);
        }
        ImGui::SetCursorScreenPos(after);
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 6 * k));
}

void App::drawSearch(View& v, float width) {
    const Palette& p = *pal;
    const float k = scale_;
    if (!v.error.empty()) {
        ImGui::TextColored(p.bad, "Search failed: %s", v.error.c_str());
        return;
    }
    if (!v.done) {
        ImGui::TextColored(p.muted, "Searching for \"%s\"\xE2\x80\xA6", v.title.c_str());
        return;
    }
    auto section = [&](const char* title, bool more, const char* type) {
        ImGui::PushFont(semibold_, kLabel);
        ImGui::TextColored(p.muted, "%s", title);
        ImGui::PopFont();
        if (more) {
            ImGui::SameLine(0, 12 * k);
            ImGui::PushFont(regular_, kSmall);
            ImGui::PushID(type);
            if (link(v.loading ? "loading\xE2\x80\xA6" : "more", p.accent)) moreSearch(view_, type);
            ImGui::PopID();
            ImGui::PopFont();
        }
    };
    if (!v.tracks.empty()) {
        section("SONGS", v.search.tracks.more, "track");
        std::string keep = v.filter;
        v.filter.clear();
        drawTracks(v, width, false, true);
        v.filter = keep;
        ImGui::Dummy(ImVec2(0, 8 * k));
    }
    if (!v.search.artists.items.empty()) {
        section("ARTISTS", v.search.artists.more, "artist");
        drawCards(nullptr, &v.search.artists.items, nullptr, width, "artists");
    }
    if (!v.search.albums.items.empty()) {
        section("ALBUMS", v.search.albums.more, "album");
        drawCards(&v.search.albums.items, nullptr, nullptr, width, "search");
    }
    if (!v.search.playlists.items.empty()) {
        section("PLAYLISTS", v.search.playlists.more, "playlist");
        drawCards(nullptr, nullptr, &v.search.playlists.items, width, "playlists");
    }
    if (v.tracks.empty() && v.search.artists.items.empty() && v.search.albums.items.empty() &&
        v.search.playlists.items.empty())
        ImGui::TextColored(p.muted, "Nothing found for \"%s\".", v.title.c_str());
}

void App::drawQueue(float x, float y, float w, float h) {
    const Palette& p = *pal;
    const float k = scale_;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col(p.sidebar));
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::BeginChild("##queue", ImVec2(w, h), ImGuiChildFlags_None);
    ImGui::SetCursorPos(ImVec2(10 * k, 10 * k));
    ImGui::BeginGroup();
    ImGui::PushFont(semibold_, kLabel);
    ImGui::TextColored(p.muted, "UP NEXT");
    ImGui::PopFont();
    ImGui::PushFont(regular_, kSmall + 0.5f);
    if (player_.upcoming.empty())
        ImGui::TextColored(p.muted, player_.loggedIn ? "Nothing queued." : "The player isn't connected.");
    const float rowH = kRowH * k, tw = w - 20 * k - ImGui::GetStyle().ScrollbarSize;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    (void)dl;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
    for (size_t i = 0; i < player_.upcoming.size(); i++) {
        const auto& q = player_.upcoming[i];
        ImVec2 a = ImGui::GetCursorScreenPos();
        std::string who;
        for (const auto& c : q.item.creators) who += (who.empty() ? "" : ", ") + c.second;
        float fy = a.y + (rowH - ImGui::GetFontSize()) / 2;
        float nw = ImGui::CalcTextSize(ellipsize(q.item.name, tw * 0.6f).c_str()).x;
        textAt(ImVec2(a.x, fy), q.item.name, q.source == "queue" ? p.accent : p.text, tw * 0.6f);
        textAt(ImVec2(a.x + nw + 8 * k, fy), who, p.muted, tw - nw - 8 * k);
        ImGui::Dummy(ImVec2(tw, rowH));
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::EndChild();
}

// drawPlayer is one slim strip: controls, what's playing, the seek bar,
// then shuffle, repeat, like, queue and volume.
void App::drawPlayer(float x, float y, float w, float h) {
    const Palette& p = *pal;
    const float k = scale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col(p.card));
    dl->AddLine(ImVec2(x, y), ImVec2(x + w, y), col(p.border));
    const Entity& it = player_.item;
    const bool live = player_.loggedIn && !it.uri.empty();
    const bool isPlaying = player_.status == "playing" || player_.status == "buffering";
    const float bs = 30;  // button size, unscaled
    const float by = y + (h - bs * k * 0.87f) / 2;

    // Controls.
    ImGui::BeginDisabled(!player_.loggedIn);
    ImGui::SetCursorScreenPos(ImVec2(x + 6 * k, by));
    ImGui::PushFont(regular_, kBody);
    if (iconButton(icon::Prev, "Previous (Ctrl+Left)", false, bs)) soloist_->Prev();
    ImGui::SameLine(0, 0);
    if (iconButton(isPlaying ? icon::Pause : icon::Play, isPlaying ? "Pause (Space)" : "Play (Space)", true, bs)) togglePlay();
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Next, "Next (Ctrl+Right)", false, bs)) soloist_->Next();
    ImGui::PopFont();
    ImGui::EndDisabled();
    float cx = ImGui::GetItemRectMax().x + 10 * k;

    // What's playing: small cover, title over artists.
    const float infoW = std::clamp(w * 0.28f, 170 * k, 320 * k);
    float cs = h - 14 * k;
    ImGui::SetCursorScreenPos(ImVec2(cx, y + 7 * k));
    cover(live ? it.image : "", cs);
    float tx = cx + cs + 8 * k, tw = infoW - cs - 8 * k;
    if (live) {
        ImGui::SetCursorScreenPos(ImVec2(tx, y + 6 * k));
        if (link(ellipsize(it.name, tw), p.text, semibold_, kSmall + 0.5f) && uriIs(it.parentUri, "album"))
            openAlbum(idOf(it.parentUri), it.parentName);
        ImGui::SetCursorScreenPos(ImVec2(tx, y + 24 * k));
        std::vector<Ref> artists;
        for (const auto& c : it.creators) artists.push_back({idOf(c.first), c.first, c.second});
        ImGui::PushClipRect(ImVec2(tx, y), ImVec2(tx + tw, y + h), true);
        artistLinks(artists, p.muted, kSmall);
        ImGui::PopClipRect();
    } else {
        ImGui::PushFont(regular_, kSmall);
        const char* why = !signedIn() ? "Not signed in"
                        : cfg_.soloistKey.empty() ? "The player needs setting up"
                        : player_.run == PlayerState::Run::Failed ? "The player stopped"
                        : !player_.loggedIn ? "Waiting for the player\xE2\x80\xA6"
                                            : "Nothing playing";
        textAt(ImVec2(tx, y + (h - ImGui::GetFontSize()) / 2), why, p.muted, tw);
        ImGui::PopFont();
    }

    // Right side: shuffle, repeat, like, queue, volume.
    const float vw = 80 * k;
    const float rightW = 5 * bs * k + vw + 16 * k;
    float rx = x + w - rightW;
    ImGui::SetCursorScreenPos(ImVec2(rx, by));
    ImGui::BeginDisabled(!player_.loggedIn);
    if (iconButton(icon::Shuffle, player_.shuffle ? "Shuffle is on" : "Shuffle", player_.shuffle, bs))
        soloist_->SetShuffle(!player_.shuffle);
    ImGui::SameLine(0, 0);
    const char* rg = player_.repeat == "track" ? icon::RepeatOne : icon::Repeat;
    const char* rt = player_.repeat == "track" ? "Repeating this song" : player_.repeat == "context" ? "Repeating" : "Repeat";
    if (iconButton(rg, rt, player_.repeat != "off", bs))
        soloist_->SetRepeat(player_.repeat == "off" ? "context" : player_.repeat == "context" ? "track" : "off");
    ImGui::EndDisabled();
    ImGui::SameLine(0, 0);
    {
        bool liked = likedState_ == 1;
        bool can = live && uriIs(it.uri, "track") && likedState_ >= 0;
        ImGui::BeginDisabled(!can);
        if (iconButton(liked ? icon::HeartFill : icon::Heart, liked ? "Remove from Liked Songs" : "Add to Liked Songs", liked, bs))
            setLiked(it.uri, !liked, it.name);
        ImGui::EndDisabled();
    }
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Queue, "Up next", cfg_.queueOpen, bs)) {
        cfg_.queueOpen = !cfg_.queueOpen;
        saveConfig();
        if (cfg_.queueOpen && player_.loggedIn) soloist_->AskQueue(50);
    }
    ImGui::SameLine(0, 0);
    static int beforeMute = 40;
    if (iconButton(volumeShown_ == 0 ? icon::Mute : icon::Volume, volumeShown_ == 0 ? "Unmute" : "Mute", false, bs)) {
        if (volumeShown_ > 0) {
            beforeMute = volumeShown_;
            volumeShown_ = 0;
        } else {
            volumeShown_ = beforeMute > 0 ? beforeMute : 40;
        }
        soloist_->SetVolume(volumeShown_);
        cfg_.volume = volumeShown_;
        saveConfig();
    }
    {
        float vx = x + w - vw - 10 * k, vy = y + h / 2;
        ImGui::SetCursorScreenPos(ImVec2(vx, vy - 8 * k));
        ImGui::InvisibleButton("##vol", ImVec2(vw, 16 * k));
        bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
        static double lastSent = 0;
        if (ImGui::IsItemActive()) {
            volumeDragging_ = true;
            int v = int(std::round(std::clamp((ImGui::GetIO().MousePos.x - vx) / vw, 0.0f, 1.0f) * 100));
            if (v != volumeShown_ && ImGui::GetTime() - lastSent > 0.08) {
                soloist_->SetVolume(v);
                lastSent = ImGui::GetTime();
            }
            volumeShown_ = v;
        } else if (volumeDragging_) {
            volumeDragging_ = false;
            soloist_->SetVolume(volumeShown_);
            cfg_.volume = volumeShown_;
            saveConfig();
        }
        if (hov && ImGui::GetIO().MouseWheel != 0) {
            volumeShown_ = std::clamp(volumeShown_ + int(ImGui::GetIO().MouseWheel * 5), 0, 100);
            soloist_->SetVolume(volumeShown_);
            cfg_.volume = volumeShown_;
        }
        float th = hov ? 4 * k : 3 * k, frac = volumeShown_ / 100.0f;
        dl->AddRectFilled(ImVec2(vx, vy - th / 2), ImVec2(vx + vw, vy + th / 2), col(p.border));
        dl->AddRectFilled(ImVec2(vx, vy - th / 2), ImVec2(vx + vw * frac, vy + th / 2), col(hov ? p.accent : p.muted));
        if (hov) {
            dl->AddCircleFilled(ImVec2(vx + vw * frac, vy), 5 * k, col(p.text));
            ImGui::SetTooltip("Volume %d%%", volumeShown_);
        }
    }

    // The seek bar fills the middle, times either side.
    {
        float sx0 = tx + tw + 14 * k, sx1 = rx - 12 * k;
        int64_t dur = it.durationMs;
        int64_t pos = player_.PositionNow(NowMs());
        ImGui::PushFont(regular_, kSmall - 1);
        float tmw = ImGui::CalcTextSize("00:00").x;
        float bx0 = sx0 + tmw + 8 * k, bx1 = sx1 - tmw - 8 * k;
        if (bx1 - bx0 > 40 * k) {
            float sy = y + h / 2;
            std::string ps = live ? Duration(seeking_ ? int64_t(seekFrac_ * dur) : pos) : "", ds = live ? Duration(dur) : "";
            ImVec2 pss = ImGui::CalcTextSize(ps.c_str());
            dl->AddText(ImVec2(bx0 - 6 * k - pss.x, sy - pss.y / 2), col(p.muted), ps.c_str());
            dl->AddText(ImVec2(bx1 + 6 * k, sy - pss.y / 2), col(p.muted), ds.c_str());
            ImGui::SetCursorScreenPos(ImVec2(bx0, sy - 8 * k));
            ImGui::InvisibleButton("##seek", ImVec2(bx1 - bx0, 16 * k));
            bool hov = live && (ImGui::IsItemHovered() || ImGui::IsItemActive());
            if (ImGui::IsItemActive() && dur > 0 && live) {
                seeking_ = true;
                seekFrac_ = std::clamp((ImGui::GetIO().MousePos.x - bx0) / (bx1 - bx0), 0.0f, 1.0f);
            } else if (seeking_) {
                seeking_ = false;
                soloist_->Seek(int64_t(seekFrac_ * dur));
            }
            float frac = !live ? 0 : seeking_ ? seekFrac_ : dur > 0 ? float(double(pos) / double(dur)) : 0;
            float th = hov ? 4 * k : 3 * k;
            dl->AddRectFilled(ImVec2(bx0, sy - th / 2), ImVec2(bx1, sy + th / 2), col(p.border));
            dl->AddRectFilled(ImVec2(bx0, sy - th / 2), ImVec2(bx0 + (bx1 - bx0) * frac, sy + th / 2),
                              col(hov ? p.accent : p.muted));
            if (hov) dl->AddCircleFilled(ImVec2(bx0 + (bx1 - bx0) * frac, sy), 5 * k, col(p.text));
        }
        ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::Dummy(ImVec2(0, 0));
}

void App::drawSetup(float width) {
    const Palette& p = *pal;
    const float k = scale_;
    auto card = [&](const char* title, auto body) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImDrawListSplitter split;
        split.Split(dl, 2);
        split.SetCurrentChannel(dl, 1);
        ImVec2 top = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(top.x + 14 * k, top.y + 10 * k));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(top.x + width - 14 * k);
        ImGui::PushFont(semibold_, kTitle);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::PushFont(regular_, kSmall + 0.5f);
        body();
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImVec2 end = ImGui::GetItemRectMax();
        ImVec2 bottom(top.x + width, end.y + 12 * k);
        split.SetCurrentChannel(dl, 0);
        dl->AddRectFilled(top, bottom, col(p.card));
        dl->AddRectFilled(top, ImVec2(top.x + 3 * k, bottom.y), col(p.mango));
        split.Merge(dl);
        ImGui::SetCursorScreenPos(ImVec2(top.x, bottom.y + 12 * k));
        ImGui::Dummy(ImVec2(width, 0));
    };

    if (!signedIn()) {
        card("Sign in with Spotify", [&] {
            ImGui::TextColored(p.muted, "Mango opens Spotify's sign-in page in your browser. Once you agree, it comes "
                                        "back here and remembers you.");
            ImGui::Dummy(ImVec2(0, 4 * k));
            if (signingIn_) {
                ImGui::TextColored(p.muted, "Waiting for the browser\xE2\x80\xA6");
                ImGui::SameLine();
                if (pillButton("Cancel", false) && cancelSignIn_) *cancelSignIn_ = true;
            } else if (pillButton("Sign in", true)) {
                signIn();
            }
            if (!signInError_.empty()) ImGui::TextColored(p.bad, "%s", signInError_.c_str());
        });
        return;
    }
    if (cfg_.soloistKey.empty()) {
        card("Set up the player", [&] {
            ImGui::TextColored(p.muted, "Mango plays through Spotify Soloist, Spotify's own player, which it runs "
                                        "hidden. Soloist needs an API key from your Spotify developer dashboard "
                                        "(Soloist API Key page). Paste it here; it's kept encrypted for your "
                                        "Windows account.");
            ImGui::Dummy(ImVec2(0, 4 * k));
            ImGui::SetNextItemWidth(std::min(420 * k, width - 160 * k));
            bool enter = ImGui::InputTextWithHint("##key", "Soloist API key", keyInput_, sizeof(keyInput_),
                                                  ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((pillButton("Save", true) || enter) && keyInput_[0]) {
                std::string key = keyInput_;
                key.erase(std::remove_if(key.begin(), key.end(), [](char c) { return c == ' ' || c == '\r' || c == '\n'; }),
                          key.end());
                cfg_.soloistKey = Protect(key);
                SecureZeroMemory(keyInput_, sizeof(keyInput_));
                saveConfig();
                startSoloist();
            }
            ImGui::Dummy(ImVec2(0, 2 * k));
            if (link("Open the developer dashboard", p.accent))
                ShellExecuteW(nullptr, L"open", L"https://developer.spotify.com/dashboard", nullptr, nullptr, SW_SHOWNORMAL);
        });
        return;
    }
    using Run = PlayerState::Run;
    if (player_.run == Run::Failed) {
        card("The player stopped", [&] {
            ImGui::TextColored(p.muted, "%s", player_.runError.c_str());
            ImGui::Dummy(ImVec2(0, 4 * k));
            if (pillButton("Try again", true)) startSoloist();
            ImGui::SameLine();
            if (pillButton("Change the key", false)) {
                cfg_.soloistKey.clear();
                saveConfig();
                startSoloist();
            }
        });
    } else if (player_.run == Run::Starting && !player_.runError.empty()) {
        card("Getting the player", [&] { ImGui::TextColored(p.muted, "%s", player_.runError.c_str()); });
    } else if (player_.run == Run::Running && player_.connected && !player_.loggedIn) {
        card("One more step: pick Mango once", [&] {
            ImGui::TextColored(p.muted, "Open Spotify on your phone (on the same Wi-Fi), tap the devices button and "
                                        "choose \"%s\". Soloist signs in through that and remembers it, so this is "
                                        "only needed once. You can close Spotify on the phone afterwards.",
                               player_.deviceName.empty() ? kDeviceName : player_.deviceName.c_str());
            ImGui::Dummy(ImVec2(0, 2 * k));
            ImGui::TextColored(p.faint, "If Windows asks whether soloist.exe may use the network, allow private "
                                        "networks: that's how the phone finds it.");
        });
    }
}

void App::trackMenu(const Track& t, const View* in) {
    const float k = scale_;
    ImGui::PushFont(regular_, kSmall + 0.5f);
    ImGui::Dummy(ImVec2(200 * k, 0));
    if (ImGui::Selectable("  Play") && in) {
        for (int i = 0; i < int(in->tracks.size()); i++)
            if (&in->tracks[i] == &t) playIn(*in, i);
    }
    if (ImGui::Selectable("  Add to queue")) {
        soloist_->Queue(t.uri);
        note("Added " + t.name + " to the queue");
    }
    ImGui::Separator();
    if (!t.album.id.empty() && ImGui::Selectable("  Go to the album")) openAlbum(t.album.id, t.album.name);
    for (const auto& a : t.artists) {
        if (a.id.empty()) continue;
        std::string label = "  Go to " + a.name;
        if (ImGui::Selectable(label.c_str())) openArtist(a.id, a.name);
    }
    ImGui::Separator();
    if (in && in->kind == Kind::Liked) {
        if (ImGui::Selectable("  Remove from Liked Songs")) setLiked(t.uri, false, t.name);
    } else if (ImGui::Selectable("  Add to Liked Songs")) {
        setLiked(t.uri, true, t.name);
    }
    if (ImGui::Selectable("  Copy the song's link")) {
        ImGui::SetClipboardText(("https://open.spotify.com/track/" + t.id).c_str());
        note("Link copied");
    }
    ImGui::PopFont();
}

}  // namespace mango
