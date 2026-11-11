#include "media.h"

#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>

#include "soloist.h"
#include "util.h"

namespace mango {

using namespace winrt::Windows::Media;
using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Storage::Streams::RandomAccessStreamReference;

struct MediaSession::Impl {
    SystemMediaTransportControls smtc{nullptr};
    winrt::event_token buttons{}, seeks{};
    std::string shownUri, shownImage, shownStatus;
    int64_t shownAt = -1, shownPos = -1, shownDur = -1;
    std::atomic<int64_t> seekTarget{0};
};

MediaSession::MediaSession(HWND hwnd) : impl_(std::make_unique<Impl>()) {
    try {
        auto interop = winrt::get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        SystemMediaTransportControls smtc{nullptr};
        winrt::check_hresult(interop->GetForWindow(hwnd, winrt::guid_of<SystemMediaTransportControls>(), winrt::put_abi(smtc)));
        impl_->smtc = smtc;
        smtc.IsEnabled(false);
        smtc.IsPlayEnabled(true);
        smtc.IsPauseEnabled(true);
        smtc.IsNextEnabled(true);
        smtc.IsPreviousEnabled(true);
        smtc.IsStopEnabled(false);
        // Both arrive on another thread: hand them to the window's.
        impl_->buttons = smtc.ButtonPressed([hwnd](auto&&, SystemMediaTransportControlsButtonPressedEventArgs const& a) {
            PostMessageW(hwnd, WM_APP_MEDIA, WPARAM(a.Button()), 0);
        });
        Impl* impl = impl_.get();
        impl_->seeks = smtc.PlaybackPositionChangeRequested(
            [hwnd, impl](auto&&, PlaybackPositionChangeRequestedEventArgs const& a) {
                impl->seekTarget = std::chrono::duration_cast<std::chrono::milliseconds>(a.RequestedPlaybackPosition()).count();
                PostMessageW(hwnd, WM_APP_MEDIA, MediaSeek, 0);
            });
    } catch (...) {
        impl_->smtc = nullptr;  // no media controls: Mango works without them
    }
}

MediaSession::~MediaSession() {
    if (!impl_->smtc) return;
    try {
        impl_->smtc.ButtonPressed(impl_->buttons);
        impl_->smtc.PlaybackPositionChangeRequested(impl_->seeks);
        impl_->smtc.IsEnabled(false);
    } catch (...) {
    }
}

int64_t MediaSession::SeekTarget() const { return impl_->seekTarget; }

void MediaSession::Update(const PlayerState& s) {
    auto& m = *impl_;
    if (!m.smtc) return;
    try {
        bool has = s.loggedIn && !s.item.uri.empty();
        std::string status = !has ? "none" : s.status;
        if (!has) {
            if (m.shownStatus != "none") {
                m.smtc.PlaybackStatus(MediaPlaybackStatus::Closed);
                m.smtc.DisplayUpdater().ClearAll();
                m.smtc.IsEnabled(false);
                m.shownUri.clear();
                m.shownStatus = "none";
            }
            return;
        }
        if (m.shownStatus == "none" || m.shownStatus.empty()) m.smtc.IsEnabled(true);
        if (s.item.uri != m.shownUri || s.item.image != m.shownImage) {
            auto u = m.smtc.DisplayUpdater();
            u.ClearAll();
            u.Type(MediaPlaybackType::Music);
            auto mp = u.MusicProperties();
            mp.Title(winrt::hstring(Widen(s.item.name)));
            std::string artists;
            for (const auto& c : s.item.creators) artists += (artists.empty() ? "" : ", ") + c.second;
            mp.Artist(winrt::hstring(Widen(artists)));
            mp.AlbumTitle(winrt::hstring(Widen(s.item.parentName)));
            if (!s.item.image.empty()) u.Thumbnail(RandomAccessStreamReference::CreateFromUri(Uri(winrt::hstring(Widen(s.item.image)))));
            u.Update();
            m.shownUri = s.item.uri;
            m.shownImage = s.item.image;
        }
        if (status != m.shownStatus) {
            m.smtc.PlaybackStatus(status == "playing" ? MediaPlaybackStatus::Playing
                                  : status == "buffering" ? MediaPlaybackStatus::Changing
                                  : status == "paused" ? MediaPlaybackStatus::Paused
                                                       : MediaPlaybackStatus::Stopped);
            m.shownStatus = status;
        }
        // The timeline, whenever Soloist moves its position anchor.
        if (s.positionAt != m.shownAt || s.positionMs != m.shownPos || s.item.durationMs != m.shownDur) {
            SystemMediaTransportControlsTimelineProperties tl;
            int64_t dur = std::max<int64_t>(s.item.durationMs, 0);
            int64_t pos = std::clamp<int64_t>(s.PositionNow(NowMs()), 0, dur);
            tl.StartTime(TimeSpan{0});
            tl.EndTime(std::chrono::milliseconds(dur));
            tl.MinSeekTime(TimeSpan{0});
            tl.MaxSeekTime(std::chrono::milliseconds(dur));
            tl.Position(std::chrono::milliseconds(pos));
            m.smtc.UpdateTimelineProperties(tl);
            m.shownAt = s.positionAt;
            m.shownPos = s.positionMs;
            m.shownDur = s.item.durationMs;
        }
    } catch (...) {
    }
}

}  // namespace mango
