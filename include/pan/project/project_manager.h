#pragma once

#include <string>
#include <memory>
#include "pan/track/track_manager.h"

namespace pan {

/**
 * Manages project file operations (save, load, export)
 */
class ProjectManager {
public:
    ProjectManager();
    ~ProjectManager();

    // Project file operations
    bool createNewProject(const std::string& name);
    bool loadProject(const std::string& filepath);
    bool saveProject(const std::string& filepath);
    bool saveProject();

    // Project properties
    std::string getProjectName() const { return projectName_; }
    std::string getProjectPath() const { return projectPath_; }
    bool isDirty() const { return isDirty_; }

    // Track manager access
    std::shared_ptr<TrackManager> getTrackManager() const { return trackManager_; }

    // Project settings
    double getSampleRate() const { return sampleRate_; }
    void setSampleRate(double sampleRate) { sampleRate_ = sampleRate; }
    
    size_t getBufferSize() const { return bufferSize_; }
    void setBufferSize(size_t bufferSize) { bufferSize_ = bufferSize; }

private:
    std::string projectName_;
    std::string projectPath_;
    bool isDirty_;
    
    double sampleRate_;
    size_t bufferSize_;
    
    std::shared_ptr<TrackManager> trackManager_;
};

} // namespace pan

