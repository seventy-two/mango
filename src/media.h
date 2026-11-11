// Windows' media controls (SMTC): tells Windows what Mango is playing, so
// it shows in the volume overlay and anything else reading media sessions,
// and takes the keyboard's media keys when Mango is the current player.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace mango {

struct PlayerState;

// Buttons arrive as WM_APP_MEDIA with wParam one of these.
constexpr UINT WM_APP_MEDIA = WM_APP + 4;
enum MediaButton { MediaPlay = 0, MediaPause = 1, MediaStop = 2, MediaNext = 6, MediaPrevious = 7, MediaSeek = 100 };

class MediaSession {
public:
    explicit MediaSession(HWND hwnd);
    ~MediaSession();
    // Update shows the player's state; cheap to call on every change.
    void Update(const PlayerState& s);
    // SeekTarget is the position (ms) asked for by the last MediaSeek.
    int64_t SeekTarget() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mango
