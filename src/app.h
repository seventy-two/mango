// The window's contents: playlists down the left, the open playlist,
// album, artist or search in the middle, and what's playing along the
// bottom. Drawn with Dear ImGui each frame.
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "images.h"
#include "media.h"
#include "imgui.h"
#include "soloist.h"
#include "spotify.h"

namespace mango {

// Inbox carries results from worker threads to the UI thread. It outlives
// the App, so work still running when the window closes has somewhere to
// put its answer.
struct Inbox {
    std::mutex mu;
    std::vector<std::function<void()>> items;
    std::function<void()> wake;
    bool closed = false;
    void Post(std::function<void()> f);
};

// Workers runs jobs on a few background threads (detached, so closing
// never waits on a slow request).
class Workers {
public:
    explicit Workers(int n);
    ~Workers();
    void Run(std::function<void()> job);

private:
    struct Shared {
        std::mutex mu;
        std::condition_variable cv;
        std::deque<std::function<void()>> jobs;
        bool stop = false;
    };
    std::shared_ptr<Shared> s_;
};

enum class Kind { Liked, Playlist, Album, Artist, Search };

struct View {
    Kind kind = Kind::Liked;
    std::string key;    // what it is, for the cache: "liked", "playlist:<id>", ...
    std::string id;     // playlist/album/artist id, or the search text
    std::string title, kindLabel, byline, image, uri;
    std::string contextUri;  // what to play the tracks within
    std::vector<Ref> artists;   // album's, clickable in the byline
    std::vector<Track> tracks;
    std::vector<Album> albums;  // an artist's
    SearchResults search;
    int total = 0;
    bool loading = false, done = false;
    bool notListed = false;     // Spotify won't list a playlist the user doesn't own
    std::string error;
    std::string filter;
    std::string filteredFor;           // the filter `shown` was made for
    size_t filteredN = size_t(-1);     // and how many tracks there were
    std::vector<int> shown;            // tracks passing the filter
    int selected = -1;
    float scroll = 0;
    bool restoreScroll = false;
    uint64_t loadGen = 0;
};

class App {
public:
    App(HWND hwnd, ID3D11Device* device, std::function<void()> wake);
    ~App();

    void Frame();
    void Pump();
    bool BeforeFrame();  // loads fonts newly needed; true if it did
    bool Animating() const;

    bool InCaption(float x, float y) const;
    void SetScale(float scale);
    void SetDark(bool dark);

    bool Place(RECT& r) const;
    void SavePlace(const RECT& r);
    void Show(bool shown);
    // SetDocked: placed against kitchensink's (the launcher's) right edge,
    // at its height, following it when it moves.
    void SetDocked(bool docked);
    bool Shown() const { return cfg_.shown; }
    void MediaKey(int id);

private:
    struct Config {
        bool placed = false;
        int x = 0, y = 0, w = 0, h = 0;
        bool shown = true;
        std::string refresh;     // sealed with DPAPI
        std::string soloistKey;  // sealed with DPAPI
        int volume = 40;
        bool queueOpen = false;
        std::string lastView;    // key
        int dockedW = 0;         // width when docked
    };

    // Setup and data.
    bool signedIn() const { return demo_ || api_->SignedIn(); }
    void demo();
    void loadConfig();
    void saveConfig();
    void startSoloist();
    void signIn();
    void signOut();
    void loadMe();
    void loadPlaylists(int offset = 0);
    void open(std::shared_ptr<View> v, bool push = true);
    void openKey(const std::string& key);
    std::shared_ptr<View> cached(const std::string& key);
    void openLiked();
    void openPlaylist(const Playlist& p);
    void openPlaylistId(const std::string& id, const std::string& name);
    void openAlbum(const std::string& id, const std::string& name);
    void openArtist(const std::string& id, const std::string& name);
    void runSearch(const std::string& q);
    void loadTracks(std::shared_ptr<View> v, int offset);
    void moreSearch(std::shared_ptr<View> v, const char* type);
    void back();
    void reload();

    // Playback.
    void playIn(const View& v, int index);
    void playContext(const std::string& contextUri, bool shuffle);
    void playUris(std::vector<std::string> uris, std::string offsetUri);
    void togglePlay();
    void checkLiked(const std::string& uri);
    void setLiked(const std::string& uri, bool liked, const std::string& name);

    // Drawing.
    void applyColours();
    void drawTitle(float width);
    void drawSidebar(float x, float y, float w, float h);
    void drawMain(float x, float y, float w, float h);
    void drawSetup(float width);
    void drawHeader(View& v, float width);
    void drawTracks(View& v, float width, bool numbered, bool showAlbum);
    void drawSearch(View& v, float width);
    void drawCards(const std::vector<Album>* albums, const std::vector<Artist>* artists,
                   const std::vector<Playlist>* playlists, float width, const char* id);
    void drawQueue(float x, float y, float w, float h);
    void drawPlayer(float x, float y, float w, float h);
    void trackMenu(const Track& t, const View* in);
    bool iconButton(const char* glyph, const char* tip, bool on = false, float size = 0, bool danger = false);
    bool pillButton(const char* label, bool primary);
    void cover(const std::string& url, float size, const char* fallback = nullptr);
    bool link(const std::string& text, ImVec4 colour, ImFont* font = nullptr, float px = 0);
    void artistLinks(const std::vector<Ref>& artists, ImVec4 colour, float px);
    void note(const std::string& text);  // a passing message along the bottom

    HWND hwnd_;
    bool demo_ = false;
    bool docked_ = false;
    RECT dockedTo_{};       // the launcher's rectangle last docked to
    double dockChecked_ = 0;
    void dock();
    std::function<void()> wake_;
    std::shared_ptr<Inbox> inbox_;
    Workers workers_;
    std::shared_ptr<Api> api_;
    std::shared_ptr<Soloist> soloist_;
    std::unique_ptr<ImageCache> images_;
    std::unique_ptr<MediaSession> media_;
    std::string dir_;
    Config cfg_;

    // Signed in.
    std::shared_ptr<std::atomic<bool>> cancelSignIn_;
    bool signingIn_ = false;
    std::string signInError_;
    std::string meId_, meName_;
    std::vector<Playlist> playlists_;
    bool playlistsLoading_ = false;
    std::string playlistsError_;

    // Views.
    std::shared_ptr<View> view_;
    std::vector<std::shared_ptr<View>> backStack_;
    std::map<std::string, std::shared_ptr<View>> cache_;
    char search_[256] = {};
    std::string searched_;
    double searchTyped_ = 0;
    bool focusSearch_ = false, focusFilter_ = false;
    char keyInput_[256] = {};

    // Player.
    PlayerState player_;
    std::shared_ptr<std::string> deviceId_ = std::make_shared<std::string>();
    std::string likedUri_;
    int likedState_ = -1;  // -1 unknown, 0 no, 1 yes
    bool seeking_ = false;
    float seekFrac_ = 0;
    bool volumeDragging_ = false;
    int volumeShown_ = 40;
    std::string lastQueueFor_;

    std::string note_;
    double noteUntil_ = 0;

    float scale_ = 1;
    bool dark_ = true;
    ImFont* regular_ = nullptr;
    ImFont* semibold_ = nullptr;
    ImFont* bold_ = nullptr;
    ImVec2 barMax_;
    std::vector<std::pair<ImVec2, ImVec2>> hot_;
};

}  // namespace mango
