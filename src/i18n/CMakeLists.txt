add_library(i18n STATIC
    gettext_wrapper.cpp
)
    
find_package(Intl REQUIRED)
target_include_directories(i18n PUBLIC ${Intl_INCLUDE_DIRS})

target_include_directories(i18n
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(i18n PUBLIC Intl::Intl fmt)