# pythonAiService

SmartMeet 的本地 AI 服务，属于 `A0 + A1` 阶段产物。  
它当前负责：
- 把 Qt 客户端里的 AI 助手请求转成 HTTP 调用，并在“真实大模型 / 本地规则兜底”之间做切换
- 为 A2 准备 ONNX Runtime 人像分割接口骨架

---

## 1. 当前职责

- 提供 `GET /healthz`
- 提供 `POST /assistant`
- 提供 `POST /segment`（A2-P1）
- 支持真实大模型优先
- 支持失败时回退到本地规则型回答
- 支持 `通用助手 / 项目助手` 双模式
- 支持最近几轮对话短期记忆
- 通过 `project_facts.md` 给项目模式补充受控事实

---

## 2. 当前边界

当前这版 AI 助手：
- 不直接读取源码
- 不直接查询数据库
- 不直接读取服务器实时状态
- 不通过信令广播到房间
- 默认仅当前客户端可见

当前这版 `/segment`：
- 只完成 Python 侧接口与 ONNX 推理骨架
- 还没有正式接入 Qt 视频主链

也就是说，它目前是：
- `Qt/C++ -> HTTP(JSON) -> Python`
- 不是代码检索助手
- 不是数据库查询助手
- 不是线上运维机器人

---

## 3. 目录说明

- [main.py](/d:/QTcoding/SmartMeet/pythonAiService/main.py)
  - FastAPI 服务入口
- [ai_service.ini](/d:/QTcoding/SmartMeet/pythonAiService/ai_service.ini)
  - AI 服务配置
- [project_facts.md](/d:/QTcoding/SmartMeet/pythonAiService/project_facts.md)
  - 项目事实知识源
- [requirements.txt](/d:/QTcoding/SmartMeet/pythonAiService/requirements.txt)
  - Python 依赖
- [models/README.md](/d:/QTcoding/SmartMeet/pythonAiService/models/README.md)
  - ONNX 模型放置说明

---

## 4. 安装与启动

### 4.1 创建虚拟环境

```powershell
cd D:\QTcoding\SmartMeet\pythonAiService
python -m venv .venv
.\.venv\Scripts\activate
pip install -r requirements.txt
```

### 4.2 启动服务

```powershell
cd D:\QTcoding\SmartMeet\pythonAiService
.\.venv\Scripts\activate
python -m uvicorn main:app --host 127.0.0.1 --port 18080
```

---

## 5. 配置说明

默认配置文件：
- [ai_service.ini](/d:/QTcoding/SmartMeet/pythonAiService/ai_service.ini)

### 5.1 `assistant` 段

- `name`
  - 助手显示名
- `provider_mode`
  - `auto`：优先真实模型，失败再回退
  - `openai_compatible`：强制走真实模型
  - `local`：强制只走本地兜底
- `fallback_to_local`
  - 真实模型失败后是否允许降级到本地规则回答

### 5.2 `llm` 段

- `enabled`
  - 是否启用真实大模型链路
- `base_url`
  - OpenAI 兼容 API 基地址
- `api_key`
  - 模型服务密钥
- `model`
  - 模型名称
- `timeout_ms`
  - 请求超时，当前默认 `600000`
- `temperature`
  - 模型随机性
- `max_tokens`
  - 单次回答最大 token 数

### 5.3 环境变量覆盖

支持这些环境变量：

```powershell
$env:SMARTMEET_AI_CONFIG="D:\QTcoding\SmartMeet\pythonAiService\ai_service.ini"
$env:SMARTMEET_LLM_API_KEY="你的密钥"
$env:SMARTMEET_LLM_BASE_URL="https://api.openai.com/v1"
$env:SMARTMEET_LLM_MODEL="你的模型名"
$env:SMARTMEET_AI_PROVIDER_MODE="auto"
$env:SMARTMEET_SEGMENT_ENABLED="true"
$env:SMARTMEET_SEGMENT_MODEL="D:\QTcoding\SmartMeet\pythonAiService\models\portrait_segmentation.onnx"
```

说明：
- 环境变量优先级高于 `ai_service.ini`
- 真正长期使用时，推荐把 `api_key` 放环境变量，不建议把真实密钥长期留在仓库文件里

---

## 6. 健康检查

```powershell
curl http://127.0.0.1:18080/healthz
```

返回里重点看：
- `provider_mode`
- `llm_ready`
- `llm_model`
- `segment_ready`
- `segment_note`

如果：
- `llm_ready=true`

说明当前配置已经满足真实模型调用条件。

如果：
- `segment_ready=true`

说明当前 ONNX 人像分割模型已成功加载。

---

## 7. 客户端接入方式

客户端配置文件：
- [smartmeet.ini](/d:/QTcoding/SmartMeet/smartmeet.ini)

当前客户端行为：
- 聊天区有 `AI助手` 按钮
- 点击后会弹出独立 AI 助手窗口
- 输入 `@AI ...` 后回车，也会自动打开助手窗口并提交
- 问答默认不广播到房间
- 模式支持：
  - `通用助手`
  - `项目助手`
- 客户端会附带最近几轮对话作为短期记忆

---

## 8. `project_facts.md` 的作用

[project_facts.md](/d:/QTcoding/SmartMeet/pythonAiService/project_facts.md) 是当前 AI 助手的受控事实源。

它的作用是：
- 约束项目模式回答
- 减少对项目真实细节的乱编
- 把“已确认事实”从提示词代码里抽出来，改成独立文档维护

如果以后这些事实变了，比如：
- 线上信令服务名变了
- 当前主链不是 RTMP 了
- 新增了已确认数据库表

应优先更新这份文件，而不是直接去改 `main.py` 里的提示词字符串。

---

## 9. 当前行为说明

- `llm.enabled=false`
  - 永远走本地规则型回答
- `llm.enabled=true` 且配置完整
  - 优先走真实大模型
- 真实模型失败、超时、异常
  - 可按 `fallback_to_local` 回退到本地规则回答
- 项目模式
  - 优先回答 SmartMeet
  - 对未确认事实应更保守
- 通用模式
  - 普通问题可正常回答
  - 但遇到项目实现细节仍不应该乱编

---

## 10. A1 验收建议

建议配合这份文档一起使用：
- [A1_AI助手验收清单.md](/d:/QTcoding/SmartMeet/docs/A1_AI助手验收清单.md)

---

## 11. 下一步

当前 `A1` 已收口，`A2` 已进入 `P1`：

1. 放置可用的 ONNX 人像分割模型
2. 通过 `/healthz` 确认 `segment_ready=true`
3. 单独测试 `/segment`
4. 再进入 Qt 侧 `AiSegmentationClient`

当前若使用 `PP-HumanSeg`，建议先配：
- `input_width=192`
- `input_height=192`
- `output_is_logits=false`
