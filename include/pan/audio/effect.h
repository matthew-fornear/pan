#pragma once

#include <string>
#include <memory>

namespace pan {

// Forward declaration
class AudioBuffer;

/**
 * Base class for audio effects
 */
class Effect {
public:
    virtual ~Effect() = default;
    
    virtual void process(AudioBuffer& buffer, size_t numFrames) = 0;
    virtual std::string getName() const = 0;
    virtual void reset() = 0;  // Reset internal state
    
    bool isEnabled() const { return enabled_; }
    void setEnabled(bool enabled) { enabled_ = enabled; }
    
protected:
    bool enabled_ = true;
};

} // namespace pan

