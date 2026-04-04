# models

放置 `pythonAiService` 使用的 ONNX 模型。

当前约定：

- A2 人像分割：
  - `pythonAiService/models/portrait_segmentation.onnx`
- A3 YOLO 检测：
  - `pythonAiService/models/yolo11n.onnx`

如果文件名或节点名不同，可以在：

- [ai_service.ini](/d:/QTcoding/SmartMeet/pythonAiService/ai_service.ini)

里调整：

- `segment.model_path`
- `segment.input_name`
- `segment.output_name`
- `detect.model_path`
- `detect.input_name`
- `detect.output_name`

当前服务端接口：

- `POST /segment`
  - 输入：base64 图片
  - 输出：PNG mask（base64）
- `POST /detect`
  - 输入：base64 图片
  - 输出：检测框、类别、置信度

说明：

- `portrait_segmentation.onnx` 已用于 A2 第一阶段。
- `yolo11n.onnx` 是 A3 第一阶段推荐的轻量检测模型名约定。
- 这两个接口都可以先独立测试，不要求立刻接入 Qt 主链。
