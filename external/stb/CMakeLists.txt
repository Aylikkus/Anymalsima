# Создаем цель для stb_image
add_library(stb STATIC 
    stb_image.h 
    stb_image.c
    stb_truetype.h 
    stb_truetype.c
)

target_include_directories(stb
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)