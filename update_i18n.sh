#!/bin/bash

xgettext -k_ -k__ -o translations/game.pot src/**/**/*.cpp src/**/*.cpp src/**/**/*.hpp src/**/*.hpp

# msginit --input=translations/game.pot --locale=en_EN --output-file=en.po --no-translator
# msginit --input=translations/game.pot --locale=ru_RU --output-file=ru.po --no-translator
# msginit --input=translations/game.pot --locale=ja_JP --output-file=ja.po --no-translator

msgmerge --update translations/en.po translations/game.pot
msgmerge --update translations/ru.po translations/game.pot
msgmerge --update translations/ja.po translations/game.pot