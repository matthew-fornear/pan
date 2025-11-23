#include "pan/track/track_manager.h"
#include "pan/audio/audio_buffer.h"
#include <algorithm>

namespace pan {

TrackManager::TrackManager() = default;

TrackManager::~TrackManager() = default;

std::shared_ptr<Track> TrackManager::createTrack(const std::string& name, Track::Type type) {
    auto track = std::make_shared<Track>(name, type);
    tracks_.push_back(track);
    return track;
}

void TrackManager::removeTrack(std::shared_ptr<Track> track) {
    tracks_.erase(
        std::remove(tracks_.begin(), tracks_.end(), track),
        tracks_.end()
    );
}

std::shared_ptr<Track> TrackManager::getTrack(size_t index) const {
    if (index >= tracks_.size()) {
        return nullptr;
    }
    return tracks_[index];
}

void TrackManager::processAllTracks(AudioBuffer& buffer, size_t numFrames) {
    // Process each track and mix into the buffer
    for (auto& track : tracks_) {
        if (track && !track->isMuted()) {
            // TODO: Create temporary buffer for track processing
            track->process(buffer, numFrames);
        }
    }
}

} // namespace pan

