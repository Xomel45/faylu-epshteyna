cmake_minimum_required(VERSION 3.16)
project(faylu-epshteyna VERSION 5.2 LANGUAGES C)

set(CMAKE_C_STANDARD 23)

add_compile_options(-Wall -Wextra -O2 -Wno-format-truncation)

if(WIN32)
    set(CURL_USE_STATIC_LIBS ON)
    set(OPENSSL_USE_STATIC_LIBS ON)
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

set(SOURCES
    main.c
    http.c
    files.c
    dl.c
    extractors/pornhub.c
    extractors/vk.c
    extractors/xvideos.c
    extractors/xhamster.c
    extractors/xnxx.c
    extractors/eporner.c
    extractors/redtube.c
    extractors/sex_studentki.c
    extractors/tiktok.c
    extractors/yandex_video.c
    extractors/bunkr.c
    extractors/ebalko.c
    extractors/pornonahobka.c
    extractors/thothub.c
    vendor/cJSON.c
)

add_executable(faylu-epshteyna ${SOURCES})

target_include_directories(faylu-epshteyna PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/vendor
)

target_compile_definitions(faylu-epshteyna PRIVATE
    APP_VERSION="V${PROJECT_VERSION}"
)

target_link_libraries(faylu-epshteyna PRIVATE
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    m
)

if(WIN32)
    target_link_options(faylu-epshteyna PRIVATE -static)
    target_link_libraries(faylu-epshteyna PRIVATE
        ws2_32 wldap32 crypt32 normaliz bcrypt
    )
endif()
