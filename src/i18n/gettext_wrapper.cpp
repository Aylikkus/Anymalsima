#include "gettext_wrapper.hpp"

#include <libintl.h>
#include <locale.h>
#include <string>

namespace
{
    std::string g_currentLang = "en_US";
}

namespace i18n
{
    void init(const std::string& localeDir)
    {
        setlocale(LC_ALL, "");

        bindtextdomain("game", localeDir.c_str());
        textdomain("game");

        bind_textdomain_codeset("game", "UTF-8");
    }

    void setLanguage(const std::string& locale)
    {
        g_currentLang = locale;

        setenv("LANG", locale.c_str(), 1);
        setenv("LC_ALL", locale.c_str(), 1);

        setlocale(LC_ALL, locale.c_str());
    }

    std::string getLanguage()
    {
        return g_currentLang;
    }

    std::string tr(const char *msgid)
    {
        const char* utf8 = gettext(msgid);

        return std::string(utf8);
    }
    std::string tr(const std::string &msgid)
    {
        return tr(msgid.c_str());
    }
}