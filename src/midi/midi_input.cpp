#include "pan/midi/midi_input.h"
#include <iostream>
#include <cstring>

#ifdef __linux__
#include <alsa/asoundlib.h>
#endif

namespace pan {

MidiInput::MidiInput()
    : deviceName_("")
    , deviceIndex_(-1)
    , isOpen_(false)
    , isRunning_(false)
    , shouldStop_(false)
    , sequencerHandle_(nullptr)
    , portId_(-1)
{
}

MidiInput::~MidiInput() {
    stop();
    closeDevice();
}

std::vector<std::string> MidiInput::enumerateDevices() {
    std::vector<std::string> devices;
    
#ifdef PAN_USE_ALSA_MIDI
    int card = -1;
    snd_ctl_t* ctl = nullptr;
    snd_rawmidi_info_t* info = nullptr;
    
    snd_rawmidi_info_alloca(&info);
    
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char name[32];
        snprintf(name, sizeof(name), "hw:%d", card);
        
        if (snd_ctl_open(&ctl, name, 0) < 0) {
            continue;
        }
        
        int device = -1;
        while (snd_ctl_rawmidi_next_device(ctl, &device) >= 0 && device >= 0) {
            snd_rawmidi_info_set_device(info, device);
            snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
            
            if (snd_ctl_rawmidi_info(ctl, info) >= 0) {
                const char* subname = snd_rawmidi_info_get_subdevice_name(info);
                if (subname && *subname) {
                    char fullName[256];
                    snprintf(fullName, sizeof(fullName), "%s (%s)", name, subname);
                    devices.push_back(fullName);
                }
            }
        }
        
        snd_ctl_close(ctl);
    }
    
    // Also try ALSA sequencer clients (more common for MIDI keyboards)
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) >= 0) {
        snd_seq_client_info_t* cinfo;
        snd_seq_port_info_t* pinfo;
        
        snd_seq_client_info_alloca(&cinfo);
        snd_seq_port_info_alloca(&pinfo);
        
        snd_seq_client_info_set_client(cinfo, -1);
        while (snd_seq_query_next_client(seq, cinfo) >= 0) {
            int client = snd_seq_client_info_get_client(cinfo);
            if (client == SND_SEQ_CLIENT_SYSTEM) {
                continue;
            }
            
            snd_seq_port_info_set_client(pinfo, client);
            snd_seq_port_info_set_port(pinfo, -1);
            while (snd_seq_query_next_port(seq, pinfo) >= 0) {
                unsigned int caps = snd_seq_port_info_get_capability(pinfo);
                if ((caps & SND_SEQ_PORT_CAP_READ) && (caps & SND_SEQ_PORT_CAP_SUBS_READ)) {
                    const char* clientName = snd_seq_client_info_get_name(cinfo);
                    const char* portName = snd_seq_port_info_get_name(pinfo);
                    char fullName[256];
                    snprintf(fullName, sizeof(fullName), "%s:%s", clientName, portName);
                    devices.push_back(fullName);
                }
            }
        }
        snd_seq_close(seq);
    }
#endif

    return devices;
}

bool MidiInput::openDevice(int deviceIndex) {
    auto devices = enumerateDevices();
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices.size())) {
        return false;
    }
    return openDevice(devices[deviceIndex]);
}

bool MidiInput::openDevice(const std::string& deviceName) {
    if (isOpen_) {
        closeDevice();
    }

#ifdef PAN_USE_ALSA_MIDI
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
        std::cerr << "Failed to open ALSA sequencer" << std::endl;
        return false;
    }
    
    snd_seq_set_client_name(seq, "Pan DAW");
    
    // Create input port
    int port = snd_seq_create_simple_port(seq, "MIDI Input",
                                          SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC);
    if (port < 0) {
        std::cerr << "Failed to create MIDI input port" << std::endl;
        snd_seq_close(seq);
        return false;
    }
    
    // Try to connect to the device

    // Parse device name (format: "Client Name:Port Name" or "hw:X (Device Name)")
    // First try ALSA sequencer format (Client:Port)
    bool found = false;
    size_t colonPos = deviceName.find(':');
    if (colonPos != std::string::npos && deviceName.find("hw:") == std::string::npos) {
        std::string clientName = deviceName.substr(0, colonPos);
        std::string portName = deviceName.substr(colonPos + 1);
        
        // Find client by name
        snd_seq_client_info_t* cinfo;
        snd_seq_port_info_t* pinfo;
        snd_seq_client_info_alloca(&cinfo);
        snd_seq_port_info_alloca(&pinfo);
        snd_seq_client_info_set_client(cinfo, -1);
        while (snd_seq_query_next_client(seq, cinfo) >= 0) {
            const char* name = snd_seq_client_info_get_name(cinfo);
            if (name && clientName == name) {
                int client = snd_seq_client_info_get_client(cinfo);
                snd_seq_port_info_set_client(pinfo, client);
                snd_seq_port_info_set_port(pinfo, -1);
                while (snd_seq_query_next_port(seq, pinfo) >= 0) {
                    const char* pname = snd_seq_port_info_get_name(pinfo);
                    if (pname && portName == pname) {
                        unsigned int caps = snd_seq_port_info_get_capability(pinfo);
                        if (caps & SND_SEQ_PORT_CAP_READ) {
                            int srcClient = snd_seq_port_info_get_client(pinfo);
                            int srcPort = snd_seq_port_info_get_port(pinfo);
                            snd_seq_connect_from(seq, port, srcClient, srcPort);
                            found = true;
                            break;
                        }
                    }
                }
                if (found) break;
            }
        }
        
        if (!found) {
            std::cerr << "Could not find MIDI device: " << deviceName << std::endl;
            std::cerr << "Trying to connect to all available MIDI devices..." << std::endl;
            // Fall through to connect-all code below
        }
    }
    
    // If device name parsing failed or we want to connect to all devices
    // Try to connect to all available MIDI output ports
    if (!found) {
        snd_seq_client_info_t* cinfo;
        snd_seq_port_info_t* pinfo;
        snd_seq_client_info_alloca(&cinfo);
        snd_seq_port_info_alloca(&pinfo);
        
        snd_seq_client_info_set_client(cinfo, -1);
        while (snd_seq_query_next_client(seq, cinfo) >= 0) {
            int client = snd_seq_client_info_get_client(cinfo);
            if (client == SND_SEQ_CLIENT_SYSTEM) continue;
            
            snd_seq_port_info_set_client(pinfo, client);
            snd_seq_port_info_set_port(pinfo, -1);
            while (snd_seq_query_next_port(seq, pinfo) >= 0) {
                unsigned int caps = snd_seq_port_info_get_capability(pinfo);
                if (caps & SND_SEQ_PORT_CAP_READ) {
                    int srcClient = snd_seq_port_info_get_client(pinfo);
                    int srcPort = snd_seq_port_info_get_port(pinfo);
                    snd_seq_connect_from(seq, port, srcClient, srcPort);
                }
            }
        }
    }
    
    sequencerHandle_ = seq;
    portId_ = port;
    deviceName_ = deviceName;
    isOpen_ = true;
    
    std::cout << "Opened MIDI input device: " << deviceName << std::endl;
    return true;
#else
    std::cerr << "MIDI input not supported on this platform" << std::endl;
    return false;
#endif
}

void MidiInput::closeDevice() {
    if (!isOpen_) {
        return;
    }
    
    stop();
    
#ifdef PAN_USE_ALSA_MIDI
    if (sequencerHandle_) {
        snd_seq_t* seq = static_cast<snd_seq_t*>(sequencerHandle_);
        if (portId_ >= 0) {
            snd_seq_delete_simple_port(seq, portId_);
        }
        snd_seq_close(seq);
        sequencerHandle_ = nullptr;
        portId_ = -1;
    }
#endif
    
    isOpen_ = false;
    deviceName_ = "";
}

bool MidiInput::start() {
    if (!isOpen_) {
        std::cerr << "Cannot start MIDI input: device not open" << std::endl;
        return false;
    }
    
    if (isRunning_) {
        return true;
    }
    
    shouldStop_ = false;
    midiThread_ = std::thread(&MidiInput::midiThreadFunction, this);
    isRunning_ = true;
    
    std::cout << "Started MIDI input" << std::endl;
    return true;
}

void MidiInput::stop() {
    if (!isRunning_) {
        return;
    }
    
    shouldStop_ = true;
    if (midiThread_.joinable()) {
        midiThread_.join();
    }
    isRunning_ = false;
    
    std::cout << "Stopped MIDI input" << std::endl;
}

void MidiInput::midiThreadFunction() {
#ifdef PAN_USE_ALSA_MIDI
    if (!sequencerHandle_) {
        return;
    }
    
    snd_seq_t* seq = static_cast<snd_seq_t*>(sequencerHandle_);
    
    // Set non-blocking mode
    snd_seq_nonblock(seq, 1);
    
    while (!shouldStop_) {
        snd_seq_event_t* ev = nullptr;
        int result = snd_seq_event_input(seq, &ev);
        
        if (result < 0) {
            if (result == -EAGAIN) {
                // No events available, sleep briefly
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            break;
        }
        
        if (ev && callback_) {
            // Convert ALSA event to MidiMessage
            if (ev->type == SND_SEQ_EVENT_NOTEON) {
                MidiMessage msg(MidiMessageType::NoteOn, 
                               ev->data.note.channel,
                               ev->data.note.note,
                               ev->data.note.velocity);
                callback_(msg);
            } else if (ev->type == SND_SEQ_EVENT_NOTEOFF) {
                MidiMessage msg(MidiMessageType::NoteOff,
                               ev->data.note.channel,
                               ev->data.note.note,
                               ev->data.note.velocity);
                callback_(msg);
            } else if (ev->type == SND_SEQ_EVENT_CONTROLLER) {
                MidiMessage msg(MidiMessageType::ControlChange,
                               ev->data.control.channel,
                               ev->data.control.param,
                               ev->data.control.value);
                callback_(msg);
            }
        }
        
        snd_seq_free_event(ev);
    }
#endif
}

} // namespace pan

