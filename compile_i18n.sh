#!/bin/bash

mkdir -p assets/i18n/ru_RU/LC_MESSAGES/
msgfmt translations/ru.po -o assets/i18n/ru_RU/LC_MESSAGES/game.mo
mkdir -p assets/i18n/ja_JP/LC_MESSAGES/
msgfmt translations/ja.po -o assets/i18n/ja_JP/LC_MESSAGES/game.mo