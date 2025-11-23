#pragma once

#include <vector>
#include <memory>
#include "pan/track/track.h"

namespace pan {

/**
 * Manages all tracks in the project
 */
class TrackManager {
public:
    TrackManager();
    ~TrackManager();

    // Track management
    std::shared_ptr<Track> createTrack(const std::string& name, Track::Type type = Track::Type::Audio);
    void removeTrack(std::shared_ptr<Track> track);
    std::vector<std::shared_ptr<Track>> getTracks() const { return tracks_; }
    
    // Track access
    std::shared_ptr<Track> getTrack(size_t index) const;
    size_t getTrackCount() const { return tracks_.size(); }

    // Processing
    void processAllTracks(class AudioBuffer& buffer, size_t numFrames);

private:
    std::vector<std::shared_ptr<Track>> tracks_;
};

} // namespace pan

