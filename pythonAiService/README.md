# pythonAiService

SmartMeet A0/A1 本地 AI 服务。

当前职责：
- 提供 `POST /assistant`
- 提供 `GET /healthz`
- 默认用本地规则型助手打通 `Qt/C++ -> HTTP(JSON) -> Python`
- 支持可配置接入 OpenAI 兼容大模型 API，并在失败时自动回退到本地规则型回答

## 1. 安装依赖

```powershell
cd D:\QTcoding\SmartMeet\pythonAiService
python -m venv .venv
.\.venv\Scripts\activate
pip install -r requirements.txt
```

## 2. 配置模式

默认配置文件：

- [ai_service.ini](/d:/QTcoding/SmartMeet/pythonAiService/ai_service.ini)

默认是本地兜底模式：

```ini
[assistant]
provider_mode=auto
fallback_to_local=true

[llm]
enabled=false
```

如果你要接真实大模型，改这里：

```ini
[assistant]
provider_mode=auto
fallback_to_local=true

[llm]
enabled=true
base_url=https://api.openai.com/v1
api_key=你的密钥
model=你的模型名
timeout_ms=600000
temperature=0.2
max_tokens=800
```

也支持环境变量覆盖：

```powershell
$env:SMARTMEET_LLM_API_KEY="你的密钥"
$env:SMARTMEET_LLM_BASE_URL="https://api.openai.com/v1"
$env:SMARTMEET_LLM_MODEL="你的模型名"
```

## 3. 启动服务

```powershell
cd D:\QTcoding\SmartMeet\pythonAiService
.\.venv\Scripts\activate
python -m uvicorn main:app --host 127.0.0.1 --port 18080
```

## 4. 健康检查

```powershell
curl http://127.0.0.1:18080/healthz
```

返回里会包含：
- 当前 provider 模式
- 是否具备真实 LLM 调用条件
- 当前模型名

## 5. 当前客户端接入方式

- `smartmeet.ini` 的 `[ai]` 段默认指向 `http://127.0.0.1:18080`
- 聊天区新增 `AI助手` 按钮，点击后弹出独立提问窗口
- 输入 `@AI xxx` 后直接回车，也会打开独立窗口并自动走本地 AI 助手
- 问答默认仅当前客户端可见，不广播到房间
- 助手窗口支持两种模式：
  - `通用助手`：一般问题也可回答
  - `项目助手`：优先围绕 SmartMeet 回答
- 助手窗口会携带最近几轮对话作为短期记忆发送给模型
- `project_facts.md` 作为项目事实知识源，会参与项目模式提示词构造，减少对项目细节的乱编

## 6. 当前行为说明

- 如果 `llm.enabled=false`，始终走本地规则型回答
- 如果 `llm.enabled=true` 且配置完整，优先请求真实大模型
- 如果真实大模型超时、报错或返回无效内容，会自动回退到本地规则型回答
- 即使启用真实大模型，当前也不会直接读取你的源码、数据库或服务器状态；项目细节回答仍然只基于已传入上下文

## 7. 下一步

- 新增 `/segment`，为 AI 虚拟背景服务
- 做请求限频、历史上下文和异常降级
