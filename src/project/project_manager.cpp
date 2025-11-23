#include "pan/project/project_manager.h"
#include "pan/track/track_manager.h"
#include <iostream>

namespace pan {

ProjectManager::ProjectManager()
    : projectName_("Untitled Project")
    , projectPath_("")
    , isDirty_(false)
    , sampleRate_(44100.0)
    , bufferSize_(512)
    , trackManager_(std::make_shared<TrackManager>())
{
}

ProjectManager::~ProjectManager() = default;

bool ProjectManager::createNewProject(const std::string& name) {
    projectName_ = name;
    projectPath_ = "";
    isDirty_ = false;
    trackManager_ = std::make_shared<TrackManager>();
    
    std::cout << "Created new project: " << projectName_ << std::endl;
    return true;
}

bool ProjectManager::loadProject(const std::string& filepath) {
    // TODO: Implement project file loading
    std::cout << "Loading project from: " << filepath << std::endl;
    projectPath_ = filepath;
    isDirty_ = false;
    return true;
}

bool ProjectManager::saveProject(const std::string& filepath) {
    // TODO: Implement project file saving
    std::cout << "Saving project to: " << filepath << std::endl;
    projectPath_ = filepath;
    isDirty_ = false;
    return true;
}

bool ProjectManager::saveProject() {
    if (projectPath_.empty()) {
        std::cerr << "No project path set. Use saveProject(filepath)" << std::endl;
        return false;
    }
    return saveProject(projectPath_);
}

} // namespace pan

