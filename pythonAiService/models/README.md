# models

把 A2 使用的 ONNX 人像分割模型放在这个目录下。

当前默认配置会尝试读取：

- `pythonAiService/models/portrait_segmentation.onnx`

第一版建议：
- 先放一个单人像分割模型
- 输入尺寸先配合 `256x256`
- 输出最好是单通道 mask，或可通过自动逻辑提取出前景通道

如果模型文件名或输入输出节点名不同，可以在：

- [ai_service.ini](/d:/QTcoding/SmartMeet/pythonAiService/ai_service.ini)

里修改：
- `model_path`
- `input_name`
- `output_name`

当前 `/segment` 接口只负责：
- 接收一张 base64 图片
- 返回一张 PNG 格式 mask（base64）

它还没有接入 Qt 视频主链；这属于 A2 后续阶段。
