#include <fstream>
#include <string>

#include "core/settings.hpp"
#include "game.hpp"
#include "gettext_wrapper.hpp"
#include "platform.hpp"
#include "renderer.hpp"
#include "serialization/file_io.hpp"
#include "utils/items_enum_maps.hpp"
#include "utils/region_enum_maps.hpp"
#include "world/world.hpp"

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

// Track memory usage. Currently works only on Linux
// #define TRACK_MEM_USAGE

#ifdef TRACK_MEM_USAGE
// Quick and dirty self-reporting
void printMemUsage() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("VmRSS:") || line.starts_with("VmPeak:") ||
            line.starts_with("VmSwap:") || line.starts_with("RssAnon:") ||
            line.starts_with("RssFile:") || line.starts_with("RssShmem:"))
            std::cout << line << "\n";
    }
    std::cout << "\n";
}
#endif

int main() {
    i18n::init("assets/i18n");

    Platform platform;
    Renderer renderer(platform);
    renderer.getResources().loadFromJson(
        FileIO::readJson("assets/definitions.json"));
    renderer.getResources().buildMaterialTextureArrays();

    InputSystem input;
    RenderTarget renderTarget;

    Settings settings;

    i18n::setLanguage(settings.getLanguage());

    UIContext context(platform.getWindowSize(), &renderer,
                      &renderer.getAudioSystem(), &platform,
                      &renderer.getResources());
    context.playerSettings = &settings;

    context.globalScale = settings.getUiScale();
    renderer.getAudioSystem().setMasterVolume(settings.getMasterVolume());
    renderer.getAudioSystem().setMusicVolume(settings.getMusicVolume());
    renderer.getAudioSystem().setSoundVolume(settings.getSoundVolume());

    platform.onResize.connect(
        [&context](Vec2 newSize) { context.setWindowSize(newSize); });

    settings.onUiScaleChanged.connect(
        [&context](float newScale) { context.globalScale = newScale; });

    settings.onMasterVolumeChanged.connect([&renderer](float newMasterV) {
        renderer.getAudioSystem().setMasterVolume(newMasterV);
    });

    settings.onMusicVolumeChanged.connect([&renderer](float newMusicV) {
        renderer.getAudioSystem().setMusicVolume(newMusicV);
    });

    settings.onSoundVolumeChanged.connect([&renderer](float newSoundV) {
        renderer.getAudioSystem().setSoundVolume(newSoundV);
    });

    settings.onLanguageChanged.connect([](const std::string& newl) {
        i18n::setLanguage(newl);
        RegionEnumMaps::buildLocalized();
        ItemsEnumMaps::buildLocalized();
    });

    settings.loadIfExists("settings.json", true);

    Game game(context);
    
#ifdef TRACK_MEM_USAGE
    float mem_usage_timer = 0.0f;
#endif

    platform.setFpsLimit(0);

    while (platform.isRunning()) {
        float deltaTime = platform.deltaTime();

        platform.pollEvents(input);

        for (const InputEvent& e : input.getEvents()) {
            game.handleInput(e, context, deltaTime);
        }

        game.update(deltaTime, context);
        game.render(renderTarget, context);

        renderer.beginFrame();
        renderer.draw(renderTarget);
        renderer.endFrame();

        renderer.getAudioSystem().update();
        game.frameEnd();
        input.clear();
        renderTarget.clear();

#ifdef TRACK_MEM_USAGE
        mem_usage_timer += deltaTime;
        if (mem_usage_timer > 0.5f) {
            printMemUsage();
            mem_usage_timer = 0.0f;
        }
#endif

#ifdef TRACY_ENABLE
        FrameMark;
#endif
    }

    game.close();
    renderer.clear();
    platform.close();

    return 0;
}