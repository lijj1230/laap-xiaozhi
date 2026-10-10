# LAAP-lite 意识架构组件（移植自 laap-esp32 固件，见 docs/consciousness_integration.md）
# 纯本地意识：认知/记忆/技能/规则/心跳（无网络栈——AI 客户端全归 xiaozhi 云，
# 设备侧自主说话经宿主 listen-detect 注入；语义向量已删，召回走关键词通道）
idf_component_register(
    SRCS "laap_skills.cc"
         "laap_fs.cc"
         "laap_cognition.cc"
         "laap_rules.cc"
         "laap_util.cc"
         "laap_memory.cc"
         "laap_boot.cc"
         "laap_config.cc"
         "laap_life.cc"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_timer joltwallet__littlefs espressif__cjson esp_wifi
)
