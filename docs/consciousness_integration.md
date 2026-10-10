# LAAP 意识架构移植到 xiaozhi-esp32 —— 集成设计

> 2026-10-04 立项。目标：小智的音频前端（AFE 降噪/回声消除/唤醒/opus/麦克风喇叭驱动）+ 我们 LAAP-lite 的意识架构（设备侧大脑）。
> 板子：立创·实战派 ESP32-S3（xiaozhi 官方支持 `main/boards/lckfb/szpi-esp32s3`）。
> 编译要求：ESP-IDF v6.1（本仓库已不支持 5.x）。

## 0. 分期策略（已定）

- **一期（本仓库当前工作）**：意识组件在设备侧立起来——需求/情绪引擎、记忆沉淀、口令技能/规则、自主表达与独白（设备侧自己的 LLM=DeepSeek + TTS=Edge，通过小智喇叭发声）。主对话仍走小智云（其音质与识别是我们的身体红利）。
- **二期（可选）**：主对话切全自研脑（硅基流动 ASR + DeepSeek + EdgeTTS），记忆/人设/技能完整注入主对话。一期完成后再评估。

## 1. 两边架构对照与取舍

| 能力 | LAAP-lite（现固件） | xiaozhi-esp32 | 移植决策 |
|---|---|---|---|
| 音频前端 | 自建 I2S+VAD | AFE（AEC/NS/VAD）+opus | **用小智的** |
| 唤醒 | esp-sr 窗口模式（曾因堆回退） | esp-sr 常驻（其内存布局容纳） | **用小智的** |
| ASR/LLM/TTS 主对话 | 设备侧直连 API | 云端服务器 | 一期**保持云端**；二期切设备侧 |
| 意识（需求/情绪/进化） | laap_cognition 529 行 | 无 | **移植**（std::string 化） |
| 记忆（episodes/语义/向量/工作环） | laap_memory 1156 行 | 无 | **移植**（LittleFS→esp_littlefs） |
| 口令技能/行为规则 | laap_skills/rules | 无 | **移植** |
| 自主表达/独白 | arisExpress/arisIdleMonologue | 无 | **移植**（FreeRTOS 任务） |
| 设备侧 LLM | laap_llm（keep-alive 槽+SSE） | 无（云对话） | **移植**（esp_http_client） |
| 设备侧 TTS | laap_edge_tts+libhelix | 云 TTS 音频下发 | **移植**（v1 复用 libhelix+esp_websocket_client） |
| Web 后台 | laap_web | 有 web 框架痕迹 | 后期可选 |
| 堆纪律 | 45KB/40KB/30KB 门禁+panic RTC 黑匣子 | 其音频管线自有内存纪律 | **门禁全部保留**（两套内存大户并存，纪律是生命线） |

## 2. 数据分区（必须改）

`partitions/v2/16M.csv` 的 assets(8M spiffs) 拆出意识数据区：

```
assets,      data, spiffs,  0x800000,  6M
conscious,   data, littlefs,0xE00000,  2M
```

（小智 assets 实际占用需在首次编译后核对；不够再压。）

## 3. 组件结构（新增 `main/consciousness/`）

```
main/consciousness/
├── CMakeLists.txt            # 组件注册（idf_component_register）
├── laap_cognition.{h,cc}     # 需求/情绪/信任/预期/意图/进化（移植自 laap_cognition，529 行）
├── laap_memory.{h,cc}        # episodes/语义/工作环/向量（移植自 laap_memory，1156 行）
├── laap_skills.{h,cc}        # 口令技能 12 槽（117 行）
├── laap_rules.{h,cc}         # 行为规则（95 行）
├── laap_config.{h,cc}        # NVS 配置（api 指向/key/调校参数）
├── laap_llm.{h,cc}           # DeepSeek 直连（esp_http_client，SSE 流式；一期不做 keep-alive 槽）
├── laap_search.{h,cc}        # 必应 RSS→DDG 三源（预算 25s 保留）
├── laap_tts.{h,cc}           # Edge TTS（esp_websocket_client）+ libhelix（从 LAAP 平移）
├── laap_life.{h,cc}          # 意识心跳：PSI tick + 表达/独白调度（FreeRTOS 任务，10s 拍）
└── laap_gate.h               # 堆门禁（45/40/30KB 三道，v3.76h 原样带来——两套内存大户并存的纪律）
```

### API 语义迁移表（Arduino → IDF）

| Arduino | IDF 对应 |
|---|---|
| `String` | `std::string` |
| `Preferences` (NVS) | `nvs_open/nvs_set_str` |
| `LittleFS` | `esp_littlefs`（conscious 分区） |
| `Serial.printf` | `ESP_LOGI/ESP_LOGW` |
| `xTaskCreate` (Arduino) | 同名（原生 FreeRTOS） |
| `WiFiClientSecure` | `esp_http_client` / `esp_websocket_client` |
| `millis()` | `esp_timer_get_time()/1000` |
| `voice.speak()` | 一期：libhelix 解码→`codec_->Output(true)` 直写喇叭（绕过小智 opus 管线，独立互斥锁防抢占） |

### 与小智宿主的挂钩点（唯一侵入面，全部通过 Schedule() 回主任务）

1. **对话记忆沉淀（MCP-first，2026-10-10 修订）**：撤协议旁听挂点。xiaozhi.me 云端 LLM 经 MCP 协议调用设备侧意识工具（main/laap_mcp_tools.cc 注册 6 个：get_status/recall/remember/teach_skill/add_intent/apply_rules）；宿主把每轮对话经 `laap::life_note_conversation(user, assistant)` 成对转发给心跳任务落账。所有回调只入队/读快照，单一写者=心跳任务不变。
2. **按钮/触摸→表达**：现有 Board 按钮事件 → `laap_life.Express(forced)`.
3. **静默独白**：`laap_life` 任务自有节奏（idleSilence/idleEvery 配置），不依赖宿主。
4. **堆门禁**：所有意识链入口（life/expression/monologue/ttspre 等价物）+ 宿主状态感知（`Application::GetDeviceState()`——小智正在语音对话时意识链全部让路，独占期=对话期+10s）。

## 4. 里程碑

- **M1（本提交）**：设计文档 + 组件骨架 + cognition/skills/config 三件移植完成（可编译为独立组件）。
- **M2**：memory + rules + nvs 配置落盘 + LittleFS 挂载（conscious 分区）。
- **M3**：laap_llm + laap_search（设备侧思考能力）。
- **M4**：laap_tts + libhelix + 喇叭直写（自主发声能力）。
- **M5**：laap_life 心跳任务 + 全链堆门禁 + 黑匣子（RTC panic 捕获平移）。

> **2026-10-10 修订（网络三件删除，用户拍板）**：设备侧 AI 客户端全部删除——
> `laap_llm`/`laap_search`/`laap_tts`+libhelix 移除（-100KB），语义向量通道删除
> （召回=关键词通道）。**自主说话经宿主 listen-detect 注入**：心跳把内在状态编入
> 合成指令 → `Protocol::SendWakeWordDetected` 发到云端 → xiaozhi 云 LLM（自带联网
> 搜索）+ 云 TTS 出声（声音与正常对话统一）。API Key 全部移到 xiaozhi.me 云端配置，
> 设备侧零密钥。M6 需实测：云端对'无唤醒 detect'的接受度。


> **2026-10-10 补丁（记忆回灌 A+B 并存，用户拍板）**：修复"断桥"——
> A 通道（宿主旁听只读挂点，自动）：stt→`life_note_user`、tts sentence_start→句片累积、
> tts stop→整轮落账；B 通道（MCP `consciousness.log_turn`，云端主动调用）。
> 双通道内置去重：用户同文 180s 只记一次；每轮回复只记一条（首达优先）。
> 效果：真实对话进本地记忆，信任/性格进化/技能命中恢复触发；云侧记忆仍独立（服务端）。


> **2026-10-10 冲突清点（cebb109）**：云端/本地全量对照——① 注入去 Aris 硬编码
> （身份单一权威=xiaozhi.me 角色介绍）② get_status 快照追加 skills/rules/intents
> （打通 teach_skill/apply_rules 的读取断供）③ 注入回声防御（detect 被云回显成 stt
> 时不记账）④ inject_say 不再记录命令文本、注入后清记账名额（自发回复可落账）。
> 遗留（非代码）：xiaozhi.me 角色介绍按草稿配置（用户侧）；注入文本可能显示在屏幕
> "用户消息"区（宿主显示层，M6 观察）。

- **M6**：IDF 6.1 编译通过 → USB 烧录 szpi-esp32s3 → 实机验证（语音对话不干扰 + 自主表达发声 + 重启记忆延续）。
- **二期**：主对话切设备侧全管线。

## 5. 风险登记

- **内部堆共存**：小智音频管线（AFE/opus）+ 我们意识链 TLS，两套大户并存。门禁阈值需实机重标定（一期先按 LAAP 数值，M6 实测调整）。
- **喇叭互斥**：小智云 TTS 与我们设备 TTS 不能同时输出（互斥锁 + 设备状态门禁）。
- **IDF 6.1 API 变动**：esp_http_client/esp_websocket_client 的 6.x 变更点在 M3/M4 实测。
- **assets 分区压缩**：若 6M 不够放，回到 7M+1M（意识数据最少 1M 可起步）。
