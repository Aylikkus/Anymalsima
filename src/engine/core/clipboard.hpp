#pragma once

#include <SDL3/SDL.h>
#include <string>

class Clipboard {
public:
    // Работаем напрямую с UTF-8
    static std::string getString() {
        char* text = SDL_GetClipboardText();
        if (!text) return "";
        
        std::string result(text);
        SDL_free(text); // Обязательно освобождаем память
        return result;
    }

    static void setString(const std::string& text) {
        SDL_SetClipboardText(text.c_str());
    }
};
