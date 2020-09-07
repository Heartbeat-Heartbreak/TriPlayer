#include "Application.hpp"
#include "ui/screen/Fullscreen.hpp"
#include "ui/screen/Home.hpp"
#include "ui/screen/Settings.hpp"
#include "ui/screen/Splash.hpp"
#include "utils/NX.hpp"

// Path to config file
#define APP_CONFIG_PATH "/config/TriPlayer/app_config.ini"

namespace Main {
    Application::Application() : database_(SyncDatabase(new Database())) {
        // Load config
        this->config_ = new Config(APP_CONFIG_PATH);
        this->database_->setSpellfixScore(this->config_->searchMaxScore());
        this->database_->setSearchPhraseCount(this->config_->searchMaxPhrases());

        // Prepare theme
        this->theme_ = new Theme();

        // Create sysmodule object (will attempt connection)
        this->sysmodule_ = new Sysmodule();
        // Continue in another thread
        this->sysThread = std::async(std::launch::async, &Sysmodule::process, this->sysmodule_);

        // Create Aether instance
        Aether::ThreadPool::setMaxThreads(8);
        this->display = new Aether::Display();
        this->display->setBackgroundColour(0, 0, 0);
        this->display->setFont("romfs:/Quicksand.ttf");
        this->display->setFontSpacing(0.9);
        this->display->setHighlightColours(Aether::Colour{255, 255, 255, 0}, this->theme_->selected());
        this->setHighlightAnimation(nullptr);
        this->display->setFadeIn();
        this->display->setShowFPS(true);

        // Setup screens
        this->screens[static_cast<int>(ScreenID::Fullscreen)] = new Screen::Fullscreen(this);
        this->screens[static_cast<int>(ScreenID::Home)] = new Screen::Home(this);
        this->screens[static_cast<int>(ScreenID::Settings)] = new Screen::Settings(this);
        this->screens[static_cast<int>(ScreenID::Splash)] = new Screen::Splash(this);
        this->setScreen(ScreenID::Splash);

        // Mark that we're playing media
        Utils::NX::setPlayingMedia(true);
    }

    void Application::setHoldDelay(int i) {
        this->display->setHoldDelay(i);
    }

    void Application::setHighlightAnimation(std::function<Aether::Colour(uint32_t)> f) {
        // Set to default animation
        if (f == nullptr) {
            f = this->theme_->highlightFunc();
        }
        this->display->setHighlightAnimation(f);
    }

    void Application::addOverlay(Aether::Overlay * o) {
        this->display->addOverlay(o);
    }

    void Application::setScreen(ScreenID s) {
        this->display->setScreen(this->screens[static_cast<int>(s)]);
    }

    void Application::pushScreen() {
        this->display->pushScreen();
    }

    void Application::popScreen() {
        this->display->popScreen();
    }

    void Application::dropScreen() {
        this->display->dropScreen();
    }

    void Application::updateScreenTheme() {
        for (Screen::Screen * s : this->screens) {
            s->updateColours();
        }
    }

    void Application::lockDatabase() {
        this->database_->close();
        this->sysmodule_->waitRequestDBLock();
        this->database_->openReadWrite();
    }

    void Application::unlockDatabase() {
        this->database_->close();
        this->sysmodule_->sendReleaseDBLock();
        this->database_->openReadOnly();
    }

    Config * Application::config() {
        return this->config_;
    }

    const SyncDatabase & Application::database() {
        return this->database_;
    }

    Sysmodule * Application::sysmodule() {
        return this->sysmodule_;
    }

    Theme * Application::theme() {
        return this->theme_;
    }

    void Application::run() {
        // Do main loop
        while (this->display->loop()) {
            // Reconnect to sysmodule if we've lost connection
            if (this->sysmodule_->error() == Sysmodule::Error::LostConnection) {
                this->sysmodule_->reconnect();
            }
        }
    }

    void Application::exit() {
        this->display->exit();
    }

    Application::~Application() {
        // Mark that we're no longer playing media
        Utils::NX::setPlayingMedia(false);

        // Delete screens
        for (Screen::Screen * s : this->screens) {
            delete s;
        }

        // Cleanup Aether after screens are deleted
        delete this->display;

        // Disconnect from sysmodule
        this->sysmodule_->exit();
        this->sysThread.get();
        delete this->sysmodule_;

        // Cleanup config object
        delete this->config_;

        // The database will be closed here as the wrapper goes out of scope
    }
};