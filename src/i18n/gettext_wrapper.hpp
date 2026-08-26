#pragma once
#include <string>
#include <locale>
#include <fmt/core.h>

#define _(msgid) i18n::tr(msgid)
#define _F(msgid, ...) i18n::tr_format(msgid, __VA_ARGS__)
#define __(msgid) msgid

namespace i18n
{
    void init(const std::string& localeDir);

    void setLanguage(const std::string& locale); // e.g. "en_US", "fr_FR"
    std::string getLanguage();

    std::string tr(const char* msgid);
    std::string tr(const std::string& msgid);

    template <typename... Args>
    std::string tr_format(const char* msgid, Args&&... args) {
        // Сначала переводим, потом форматируем через runtime
        return fmt::format(fmt::runtime(i18n::tr(msgid)), std::forward<Args>(args)...);
    }
}