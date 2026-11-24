# pythonAiService

SmartMeet 的本地 AI 服务。

当前职责：

- A0/A1：AI 助手
- A2：人像分割 `/segment`
- A3：YOLO 检测 `/detect` 与 Qt 本地预览叠框

---

## 1. 当前接口

- `GET /healthz`
- `POST /assistant`
- `POST /segment`
- `POST /detect`

---

## 2. 当前边界

当前服务：

- 不直接读取源码仓库
- 不直接查询数据库
- 不直接读取线上服务器实时状态
- 默认用于本地客户端调用，个人助手问答不通过会议信令广播
- 尚无共享会议历史、ASR 转写、会议记录工具调用与智能纪要

当前 `/segment`：

- 已可独立使用
- 已接入 Qt 客户端虚拟背景主线

当前 `/detect`：

- 已接入 Qt 本地预览，提供菜单开关
- 叠框不进入推流；虚拟背景或共享开启时暂停检测

---

## 3. 目录说明

- [main.py](./main.py)
- [ai_service.ini](./ai_service.ini)
- [project_facts.md](./project_facts.md)
- [requirements.txt](./requirements.txt)
- [models/README.md](./models/README.md)

---

## 4. 安装与启动

```powershell
cd D:\QTcoding\SmartMeet\pythonAiService
python -m venv .venv
.\.venv\Scripts\activate
pip install -r requirements.txt
python -m uvicorn main:app --host 127.0.0.1 --port 18080
```

---

## 5. 配置文件

配置文件：

- [ai_service.ini](./ai_service.ini)

主要分段：

- `[assistant]`
  - 助手模式与本地回退策略
- `[llm]`
  - 真实大模型配置
- `[segment]`
  - A2 人像分割模型配置
- `[detect]`
  - A3 YOLO 检测模型配置

支持环境变量覆盖：

```powershell
$env:SMARTMEET_AI_CONFIG="D:\QTcoding\SmartMeet\pythonAiService\ai_service.ini"
$env:SMARTMEET_LLM_API_KEY="你的密钥"
$env:SMARTMEET_SEGMENT_ENABLED="true"
$env:SMARTMEET_SEGMENT_MODEL="D:\QTcoding\SmartMeet\pythonAiService\models\portrait_segmentation.onnx"
$env:SMARTMEET_DETECT_ENABLED="true"
$env:SMARTMEET_DETECT_MODEL="D:\QTcoding\SmartMeet\pythonAiService\models\yolo11n.onnx"
```

---

## 6. 健康检查

```powershell
curl http://127.0.0.1:18080/healthz
```

重点看：

- `llm_ready`
- `segment_ready`
- `detect_ready`
- `segment_note`
- `detect_note`

---

## 7. A2 分割

当前推荐：

- 模型：`portrait_segmentation.onnx`
- 输入：`192x192`
- `output_is_logits=false`

测试 `/segment` 时：

- 输入一张 base64 图片
- 返回一张 PNG mask（base64）

---

## 8. A3 检测

当前推荐模型约定：

- `pythonAiService/models/yolo11n.onnx`

当前 `/detect` 返回：

- 检测框
- 类别 ID
- 类别名
- 置信度

它现在适合做：

- 服务端单独验证
- 模型选择与阈值调试
- Qt 本地预览调试叠框

它现在还不负责：

- 远端视频联动
- OpenGL 叠加

---

## 9. 项目事实文件

[project_facts.md](./project_facts.md) 用来约束 AI 助手回答 SmartMeet 项目事实。

如果这些事实变了，优先更新这份文件，而不是直接改写死在 `main.py` 里的提示词。

---

## 10. 当前状态

- A1：已收口
- A2：已完成第一阶段并接入客户端
- A3：`/detect`、Qt 本地预览叠框与菜单开关已接入
- 本服务当前仍是文字助手与视觉推理服务，不具备自主获取会议音频或历史的能力

---

## 11. 下一步

以 [开发与部署计划](../docs/毕业设计开发与部署计划.md) 为准，以下均为待开发：

1. 恢复本地 Linux 服务，验证 WebRTC 原生 SDK 与 SFU 并迁移媒体链。
2. 新增共享转写工作进程与会议记录接口，聊天和转写持久化；记录器按会议运行，不随个人客户端退出。
3. 复用 Qt 独立助手窗口，增加受控工具调用、历史查询、阶段摘要与纪要编辑导出。

个人问答按用户、会议和线程隔离，不广播给其他成员。共享服务可先放在开发电脑，后续按资源部署；与现有视觉服务分离。当前不接入 TTS、主动语音发言或停止播报，详细范围见 [首模块计划](../docs/毕业设计首模块计划.md)。
