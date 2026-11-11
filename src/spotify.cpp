#include "spotify.h"

#include <windows.h>

#include <algorithm>

#include "util.h"

namespace mango {

using json = nlohmann::json;

namespace {

std::string str(const json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

int num(const json& j, const char* key) {
    if (!j.is_object()) return 0;
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return 0;
    return it->get<int>();
}

const json& sub(const json& j, const char* key) {
    static const json none;
    if (!j.is_object()) return none;
    auto it = j.find(key);
    return it == j.end() ? none : *it;
}

Ref parseRef(const json& j) { return {str(j, "id"), str(j, "uri"), str(j, "name")}; }

std::vector<Ref> parseRefs(const json& arr) {
    std::vector<Ref> out;
    if (arr.is_array())
        for (const auto& a : arr) out.push_back(parseRef(a));
    return out;
}

template <typename T, typename F>
Page<T> parsePage(const json& j, F each) {
    Page<T> p;
    p.total = num(j, "total");
    p.offset = num(j, "offset");
    p.more = sub(j, "next").is_string();
    const json& items = sub(j, "items");
    if (items.is_array())
        for (const auto& i : items)
            if (i.is_object()) each(i, p.items);
    return p;
}

}  // namespace

std::string PickImage(const json& images, int atLeast) {
    if (!images.is_array() || images.empty()) return {};
    // Spotify lists the biggest first; take the smallest that's big enough.
    std::string best;
    int bestW = 1 << 30;
    for (const auto& i : images) {
        int w = num(i, "width");
        if (w == 0) w = 300;  // unknown size: user-made playlist mosaics
        if (w >= atLeast && w < bestW) {
            bestW = w;
            best = str(i, "url");
        }
    }
    return best.empty() ? str(images[0], "url") : best;
}

Track ParseTrack(const json& j) {
    Track t;
    t.id = str(j, "id");
    t.uri = str(j, "uri");
    t.name = str(j, "name");
    t.artists = parseRefs(sub(j, "artists"));
    const json& al = sub(j, "album");
    t.album = parseRef(al);
    t.image = PickImage(sub(al, "images"), 64);
    t.durationMs = sub(j, "duration_ms").is_number() ? sub(j, "duration_ms").get<int64_t>() : 0;
    if (sub(j, "is_playable").is_boolean()) t.playable = sub(j, "is_playable").get<bool>();
    if (sub(j, "is_local").is_boolean() && sub(j, "is_local").get<bool>()) t.playable = false;
    if (sub(j, "explicit").is_boolean()) t.explicit_ = sub(j, "explicit").get<bool>();
    return t;
}

Playlist ParsePlaylist(const json& j) {
    Playlist p;
    p.id = str(j, "id");
    p.uri = str(j, "uri");
    p.name = str(j, "name");
    p.description = str(j, "description");
    const json& owner = sub(j, "owner");
    p.owner = str(owner, "display_name");
    p.ownerId = str(owner, "id");
    if (p.owner.empty()) p.owner = p.ownerId;
    p.image = PickImage(sub(j, "images"), 120);
    // Since February 2026 "tracks" is "items"; take whichever is there.
    const json& items = sub(j, "items");
    const json& tracks = sub(j, "tracks");
    p.total = items.is_object() ? num(items, "total") : num(tracks, "total");
    if (sub(j, "collaborative").is_boolean()) p.collaborative = sub(j, "collaborative").get<bool>();
    return p;
}

Album ParseAlbum(const json& j) {
    Album a;
    a.id = str(j, "id");
    a.uri = str(j, "uri");
    a.name = str(j, "name");
    a.image = PickImage(sub(j, "images"), 120);
    a.type = str(j, "album_type");
    a.year = str(j, "release_date").substr(0, 4);
    a.artists = parseRefs(sub(j, "artists"));
    a.total = num(j, "total_tracks");
    return a;
}

Artist ParseArtist(const json& j) {
    Artist a;
    a.id = str(j, "id");
    a.uri = str(j, "uri");
    a.name = str(j, "name");
    a.image = PickImage(sub(j, "images"), 120);
    const json& g = sub(j, "genres");
    if (g.is_array())
        for (const auto& s : g)
            if (s.is_string()) a.genres.push_back(s.get<std::string>());
    return a;
}

Page<Playlist> ParsePlaylists(const json& j) {
    return parsePage<Playlist>(j, [](const json& i, std::vector<Playlist>& out) { out.push_back(ParsePlaylist(i)); });
}

Page<Track> ParseSavedTracks(const json& j) {
    return parsePage<Track>(j, [](const json& i, std::vector<Track>& out) {
        const json& t = sub(i, "item").is_object() ? sub(i, "item") : sub(i, "track");
        if (!t.is_object()) return;  // removed from Spotify
        if (str(t, "type") == "episode") return;
        Track tr = ParseTrack(t);
        tr.addedAt = str(i, "added_at");
        out.push_back(std::move(tr));
    });
}

Page<Track> ParseTracks(const json& j, const Album* album) {
    return parsePage<Track>(j, [album](const json& i, std::vector<Track>& out) {
        Track t = ParseTrack(i);
        if (album && t.album.id.empty()) {
            t.album = {album->id, album->uri, album->name};
            t.image = album->image;
        }
        out.push_back(std::move(t));
    });
}

Page<Album> ParseAlbums(const json& j) {
    return parsePage<Album>(j, [](const json& i, std::vector<Album>& out) { out.push_back(ParseAlbum(i)); });
}

SearchResults ParseSearch(const json& j) {
    SearchResults r;
    r.tracks = ParseTracks(sub(j, "tracks"));
    r.albums = ParseAlbums(sub(j, "albums"));
    r.artists = parsePage<Artist>(sub(j, "artists"), [](const json& i, std::vector<Artist>& out) { out.push_back(ParseArtist(i)); });
    r.playlists = ParsePlaylists(sub(j, "playlists"));
    return r;
}

Api::Api(std::string clientId) : clientId_(std::move(clientId)) {}

void Api::SetTokens(const Tokens& t) {
    std::lock_guard<std::mutex> lock(mu_);
    tokens_ = t;
}

Tokens Api::GetTokens() {
    std::lock_guard<std::mutex> lock(mu_);
    return tokens_;
}

bool Api::SignedIn() {
    std::lock_guard<std::mutex> lock(mu_);
    return !tokens_.refresh.empty();
}

bool Api::SignIn(std::string& error, const std::atomic<bool>& cancel) {
    Tokens t;
    if (!mango::SignIn(http_, clientId_, t, error, cancel)) return false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        tokens_ = t;
    }
    if (onTokens_) onTokens_(t);
    return true;
}

bool Api::ensureToken(std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (tokens_.refresh.empty()) {
        error = "not signed in";
        return false;
    }
    if (!tokens_.access.empty() && NowMs() < tokens_.expiresAt - 60000) return true;
    std::string before = tokens_.refresh;
    if (!Refresh(http_, clientId_, tokens_, error)) {
        if (error.find("revoked") != std::string::npos || error.find("invalid") != std::string::npos ||
            error.find("Invalid") != std::string::npos)
            tokens_ = {};  // signed out elsewhere: ask again
        return false;
    }
    if (onTokens_) onTokens_(tokens_);
    return true;
}

Api::Result Api::Call(const std::string& method, const std::string& path, const std::string& body) {
    Result res;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!ensureToken(res.error)) return res;
        std::string token;
        {
            std::lock_guard<std::mutex> lock(mu_);
            token = tokens_.access;
        }
        Headers h{{"Authorization", "Bearer " + token}};
        if (!body.empty() || method == "PUT" || method == "POST") h.push_back({"Content-Type", "application/json"});
        auto r = http_.Request(method, "https://api.spotify.com/v1" + path, h, body);
        res.status = int(r.status);
        if (r.status == 401 && attempt == 0) {
            std::lock_guard<std::mutex> lock(mu_);
            tokens_.access.clear();
            continue;
        }
        if (r.status == 429 && attempt < 2) {
            int wait = std::clamp(r.retryAfter, 1, 10);
            Sleep(DWORD(wait * 1000));
            continue;
        }
        if (r.status >= 200 && r.status < 300) {
            res.ok = true;
            if (!r.body.empty()) {
                try {
                    res.json = json::parse(r.body);
                } catch (...) {
                }
            }
            return res;
        }
        res.error = r.error;
        try {
            auto j = json::parse(r.body);
            std::string m = str(sub(j, "error"), "message");
            if (!m.empty()) res.error = m;
            if (str(j, "reason") == "QUOTA_EXCEEDED") res.error = "Spotify's request quota is used up for now";
        } catch (...) {
        }
        if (r.status == 429) res.error = "Spotify asked us to slow down; try again shortly";
        return res;
    }
    return res;
}

bool Api::Me(std::string& id, std::string& name, std::string& error) {
    auto r = Call("GET", "/me");
    if (!r.ok) return error = r.error, false;
    id = str(r.json, "id");
    name = str(r.json, "display_name");
    return true;
}

bool Api::MyPlaylists(int offset, Page<Playlist>& out, std::string& error) {
    auto r = Call("GET", "/me/playlists?limit=50&offset=" + std::to_string(offset));
    if (!r.ok) return error = r.error, false;
    out = ParsePlaylists(r.json);
    return true;
}

bool Api::Liked(int offset, Page<Track>& out, std::string& error) {
    auto r = Call("GET", "/me/tracks?limit=50&offset=" + std::to_string(offset));
    if (!r.ok) return error = r.error, false;
    out = ParseSavedTracks(r.json);
    return true;
}

bool Api::PlaylistItems(const std::string& id, int offset, Page<Track>& out, std::string& error, int* status) {
    auto r = Call("GET", "/playlists/" + id + "/items?limit=50&offset=" + std::to_string(offset));
    if (status) *status = r.status;
    if (!r.ok) return error = r.error, false;
    out = ParseSavedTracks(r.json);
    return true;
}

bool Api::GetPlaylist(const std::string& id, Playlist& out, std::string& error) {
    auto r = Call("GET", "/playlists/" + id + "?fields=id,uri,name,description,owner,images,collaborative,items(total),tracks(total)");
    if (!r.ok) return error = r.error, false;
    out = ParsePlaylist(r.json);
    return true;
}

bool Api::GetAlbum(const std::string& id, Album& album, Page<Track>& tracks, std::string& error) {
    auto r = Call("GET", "/albums/" + id);
    if (!r.ok) return error = r.error, false;
    album = ParseAlbum(r.json);
    tracks = ParseTracks(sub(r.json, "tracks"), &album);
    return true;
}

bool Api::AlbumTracks(const Album& album, int offset, Page<Track>& out, std::string& error) {
    auto r = Call("GET", "/albums/" + album.id + "/tracks?limit=50&offset=" + std::to_string(offset));
    if (!r.ok) return error = r.error, false;
    out = ParseTracks(r.json, &album);
    return true;
}

bool Api::GetArtist(const std::string& id, Artist& out, std::string& error) {
    auto r = Call("GET", "/artists/" + id);
    if (!r.ok) return error = r.error, false;
    out = ParseArtist(r.json);
    return true;
}

bool Api::ArtistAlbums(const std::string& id, int offset, Page<Album>& out, std::string& error) {
    std::string base = "/artists/" + id + "/albums?include_groups=album,single,compilation&offset=" + std::to_string(offset);
    auto r = Call("GET", base + "&limit=50");
    if (r.status == 400) r = Call("GET", base + "&limit=10");  // in case dev mode caps it
    if (!r.ok) return error = r.error, false;
    out = ParseAlbums(r.json);
    return true;
}

bool Api::Search(const std::string& q, const std::string& types, int offset, SearchResults& out, std::string& error) {
    // Development mode caps search at 10 a page.
    auto r = Call("GET", "/search?q=" + UrlEncode(q) + "&type=" + types + "&limit=10&offset=" + std::to_string(offset));
    if (!r.ok) return error = r.error, false;
    out = ParseSearch(r.json);
    return true;
}

bool Api::Devices(std::vector<Device>& out, std::string& error) {
    auto r = Call("GET", "/me/player/devices");
    if (!r.ok) return error = r.error, false;
    out.clear();
    const json& ds = sub(r.json, "devices");
    if (ds.is_array())
        for (const auto& d : ds) {
            Device dev{str(d, "id"), str(d, "name"), str(d, "type")};
            if (sub(d, "is_active").is_boolean()) dev.active = sub(d, "is_active").get<bool>();
            out.push_back(dev);
        }
    return true;
}

bool Api::Play(const std::string& deviceId, const std::string& contextUri, const std::string& offsetUri,
               const std::vector<std::string>& uris, std::string& error) {
    json body = json::object();
    if (!contextUri.empty()) body["context_uri"] = contextUri;
    else if (!uris.empty()) body["uris"] = uris;
    if (!offsetUri.empty()) body["offset"] = {{"uri", offsetUri}};
    std::string path = "/me/player/play";
    if (!deviceId.empty()) path += "?device_id=" + UrlEncode(deviceId);
    auto r = Call("PUT", path, body.dump());
    if (!r.ok) return error = r.error, false;
    return true;
}

bool Api::Saved(const std::vector<std::string>& uris, std::vector<bool>& out, std::string& error) {
    out.assign(uris.size(), false);
    if (uris.empty()) return true;
    std::string list;
    for (const auto& u : uris) list += (list.empty() ? "" : ",") + u;
    auto r = Call("GET", "/me/library/contains?uris=" + UrlEncode(list));
    if (!r.ok) return error = r.error, false;
    if (r.json.is_array())
        for (size_t i = 0; i < r.json.size() && i < out.size(); i++) out[i] = r.json[i].is_boolean() && r.json[i].get<bool>();
    return true;
}

bool Api::SetSaved(const std::string& uri, bool saved, std::string& error) {
    auto r = Call(saved ? "PUT" : "DELETE", "/me/library?uris=" + UrlEncode(uri));
    if (!r.ok) return error = r.error, false;
    return true;
}

}  // namespace mango
