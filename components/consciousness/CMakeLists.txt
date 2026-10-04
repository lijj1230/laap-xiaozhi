# LAAP-lite 意识架构组件（移植自 laap-esp32 固件，见 docs/consciousness_integration.md）
# M1：cognition/skills/config 骨架；M2 起逐模块补齐 memory/llm/tts/life
idf_component_register(
    SRCS "laap_skills.cc"
         "laap_fs.cc"
         "laap_cognition.cc"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer json esp_littlefs
)
