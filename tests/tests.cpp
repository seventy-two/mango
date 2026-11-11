// Unit tests for the parts that don't need a window or the network.
#include <cstdio>
#include <string>

#include "auth.h"
#include "soloist.h"
#include "spotify.h"
#include "util.h"

using namespace mango;
using json = nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void testUtil() {
    CHECK(UrlEncode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9");
    CHECK(UrlDecode("a+b%26c") == "a b&c");
    CHECK(QueryValue("code=abc&state=x%20y", "state") == "x y");
    CHECK(QueryValue("code=abc&state=x", "nope").empty());
    CHECK(Base64("foobar") == "Zm9vYmFy");
    CHECK(Base64("fo") == "Zm8=");
    CHECK(Base64("f", true) == "Zg");
    CHECK(Unbase64("Zm9vYmFy") == "foobar");
    CHECK(Unbase64(Base64(std::string("\x00\xFF\x10", 3))) == std::string("\x00\xFF\x10", 3));
    CHECK(Duration(0) == "0:00");
    CHECK(Duration(215000) == "3:35");
    CHECK(Duration(3725000) == "1:02:05");
    CHECK(LongDuration(42 * 60000) == "42 min");
    CHECK(LongDuration(72 * 60000) == "1 hr 12 min");
    CHECK(Unprotect(Protect("secret")) == "secret");
}

static void testAuth() {
    // RFC 7636's example.
    CHECK(Challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    auto c = ParseCallback("GET /callback?code=AQB%2Dx&state=s1 HTTP/1.1\r\nHost: 127.0.0.1\r\n");
    CHECK(c.found && c.code == "AQB-x" && c.state == "s1" && c.error.empty());
    auto d = ParseCallback("GET /callback?error=access_denied&state=s1 HTTP/1.1\r\n");
    CHECK(d.found && d.error == "access_denied");
    CHECK(!ParseCallback("GET /favicon.ico HTTP/1.1\r\n").found);
    std::string url = AuthorizeUrl("cid", "chal", "st");
    CHECK(url.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A46075%2Fcallback") != std::string::npos);
    CHECK(url.find("code_challenge_method=S256") != std::string::npos);

    Tokens t;
    t.refresh = "old";
    std::string err;
    CHECK(ParseTokens(R"({"access_token":"a1","token_type":"Bearer","expires_in":3600})", t, err));
    CHECK(t.access == "a1" && t.refresh == "old" && t.expiresAt > NowMs());
    CHECK(ParseTokens(R"({"access_token":"a2","refresh_token":"new","expires_in":3600})", t, err) && t.refresh == "new");
    CHECK(!ParseTokens(R"({"error":"invalid_grant","error_description":"Refresh token revoked"})", t, err));
    CHECK(err == "Refresh token revoked");
}

static void testSpotify() {
    // A playlist items page in the February 2026 shape ("item"), with a
    // removed track (null) and a local file.
    auto page = json::parse(R"({
      "items": [
        {"added_at": "2026-01-02T03:04:05Z", "item": {
          "id": "t1", "uri": "spotify:track:t1", "name": "Song", "duration_ms": 215000, "explicit": true,
          "artists": [{"id": "a1", "uri": "spotify:artist:a1", "name": "Band"}, {"id": "a2", "uri": "spotify:artist:a2", "name": "Guest"}],
          "album": {"id": "al1", "uri": "spotify:album:al1", "name": "Record",
                    "images": [{"url": "big", "width": 640}, {"url": "mid", "width": 300}, {"url": "small", "width": 64}]}}},
        {"added_at": "2026-01-03T00:00:00Z", "item": null},
        {"added_at": "2026-01-04T00:00:00Z", "track": {"id": null, "uri": "spotify:local:x", "name": "Local", "is_local": true,
          "artists": [], "album": {"name": "", "images": []}}}
      ],
      "total": 3, "offset": 0, "next": "https://api.spotify.com/v1/..."
    })");
    auto p = ParseSavedTracks(page);
    CHECK(p.items.size() == 2);
    CHECK(p.total == 3 && p.more);
    CHECK(p.items[0].name == "Song" && p.items[0].artists.size() == 2 && p.items[0].album.name == "Record");
    CHECK(p.items[0].image == "small");
    CHECK(p.items[0].explicit_ && p.items[0].durationMs == 215000);
    CHECK(p.items[0].addedAt == "2026-01-02T03:04:05Z");
    CHECK(!p.items[1].playable);

    auto pls = ParsePlaylists(json::parse(R"({
      "items": [{"id": "p1", "uri": "spotify:playlist:p1", "name": "Mine", "collaborative": false,
                 "owner": {"id": "me", "display_name": "Megz"}, "images": [{"url": "mosaic", "width": null}],
                 "items": {"total": 42}},
                {"id": "p2", "uri": "spotify:playlist:p2", "name": "Old shape", "owner": {"id": "x"},
                 "images": null, "tracks": {"total": 7}}],
      "total": 2, "offset": 0, "next": null
    })"));
    CHECK(pls.items.size() == 2 && !pls.more);
    CHECK(pls.items[0].total == 42 && pls.items[0].owner == "Megz" && pls.items[0].image == "mosaic");
    CHECK(pls.items[1].total == 7 && pls.items[1].owner == "x" && pls.items[1].image.empty());

    Album al;
    al.id = "al9";
    al.name = "LP";
    al.image = "cov";
    auto at = ParseTracks(json::parse(R"({"items": [{"id": "t", "uri": "spotify:track:t", "name": "One",
      "duration_ms": 1000, "artists": []}], "total": 1})"),
                          &al);
    CHECK(at.items.size() == 1 && at.items[0].album.id == "al9" && at.items[0].image == "cov");

    auto s = ParseSearch(json::parse(R"({
      "tracks": {"items": [], "total": 0},
      "albums": {"items": [{"id": "x", "name": "A", "album_type": "single", "release_date": "1999-05-01",
                            "images": [], "artists": [{"id": "r", "name": "R"}]}], "total": 1, "next": "n"},
      "artists": {"items": [{"id": "r", "name": "R", "images": [], "genres": ["rock"]}], "total": 1},
      "playlists": {"items": [null, {"id": "q", "name": "Q", "owner": {"id": "o"}}], "total": 2}
    })"));
    CHECK(s.albums.items.size() == 1 && s.albums.items[0].year == "1999" && s.albums.more);
    CHECK(s.artists.items[0].genres.size() == 1);
    CHECK(s.playlists.items.size() == 1);
}

static void testSoloist() {
    // The playback_state example from Soloist's WebSocket documentation.
    auto msg = json::parse(R"({
      "type": "playback_state", "status": "playing",
      "item": {"uri": "spotify:track:2JRo0gjbX4GrCqBYdRohoo", "entity_type": "track",
        "decorations": {"identity": {"name": "My Song"},
          "visual_identity": {"cover": [{"url": "s", "size": "small"}, {"url": "d", "size": "default"}]},
          "parent": {"entity": {"uri": "spotify:album:4aaw", "entity_type": "album", "decorations": {"identity": {"name": "Album Name"}}}},
          "creators": [{"entity": {"uri": "spotify:artist:zz", "entity_type": "artist", "decorations": {"identity": {"name": "Artist Name"}}}}],
          "playback": {"duration_ms": 210000, "content_ratings": []}}},
      "context": {"uri": "spotify:playlist:37i9", "entity_type": "playlist", "decorations": {"identity": {"name": "Today's Top Hits"}}},
      "position": {"position_ms": 45000, "timestamp_ms": 1747654321000, "speed": 1.0},
      "volume": 65, "is_active": true,
      "options": {"shuffle": false, "repeat": "off", "playback_speed": 1.0, "modes": {}},
      "available_actions": {"pause": {}}
    })");
    PlayerState s;
    CHECK(Apply(s, msg));
    CHECK(s.status == "playing" && s.item.name == "My Song" && s.item.image == "d");
    CHECK(s.item.parentName == "Album Name" && s.item.creators.size() == 1 && s.item.creators[0].second == "Artist Name");
    CHECK(s.item.durationMs == 210000 && s.context.uri == "spotify:playlist:37i9" && s.volume == 65);
    CHECK(s.PositionNow(1747654321000 + 5000) == 50000);
    CHECK(s.PositionNow(1747654321000 + 999999) == 210000);  // never past the end

    CHECK(Apply(s, json::parse(R"({"type":"playback_changed","status":"paused"})")) && s.status == "paused");
    CHECK(Apply(s, json::parse(R"({"type":"position_sync","position":{"position_ms":1000,"timestamp_ms":5,"speed":0.0}})")));
    CHECK(s.PositionNow(999999) == 1000);
    CHECK(Apply(s, json::parse(R"({"type":"options_changed","options":{"shuffle":true,"repeat":"track"}})")));
    CHECK(s.shuffle && s.repeat == "track");
    CHECK(Apply(s, json::parse(R"({"type":"auth_state","logged_in":true,"is_active":false,"device_name":"Mango"})")));
    CHECK(s.loggedIn && !s.active && s.deviceName == "Mango");
    CHECK(Apply(s, json::parse(R"({"type":"queue_changed","previous":[],"upcoming":[
      {"uid":"u","source":"queue","item":{"uri":"spotify:track:n","entity_type":"track","decorations":{"identity":{"name":"Next Song"}}}}]})")));
    CHECK(s.upcoming.size() == 1 && s.upcoming[0].source == "queue" && s.upcoming[0].item.name == "Next Song");
    uint64_t v = s.version;
    CHECK(!Apply(s, json::parse(R"({"type":"command_result","command":"pause"})")));
    CHECK(s.version == v);
}

int main() {
    testUtil();
    testAuth();
    testSpotify();
    testSoloist();
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("tests passed\n");
    return 0;
}
