# LAAP-lite 意识架构组件（移植自 laap-esp32 固件，见 docs/consciousness_integration.md）
# M1：cognition/skills/config 骨架；M2 起逐模块补齐 memory/llm/tts/life
# M1+M2a：skills + fs 垫片 + cognition 认知核心；M2b 起加 laap_memory.cc / laap_llm.cc / laap_life.cc
idf_component_register(
    SRCS ${CMAKE_CURRENT_LIST_DIR}/libhelix/bitstream.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/buffers.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/dct32.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/dequant.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/dqchan.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/huffman.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/hufftabs.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/imdct.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/mp3dec.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/mp3tabs.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/polyphase.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/scalfact.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/stproc.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/subband.c
         ${CMAKE_CURRENT_LIST_DIR}/libhelix/trigtabs.c
         "laap_skills.cc"
         "laap_fs.cc"
         "laap_cognition.cc"
         "laap_rules.cc"
         "laap_util.cc"
         "laap_memory.cc"
         "laap_boot.cc"
         "laap_config.cc"
         "laap_llm.cc"
         "laap_search.cc"
         "laap_tts.cc"
         "laap_life.cc"
    INCLUDE_DIRS "." ${CMAKE_CURRENT_LIST_DIR}/libhelix
    REQUIRES nvs_flash esp_timer joltwallet__littlefs espressif__cjson esp_http_client lwip mbedtls espressif__esp_websocket_client esp_wifi esp_netif
)

# libhelix 平台识别（assembly.h/mp3dec.h 的 ARDUINO 分支=通用 GCC 路径）
add_compile_definitions(ARDUINO=100 PROGMEM=)   # IDF6 无 pgmspace：PROGMEM 置空=普通 const
