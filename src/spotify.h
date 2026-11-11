// The Spotify Web API: what Mango shows (playlists, Liked Songs, albums,
// artists, search) and starting playback on Mango's own device.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "auth.h"
#include "net.h"
#include "nlohmann/json.hpp"

namespace mango {

struct Ref {
    std::string id, uri, name;
};

struct Track {
    std::string id, uri, name;
    std::vector<Ref> artists;
    Ref album;
    std::string image;  // smallest cover that's at least 64px
    int64_t durationMs = 0;
    bool playable = true;
    bool explicit_ = false;
    std::string addedAt;  // for playlists and Liked Songs
};

struct Playlist {
    std::string id, uri, name, owner, ownerId, image, description;
    int total = 0;
    bool collaborative = false;
};

struct Album {
    std::string id, uri, name, image, year, type;  // type: album, single, compilation
    std::vector<Ref> artists;
    int total = 0;
};

struct Artist {
    std::string id, uri, name, image;
    std::vector<std::string> genres;
};

template <typename T>
struct Page {
    std::vector<T> items;
    int total = 0;
    int offset = 0;
    bool more = false;
};

struct SearchResults {
    Page<Track> tracks;
    Page<Album> albums;
    Page<Artist> artists;
    Page<Playlist> playlists;
};

struct Device {
    std::string id, name, type;
    bool active = false;
};

// Parsing, separate from requests so it can be tested.
Track ParseTrack(const nlohmann::json& j);
Playlist ParsePlaylist(const nlohmann::json& j);
Album ParseAlbum(const nlohmann::json& j);
Artist ParseArtist(const nlohmann::json& j);
Page<Playlist> ParsePlaylists(const nlohmann::json& j);
// ParseSavedTracks reads /me/tracks and playlist items pages (each item
// wrapping its track as "item", or "track" in older responses).
Page<Track> ParseSavedTracks(const nlohmann::json& j);
Page<Track> ParseTracks(const nlohmann::json& j, const Album* album = nullptr);
Page<Album> ParseAlbums(const nlohmann::json& j);
SearchResults ParseSearch(const nlohmann::json& j);
std::string PickImage(const nlohmann::json& images, int atLeast = 64);

// Api makes Web API requests, refreshing the access token as needed.
// Safe to use from several threads.
class Api {
public:
    explicit Api(std::string clientId);

    void SetTokens(const Tokens& t);
    Tokens GetTokens();
    bool SignedIn();
    // onTokens is called (on a worker thread) when the refresh token changes.
    void OnTokens(std::function<void(const Tokens&)> f) { onTokens_ = std::move(f); }
    bool SignIn(std::string& error, const std::atomic<bool>& cancel);

    struct Result {
        bool ok = false;
        int status = 0;
        std::string error;
        nlohmann::json json;
    };
    // Call makes a request to api.spotify.com/v1<path>.
    Result Call(const std::string& method, const std::string& path, const std::string& body = {});

    // Fetch helpers; each returns false with error set on failure.
    bool Me(std::string& id, std::string& name, std::string& error);
    bool MyPlaylists(int offset, Page<Playlist>& out, std::string& error);
    bool Liked(int offset, Page<Track>& out, std::string& error);
    bool PlaylistItems(const std::string& id, int offset, Page<Track>& out, std::string& error, int* status = nullptr);
    bool GetPlaylist(const std::string& id, Playlist& out, std::string& error);
    bool GetAlbum(const std::string& id, Album& album, Page<Track>& tracks, std::string& error);
    bool AlbumTracks(const Album& album, int offset, Page<Track>& out, std::string& error);
    bool GetArtist(const std::string& id, Artist& out, std::string& error);
    bool ArtistAlbums(const std::string& id, int offset, Page<Album>& out, std::string& error);
    bool Search(const std::string& q, const std::string& types, int offset, SearchResults& out, std::string& error);
    bool Devices(std::vector<Device>& out, std::string& error);
    // Play starts a context (album/playlist/collection) at a track, or a
    // list of tracks, on the given device.
    // offsetPosition (0-based), when not -1, is used instead of offsetUri.
    bool Play(const std::string& deviceId, const std::string& contextUri, const std::string& offsetUri,
              const std::vector<std::string>& uris, std::string& error, int offsetPosition = -1);
    // Saved checks which of up to 40 URIs are in the library.
    bool Saved(const std::vector<std::string>& uris, std::vector<bool>& out, std::string& error);
    bool SetSaved(const std::string& uri, bool saved, std::string& error);

private:
    bool ensureToken(std::string& error);

    std::string clientId_;
    Http http_;
    std::mutex mu_;
    Tokens tokens_;
    std::function<void(const Tokens&)> onTokens_;
};

}  // namespace mango
