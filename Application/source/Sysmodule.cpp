#include <cstring>
#include <limits>
#include "Log.hpp"
#include "Protocol.hpp"
#include "Sysmodule.hpp"
#include "utils/NX.hpp"

// Program ID of sysmodule
#define PROGRAM_ID 0x4200000000000FFF

// Macro for adding delimiter
#define DELIM std::string(1, Protocol::Delimiter)
// Number of seconds between updating state (automatically)
#define UPDATE_DELAY 0.1

bool Sysmodule::addToWriteQueue(std::string s, std::function<void(std::string)> f) {
    if (this->error_ != Error::None) {
        return false;
    }

    std::scoped_lock<std::mutex> mtx(this->writeMutex);
    this->writeQueue.push(std::pair< std::string, std::function<void(std::string)> >(s, f));
    return true;
}

Sysmodule::Sysmodule() {
    // Attempt connection on creation
    this->connector = new Socket::Connector(Protocol::Port);
    this->connector->setTimeout(Protocol::Timeout);
    this->error_ = Error::Unknown;
    this->limit_ = -1;
    this->socket = nullptr;
    this->reconnect();

    // Initialize all variables
    this->currentSong_ = -1;
    this->exit_ = false;
    this->lastUpdateTime = std::chrono::steady_clock::now();
    this->playingFrom_ = "";
    this->position_ = 0.0;
    this->queueChanged_ = false;
    this->queueSize_ = 0;
    this->repeatMode_ = RepeatMode::Off;
    this->shuffleMode_ = ShuffleMode::Off;
    this->songIdx_ = 0;
    this->subQueueChanged_ = false;
    this->status_ = PlaybackStatus::Stopped;
    this->volume_ = 100.0;

    // Fetch queue at launch
    this->sendGetQueue(0, 25000);
    this->sendGetSubQueue(0, 5000);
}

Sysmodule::Error Sysmodule::error() {
    return this->error_;
}

bool Sysmodule::launch() {
    return Utils::NX::launchProgram(PROGRAM_ID);
}

void Sysmodule::reconnect() {
    std::scoped_lock<std::mutex> mtx(this->writeMutex);

    // Delete old socket and get a new one
    delete this->socket;
    this->socket = this->connector->getTransferSocket();

    // Check we're actually connected before proceeding
    if (this->socket == nullptr || !this->socket->isConnected()) {
        this->error_ = Error::NotConnected;
        Log::writeError("[SYSMODULE] Unable to connect to sysmodule");
        return;
    }

    // Check the protocol version next and make sure it matches
    std::string message = std::to_string((int)Protocol::Command::Version);
    this->socket->writeMessage(message);
    std::string reply = this->socket->readMessage();
    if (reply.length() > 0) {
        int version = std::stoi(reply);
        if (version != Protocol::Version) {
            Log::writeError("[SYSMODULE] Versions do not match! Sysmodule: " + reply + ", Application: " + message);
            this->error_ = Error::DifferentVersion;
            return;
        }

    // If the response is empty some other unknown error occurred
    } else {
        Log::writeError("[SYSMODULE] Unable to get sysmodule version");
        this->error_ = Error::Unknown;
        return;
    }

    // If we reach here we're connected successfully!
    Log::writeSuccess("[SYSMODULE] Connection established!");
    this->error_ = Error::None;
}

bool Sysmodule::terminate() {
    bool ok = Utils::NX::terminateProgram(PROGRAM_ID);
    if (ok) {
        this->error_ = Error::LostConnection;
    }
    return ok;
}

void Sysmodule::setQueueLimit(int limit) {
    this->limit_ = limit;
}

void Sysmodule::process() {
    // Loop until we want to exit
    while (!this->exit_) {
        // Sleep if an error occurred (this way if the app asks it to reconnect it will continue communicating)
        if (this->error_ != Error::None) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // First process commands on the write queue
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> mtx(this->writeMutex);
        while (!this->writeQueue.empty()) {
            // Get the first message on the queue
            std::string message = this->writeQueue.front().first;
            if (!this->socket->writeMessage(message)) {
                this->error_ = Error::LostConnection;

            // If the write succeeded wait for and read the response
            } else {
                std::string str = this->socket->readMessage();
                if (str.length() == 0) {
                    this->error_ = Error::LostConnection;

                // If we successfully received a response call the associated function and pop the
                // message from the queue
                } else {
                    std::function<void(std::string)> queuedFunc = this->writeQueue.front().second;
                    mtx.unlock();
                    queuedFunc(str);
                    mtx.lock();
                    this->writeQueue.pop();
                }
            }

            // Clear queue if an error occurred
            if (this->error_ != Error::None) {
                Log::writeError("[SYSMODULE] Command queue cleared as an error occurred during processing");
                this->writeQueue = std::queue< std::pair<std::string, std::function<void(std::string)> > >();
                continue;
            }
        }
        mtx.unlock();

        // Check if we need to log to save cycles
        if (Log::loggingLevel() == Log::Level::Info) {
            Log::writeInfo("[SYSMODULE] Update took: " + std::to_string(std::chrono::duration_cast< std::chrono::duration<double> >(std::chrono::steady_clock::now() - now).count()) + " seconds");
        }

        // Check if variables need to be updated
        now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast< std::chrono::duration<double> >(now - this->lastUpdateTime).count() > UPDATE_DELAY) {
            this->sendGetPlayingFrom();
            this->sendGetPosition();
            this->sendGetQueueSize();
            this->sendGetRepeat();
            this->sendGetShuffle();
            this->sendGetSong();
            this->sendGetSongIdx();
            this->sendGetSubQueueSize();
            this->sendGetStatus();
            this->sendGetVolume();
            this->lastUpdateTime = now;

        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

SongID Sysmodule::currentSong() {
    return this->currentSong_;
}

std::string Sysmodule::playingFrom() {
    std::scoped_lock<std::mutex> mtx(this->playingFromMutex);
    // If the string contains a space it's "empty"
    return (this->playingFrom_ == " " ? "" : this->playingFrom_);
}

double Sysmodule::position() {
    return this->position_;
}

bool Sysmodule::queueChanged() {
    if (this->queueChanged_) {
        this->queueChanged_ = false;
        return true;
    }

    return false;
}

std::vector<SongID> Sysmodule::queue() {
    std::scoped_lock<std::mutex> mtx(this->queueMutex);
    return this->queue_;
}

size_t Sysmodule::queueSize() {
    return this->queueSize_;
}

RepeatMode Sysmodule::repeatMode() {
    return this->repeatMode_;
}

ShuffleMode Sysmodule::shuffleMode() {
    return this->shuffleMode_;
}

size_t Sysmodule::songIdx() {
    return this->songIdx_;
}

bool Sysmodule::subQueueChanged() {
    if (this->subQueueChanged_) {
        this->subQueueChanged_ = false;
        return true;
    }

    return false;
}

std::vector<SongID> Sysmodule::subQueue() {
    std::scoped_lock<std::mutex> mtx(this->subQueueMutex);
    return this->subQueue_;
}

size_t Sysmodule::subQueueSize() {
    return this->subQueueSize_;
}

PlaybackStatus Sysmodule::status() {
    return this->status_;
}

double Sysmodule::volume() {
    return this->volume_;
}

bool Sysmodule::waitRequestDBLock() {
    std::atomic<bool> done = false;

    this->addToWriteQueue(std::to_string((int)Protocol::Command::RequestDBLock), [&done](std::string s) {
        if (std::stoi(s) != 0) {
            // throw error
        }
        done = true;
    });

    // Block until done
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (this->error_ != Error::None) {
            return false;
        }
    }
    return true;
}

bool Sysmodule::waitReset() {
    std::atomic<bool> done = false;

    this->addToWriteQueue(std::to_string((int)Protocol::Command::Reset), [&done](std::string s) {
        done = true;
    });

    // Block until done
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (this->error_ != Error::None) {
            return false;
        }
    }

    return true;
}

size_t Sysmodule::waitSongIdx() {
    std::atomic<bool> done = false;

    // Query song idx
    this->addToWriteQueue(std::to_string((int)Protocol::Command::QueueIdx), [this, &done](std::string s) {
        this->songIdx_ = std::stoi(s);
        done = true;
    });

    // Block until done
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (this->error_ != Error::None) {
            return std::numeric_limits<size_t>::max();
            break;
        }
    }

    return this->songIdx_;
}

void Sysmodule::sendResume() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::Resume), [this](std::string s) {
        this->currentSong_ = std::stoi(s);
    });
}

void Sysmodule::sendPause() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::Pause), [this](std::string s) {
        this->currentSong_ = std::stoi(s);
    });
}

void Sysmodule::sendPrevious() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::Previous), [this](std::string s) {
        if (std::stoi(s) != 0) {
            // Handle error
        }
    });
}

void Sysmodule::sendNext() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::Next), [this](std::string s) {
        if (std::stoi(s) != 0) {
            // Handle error
        }
    });
}

void Sysmodule::sendGetVolume() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::GetVolume), [this](std::string s) {
        this->volume_ = std::stod(s);
    });
}

void Sysmodule::sendSetVolume(const double v) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::SetVolume) + DELIM + std::to_string(v), [this](std::string s) {
        this->volume_ = std::stod(s);
    });
}

void Sysmodule::sendMute() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::Mute), [this](std::string s) {
        this->volume_ = std::stod(s);
    });
}

void Sysmodule::sendUnmute() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::Unmute), [this](std::string s) {
        this->volume_ = std::stod(s);
    });
}

void Sysmodule::sendSetSongIdx(const size_t id) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::SetQueueIdx) + DELIM + std::to_string(id), [this](std::string s) {
        this->songIdx_ = std::stoi(s);
    });
}

void Sysmodule::sendGetSongIdx() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::QueueIdx), [this](std::string s) {
        size_t tmp = std::stoul(s);

        // Update queues if size changes
        if (this->songIdx_ != tmp) {
            this->sendGetQueue(0, 25000);
            this->sendGetSubQueue(0, 5000);
        }

        this->songIdx_ = tmp;
    });
}

void Sysmodule::sendGetQueueSize() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::QueueSize), [this](std::string s) {
        size_t tmp = std::stoul(s);

        // Update queue if size changes
        if (this->queueSize_ != tmp) {
            this->sendGetQueue(0, 25000);
        }

        this->queueSize_ = tmp;
    });
}

void Sysmodule::sendRemoveFromQueue(const size_t pos) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::RemoveFromQueue) + DELIM + std::to_string(pos), [this, pos](std::string s) {
        if (std::stoul(s) != pos) {
            // Error message here
        }
    });
}

void Sysmodule::sendGetQueue(const size_t s, const size_t e) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::GetQueue) + DELIM + std::to_string(s) + DELIM + std::to_string(e), [this](std::string s) {
        // Add each token in string
        std::scoped_lock<std::mutex> mtx(this->queueMutex);
        this->queue_.clear();
        char * str = strdup(s.c_str());
        char * tok = strtok(str, &Protocol::Delimiter);
        while (tok != nullptr) {
            this->queue_.push_back(strtol(tok, nullptr, 10));
            tok = strtok(nullptr, &Protocol::Delimiter);
        }
        free(str);
        this->queueChanged_ = true;
    });
}

void Sysmodule::sendSetQueue(const std::vector<SongID> & q) {
    if (q.size() == 0 || this->limit_ == 0) {
        return;
    }

    // Construct string first
    std::string seq;
    size_t size = q.size();
    bool hasLimit = (this->limit_ >= 0);
    for (size_t i = 0; i < size; i++) {
        // Stop if we reach the config limit
        if (hasLimit && i >= (size_t)this->limit_) {
            break;
        }

        seq.push_back(Protocol::Delimiter);
        seq += std::to_string(q[i]);
    }

    this->addToWriteQueue(std::to_string((int)Protocol::Command::SetQueue) + seq, [this, size](std::string s) {
        if (std::stoul(s) != size) {
            // Error message here
        }
    });
}

void Sysmodule::sendAddToSubQueue(const SongID id) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::AddToSubQueue) + DELIM + std::to_string(id), [this, id](std::string s) {
        if (std::stoi(s) != id) {
            // Error message here
        }
    });
}

void Sysmodule::sendRemoveFromSubQueue(const size_t pos) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::RemoveFromSubQueue) + DELIM + std::to_string(pos), [this, pos](std::string s) {
        if (std::stoul(s) != pos) {
            // Error message here
        }
    });
}

void Sysmodule::sendGetSubQueueSize() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::SubQueueSize), [this](std::string s) {
        size_t tmp = std::stoul(s);

        // Update queue if size changes
        if (this->subQueueSize_ != tmp) {
            this->sendGetSubQueue(0, 5000);
        }

        this->subQueueSize_ = tmp;
    });
}

void Sysmodule::sendGetSubQueue(const size_t s, const size_t e) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::GetSubQueue) + DELIM + std::to_string(s) + DELIM + std::to_string(e), [this](std::string s) {
        // Add each token in string
        std::scoped_lock<std::mutex> mtx(this->subQueueMutex);
        this->subQueue_.clear();
        char * str = strdup(s.c_str());
        char * tok = strtok(str, &Protocol::Delimiter);
        while (tok != nullptr) {
            this->subQueue_.push_back(strtol(tok, nullptr, 10));
            tok = strtok(nullptr, &Protocol::Delimiter);
        }
        free(str);
        this->subQueueChanged_ = true;
    });
}

void Sysmodule::sendSkipSubQueueSongs(const size_t n) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::SkipSubQueueSongs) + DELIM + std::to_string(n), [this, n](std::string s) {
        if (std::stoul(s) != n) {
            // Error message here
        }
    });
}

void Sysmodule::sendGetRepeat() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::GetRepeat), [this](std::string s) {
        RepeatMode rm = RepeatMode::Off;
        switch ((Protocol::Repeat)std::stoi(s)) {
            case Protocol::Repeat::Off:
                rm = RepeatMode::Off;
                break;

            case Protocol::Repeat::One:
                rm = RepeatMode::One;
                break;

            case Protocol::Repeat::All:
                rm = RepeatMode::All;
                break;
        }
        this->repeatMode_ = rm;
    });
}

void Sysmodule::sendSetRepeat(const RepeatMode m) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::SetRepeat) + DELIM + std::to_string((int)m), [this, m](std::string s) {
        RepeatMode rm;
        switch ((Protocol::Repeat)std::stoi(s)) {
            case Protocol::Repeat::Off:
                rm = RepeatMode::Off;
                break;

            case Protocol::Repeat::One:
                rm = RepeatMode::One;
                break;

            case Protocol::Repeat::All:
                rm = RepeatMode::All;
                break;
        }

        if (rm != m) {
            // Handle error here

        } else {
            this->repeatMode_ = rm;
        }
    });
}

void Sysmodule::sendGetShuffle() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::GetShuffle), [this](std::string s) {
        this->shuffleMode_ = ((Protocol::Shuffle)std::stoi(s) == Protocol::Shuffle::Off ? ShuffleMode::Off : ShuffleMode::On);
    });
}

void Sysmodule::sendSetShuffle(const ShuffleMode m) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::SetShuffle) + DELIM + std::to_string((int)m), [this, m](std::string s) {
        ShuffleMode sm = ((Protocol::Shuffle)std::stoi(s) == Protocol::Shuffle::Off ? ShuffleMode::Off : ShuffleMode::On);
        if (sm != m) {
            // Error message here
        }

        this->sendGetQueue(0, 25000);
        this->shuffleMode_ = sm;
    });
}

void Sysmodule::sendGetSong() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::GetSong), [this](std::string s) {
        this->currentSong_ = std::stoi(s);
    });
}

void Sysmodule::sendGetStatus() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::GetStatus), [this](std::string s) {
        PlaybackStatus ps = PlaybackStatus::Error;
        switch ((Protocol::Status)std::stoi(s)) {
            case Protocol::Status::Error:
                ps = PlaybackStatus::Error;
                break;

            case Protocol::Status::Playing:
                ps = PlaybackStatus::Playing;
                break;

            case Protocol::Status::Paused:
                ps = PlaybackStatus::Paused;
                break;

            case Protocol::Status::Stopped:
                ps = PlaybackStatus::Stopped;
                break;
        }
        this->status_ = ps;
    });
}

void Sysmodule::sendGetPosition() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::GetPosition), [this](std::string s) {
        this->position_ = std::stod(s);
    });
}

void Sysmodule::sendSetPosition(double pos) {
    this->position_ = pos;
    this->addToWriteQueue(std::to_string((int)Protocol::Command::SetPosition) + DELIM + std::to_string(pos), [this](std::string s) {
        this->position_ = std::stod(s);
    });
}

void Sysmodule::sendGetPlayingFrom() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::GetPlayingFrom), [this](std::string s) {
        std::scoped_lock<std::mutex> mtx(this->playingFromMutex);
        this->playingFrom_ = s;
    });
}

void Sysmodule::sendSetPlayingFrom(const std::string & str) {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::SetPlayingFrom) + DELIM + str, [this](std::string s) {
        std::scoped_lock<std::mutex> mtx(this->playingFromMutex);
        this->playingFrom_ = s;
    });
}

void Sysmodule::sendReleaseDBLock() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::ReleaseDBLock), [this](std::string s) {
        if (std::stoi(s) != 0) {
            // throw error
        }
    });
}

void Sysmodule::sendReloadConfig() {
    this->addToWriteQueue(std::to_string((int)Protocol::Command::ReloadConfig), [this](std::string s) {
        if (std::stoi(s) != 0) {
            // throw error
        }
    });
}

void Sysmodule::exit() {
    this->exit_ = true;
}

Sysmodule::~Sysmodule() {
    delete this->connector;
    delete this->socket;
}