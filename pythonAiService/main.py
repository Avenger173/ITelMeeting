from __future__ import annotations

import base64
import configparser
import io
import logging
import math
import os
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import httpx
import numpy as np
from fastapi import FastAPI, HTTPException
from PIL import Image
from pydantic import BaseModel, Field

try:
    import onnxruntime as ort
except Exception:  # noqa: BLE001
    ort = None


logger = logging.getLogger("smartmeet-ai")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

PIL_BILINEAR = Image.Resampling.BILINEAR if hasattr(Image, "Resampling") else Image.BILINEAR


@dataclass
class ServiceConfig:
    assistant_name: str = "AI助手"
    provider_mode: str = "auto"
    fallback_to_local: bool = True
    llm_enabled: bool = False
    llm_base_url: str = "https://api.openai.com/v1"
    llm_api_key: str = ""
    llm_model: str = ""
    llm_timeout_ms: int = 600000
    llm_temperature: float = 0.5
    llm_max_tokens: int = 1200
    segment_enabled: bool = False
    segment_backend: str = "onnxruntime"
    segment_model_path: str = "models/portrait_segmentation.onnx"
    segment_input_width: int = 256
    segment_input_height: int = 256
    segment_threshold: float = 0.5
    segment_output_is_logits: bool = True
    segment_input_name: str = ""
    segment_output_name: str = ""


def _resolve_config_path() -> Path:
    env_path = os.getenv("SMARTMEET_AI_CONFIG", "").strip()
    if env_path:
        return Path(env_path).expanduser().resolve()
    return (Path(__file__).resolve().parent / "ai_service.ini").resolve()


def _resolve_project_facts_path() -> Path:
    env_path = os.getenv("SMARTMEET_PROJECT_FACTS", "").strip()
    if env_path:
        return Path(env_path).expanduser().resolve()
    return (Path(__file__).resolve().parent / "project_facts.md").resolve()


def _service_root() -> Path:
    return Path(__file__).resolve().parent


def _resolve_segment_model_path(raw_path: str) -> str:
    text = (raw_path or "").strip()
    if not text:
        return str((_service_root() / "models" / "portrait_segmentation.onnx").resolve())

    path = Path(text).expanduser()
    if not path.is_absolute():
        path = (_service_root() / path).resolve()
    return str(path)


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name, "").strip().lower()
    if not raw:
        return default
    return raw in {"1", "true", "yes", "on"}


def _load_config() -> ServiceConfig:
    path = _resolve_config_path()
    parser = configparser.ConfigParser()
    if path.exists():
        parser.read(path, encoding="utf-8")

    cfg = ServiceConfig(
        assistant_name=parser.get("assistant", "name", fallback="AI助手").strip() or "AI助手",
        provider_mode=parser.get("assistant", "provider_mode", fallback="auto").strip().lower() or "auto",
        fallback_to_local=parser.getboolean("assistant", "fallback_to_local", fallback=True),
        llm_enabled=parser.getboolean("llm", "enabled", fallback=False),
        llm_base_url=parser.get("llm", "base_url", fallback="https://api.openai.com/v1").strip(),
        llm_api_key=parser.get("llm", "api_key", fallback="").strip(),
        llm_model=parser.get("llm", "model", fallback="").strip(),
        llm_timeout_ms=max(1000, parser.getint("llm", "timeout_ms", fallback=600000)),
        llm_temperature=parser.getfloat("llm", "temperature", fallback=0.5),
        llm_max_tokens=max(64, parser.getint("llm", "max_tokens", fallback=1200)),
        segment_enabled=parser.getboolean("segment", "enabled", fallback=False),
        segment_backend=parser.get("segment", "backend", fallback="onnxruntime").strip().lower() or "onnxruntime",
        segment_model_path=_resolve_segment_model_path(
            parser.get("segment", "model_path", fallback="models/portrait_segmentation.onnx").strip()
        ),
        segment_input_width=max(64, parser.getint("segment", "input_width", fallback=256)),
        segment_input_height=max(64, parser.getint("segment", "input_height", fallback=256)),
        segment_threshold=min(0.99, max(0.01, parser.getfloat("segment", "threshold", fallback=0.5))),
        segment_output_is_logits=parser.getboolean("segment", "output_is_logits", fallback=True),
        segment_input_name=parser.get("segment", "input_name", fallback="").strip(),
        segment_output_name=parser.get("segment", "output_name", fallback="").strip(),
    )

    env_api_key = os.getenv("SMARTMEET_LLM_API_KEY", "").strip()
    env_base_url = os.getenv("SMARTMEET_LLM_BASE_URL", "").strip()
    env_model = os.getenv("SMARTMEET_LLM_MODEL", "").strip()
    env_mode = os.getenv("SMARTMEET_AI_PROVIDER_MODE", "").strip().lower()
    env_segment_model = os.getenv("SMARTMEET_SEGMENT_MODEL", "").strip()
    env_segment_enabled = _env_bool("SMARTMEET_SEGMENT_ENABLED", cfg.segment_enabled)
    env_segment_w = os.getenv("SMARTMEET_SEGMENT_INPUT_WIDTH", "").strip()
    env_segment_h = os.getenv("SMARTMEET_SEGMENT_INPUT_HEIGHT", "").strip()
    env_segment_threshold = os.getenv("SMARTMEET_SEGMENT_THRESHOLD", "").strip()

    if env_api_key:
        cfg.llm_api_key = env_api_key
    if env_base_url:
        cfg.llm_base_url = env_base_url
    if env_model:
        cfg.llm_model = env_model
    if env_mode:
        cfg.provider_mode = env_mode

    cfg.segment_enabled = env_segment_enabled
    if env_segment_model:
        cfg.segment_model_path = _resolve_segment_model_path(env_segment_model)
    if env_segment_w.isdigit():
        cfg.segment_input_width = max(64, int(env_segment_w))
    if env_segment_h.isdigit():
        cfg.segment_input_height = max(64, int(env_segment_h))
    if env_segment_threshold:
        try:
            cfg.segment_threshold = min(0.99, max(0.01, float(env_segment_threshold)))
        except ValueError:
            logger.warning("invalid SMARTMEET_SEGMENT_THRESHOLD=%s", env_segment_threshold)

    logger.info(
        '[Config] path="%s" assistant="%s" mode="%s" llm_enabled=%s base="%s" model="%s" segment_enabled=%s segment_model="%s"',
        path,
        cfg.assistant_name,
        cfg.provider_mode,
        cfg.llm_enabled,
        cfg.llm_base_url,
        cfg.llm_model or "<empty>",
        cfg.segment_enabled,
        cfg.segment_model_path,
    )
    return cfg


CONFIG = _load_config()


def _load_project_facts() -> str:
    path = _resolve_project_facts_path()
    try:
        if path.exists():
            content = path.read_text(encoding="utf-8").strip()
            if content:
                logger.info('[ProjectFacts] loaded path="%s"', path)
                return content
    except Exception as exc:  # noqa: BLE001
        logger.warning("failed to load project facts: %s", exc)

    logger.warning('[ProjectFacts] fallback to built-in facts, path="%s"', path)
    return (
        "SmartMeet confirmed project facts:\n"
        "- Client is a Qt Widgets based Windows desktop meeting client.\n"
        "- Current media main path is RTMP.\n"
        "- Online signaling is Go based.\n"
        "- Online database is MySQL.\n"
        "- Confirmed database tables are users and meeting_events only.\n"
        "- Assistant cannot directly read source code, database, or live server state.\n"
    )


PROJECT_FACTS = _load_project_facts()


class OnnxPortraitSegmenter:
    def __init__(self, cfg: ServiceConfig) -> None:
        self._cfg = cfg
        self._session = None
        self._lock = threading.Lock()
        self.ready = False
        self.note = "disabled"
        self.backend = cfg.segment_backend
        self.model_name = Path(cfg.segment_model_path).name or "<empty>"
        self.input_name = cfg.segment_input_name
        self.output_name = cfg.segment_output_name

        if not cfg.segment_enabled:
            return

        if cfg.segment_backend != "onnxruntime":
            self.note = f"unsupported_backend:{cfg.segment_backend}"
            return

        if ort is None:
            self.note = "onnxruntime_missing"
            return

        model_path = Path(cfg.segment_model_path)
        if not model_path.exists():
            self.note = f"model_not_found:{model_path}"
            return

        try:
            session_options = ort.SessionOptions()
            session_options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
            self._session = ort.InferenceSession(
                str(model_path),
                sess_options=session_options,
                providers=["CPUExecutionProvider"],
            )

            if not self.input_name:
                self.input_name = self._session.get_inputs()[0].name
            if not self.output_name:
                self.output_name = self._session.get_outputs()[0].name

            self.ready = True
            self.note = "ready"
            logger.info(
                '[Segment] ONNX ready model="%s" input="%s" output="%s" size=%dx%d',
                model_path,
                self.input_name,
                self.output_name,
                cfg.segment_input_width,
                cfg.segment_input_height,
            )
        except Exception as exc:  # noqa: BLE001
            self.note = f"load_failed:{type(exc).__name__}"
            logger.warning("segment model load failed: %s", exc)

    def _extract_mask(self, output: np.ndarray) -> np.ndarray:
        mask = np.asarray(output, dtype=np.float32)

        if mask.ndim == 4 and mask.shape[0] == 1:
            mask = mask[0]
        if mask.ndim == 3:
            if mask.shape[0] == 1:
                mask = mask[0]
            elif mask.shape[0] == 2:
                mask = mask[1]
            else:
                mask = mask[0]

        if mask.ndim != 2:
            raise ValueError(f"unsupported mask shape: {tuple(mask.shape)}")
        return mask

    def _normalize_mask(self, mask: np.ndarray) -> np.ndarray:
        if self._cfg.segment_output_is_logits or mask.min() < 0.0 or mask.max() > 1.0:
            mask = np.clip(mask, -20.0, 20.0)
            mask = 1.0 / (1.0 + np.exp(-mask))
        return np.clip(mask, 0.0, 1.0)

    def segment_image(self, image: Image.Image, threshold: float, return_soft_mask: bool) -> tuple[bytes, int, int, float]:
        if not self.ready or self._session is None:
            raise RuntimeError(self.note)

        rgb = image.convert("RGB")
        orig_w, orig_h = rgb.size

        resized = rgb.resize((self._cfg.segment_input_width, self._cfg.segment_input_height), PIL_BILINEAR)
        arr = np.asarray(resized, dtype=np.float32) / 255.0
        arr = np.transpose(arr, (2, 0, 1))[None, ...]

        start = time.perf_counter()
        with self._lock:
            outputs = self._session.run([self.output_name], {self.input_name: arr})
        _ = int((time.perf_counter() - start) * 1000)

        mask = self._extract_mask(outputs[0])
        mask = self._normalize_mask(mask)

        fg_ratio = float((mask >= threshold).mean())

        if return_soft_mask:
            mask_u8 = np.clip(mask * 255.0, 0.0, 255.0).astype(np.uint8)
        else:
            mask_u8 = np.where(mask >= threshold, 255, 0).astype(np.uint8)

        mask_img = Image.fromarray(mask_u8, mode="L").resize((orig_w, orig_h), PIL_BILINEAR)
        buffer = io.BytesIO()
        mask_img.save(buffer, format="PNG")
        return buffer.getvalue(), orig_w, orig_h, fg_ratio


SEGMENTER = OnnxPortraitSegmenter(CONFIG)

app = FastAPI(
    title="SmartMeet AI Service",
    version="0.5.0",
    description="SmartMeet 本地 AI 助手与人像分割服务",
)


class AssistantRequest(BaseModel):
    message: str = Field(..., min_length=1, max_length=2000)
    room_id: Optional[str] = None
    user_id: Optional[str] = None
    stream: Optional[str] = None
    profile: Optional[str] = None
    assistant_mode: str = "general"
    history: list[dict] = Field(default_factory=list)


class AssistantResponse(BaseModel):
    ok: bool = True
    answer: str
    provider: str = "local"
    model: str = "rule-based-v1"
    latency_ms: int
    note: Optional[str] = None


class SegmentRequest(BaseModel):
    image_base64: str = Field(..., min_length=16)
    request_id: Optional[str] = None
    threshold: Optional[float] = Field(default=None, ge=0.01, le=0.99)
    return_soft_mask: bool = True


class SegmentResponse(BaseModel):
    ok: bool = True
    backend: str = "onnxruntime"
    model: str
    request_id: Optional[str] = None
    width: int
    height: int
    mask_png_base64: str
    latency_ms: int
    foreground_ratio: float
    note: Optional[str] = None


def _contains_any(text: str, *keywords: str) -> bool:
    return any(keyword in text for keyword in keywords)


def _sanitize_mode(mode: str) -> str:
    clean = (mode or "").strip().lower()
    return clean if clean in {"general", "project"} else "general"


def _project_fact_sheet() -> str:
    return PROJECT_FACTS


def _build_local_answer(req: AssistantRequest) -> str:
    text = req.message.strip().lower()
    mode = _sanitize_mode(req.assistant_mode)

    if _contains_any(text, "共享", "屏幕", "share", "window"):
        return (
            "屏幕共享已经接进 SmartMeet 主流程啦。你可以先选共享源，再开始共享；"
            "主持人也能停止成员共享。要是共享画面看起来偏糊，通常先查分辨率、码率档位和窗口缩放。"
        )

    if _contains_any(text, "白板", "wb", "draw", "undo", "lock"):
        return (
            "白板目前支持画线、撤销、清空和锁定。主持人或联席主持能管锁定状态，"
            "普通成员在白板锁定时只能看不能写。"
        )

    if _contains_any(text, "主持", "联席", "mute", "kick", "cohost", "host"):
        return (
            "主持控制已经包括静音/恢复、关开摄像头、停止共享、踢人、转主持和联席主持管理。"
            "现在这条链路已经跑在 Go 信令上。"
        )

    if _contains_any(text, "录制", "录屏", "截图", "拍照", "record", "capture"):
        return (
            "录制和截图是客户端能力。录制更偏会议留档，截图更偏临时保存。"
            "后面更值得补的是录制状态反馈和导出后的文件提示。"
        )

    if _contains_any(text, "美颜", "磨皮", "瘦脸", "虚拟背景", "background", "beauty"):
        return (
            "当前美颜还主要是本地 CPU 图像处理，能用，但自然度和性能都还有继续优化的空间。"
            "接下来更值得做的是 AI 虚拟背景，再考虑把部分显示叠加交给 OpenGL。"
        )

    if _contains_any(text, "聊天", "消息", "assistant", "ai", "@ai"):
        return (
            "AI 助手现在是本地独立窗口，不会把你的提问广播给房间其他人。"
            "它会优先走真实大模型；如果模型超时或失败，就回退到本地规则回答。"
        )

    if _contains_any(text, "部署", "服务器", "mysql", "go", "信令", "signal"):
        return (
            "当前线上基线是 RTMP + Go 信令 + MySQL + ZLMediaKit。"
            "如果你要继续往工程化推进，优先做 README 收口、部署脚本、日志观测和回归基线会更稳。"
        )

    room_hint = f" 当前房间是 {req.room_id}。" if req.room_id else ""
    user_hint = f" 当前用户是 {req.user_id}。" if req.user_id else ""
    if mode == "general":
        return (
            "我现在落到本地兜底回答啦，所以通用问题的发挥会比较有限。"
            "如果你是在问 SmartMeet 相关内容，我还能继续帮你梳理；"
            "如果你是在问开放问题，建议先确认真实大模型服务是不是可用。"
            f"{room_hint}{user_hint}"
        )
    return (
        "我现在是 SmartMeet 的本地项目助手兜底模式，更适合回答会议功能、部署结构、白板、共享、录制和 AI 规划。"
        f"{room_hint}{user_hint}"
    )


def _build_system_prompt(req: AssistantRequest) -> str:
    room_hint = req.room_id or "unknown"
    user_hint = req.user_id or "unknown"
    stream_hint = req.stream or "unknown"
    profile_hint = req.profile or "unknown"
    mode = _sanitize_mode(req.assistant_mode)

    common_style = (
        "回答要自然、清晰、直接，语气可以带一点可爱俏皮感，但不要过度卖萌，不要堆表情。"
        "优先保证事实准确和表达顺滑。"
    )

    if mode == "project":
        return (
            "你是 SmartMeet 客户端内的项目助手。\n"
            f"{common_style}\n"
            "你优先回答 SmartMeet 的功能、架构、部署、信令、数据库、白板、共享、录制、AI 规划等问题。\n"
            "你也可以回答一般常识问题，不要生硬拒绝。\n"
            "如果用户问到 SmartMeet 的具体实现细节，而上下文没有明确事实支撑，必须明确说“不确定”或“当前无法确认”，不能编造。\n"
            "绝对不要声称自己读过源码、查过数据库或访问过服务器实时状态，除非请求里真的提供了这些内容。\n"
            f"{_project_fact_sheet()}\n"
            f"当前会话信息：profile={profile_hint}, room={room_hint}, user={user_hint}, stream={stream_hint}。"
        )

    return (
        "你是 SmartMeet 客户端里的通用 AI 助手。\n"
        f"{common_style}\n"
        "你可以正常回答开放问题、闲聊问题、技术问题，也可以回答 SmartMeet 相关问题。\n"
        "如果用户问 SmartMeet 的真实代码、真实表结构、真实部署细节，只能基于已提供的事实回答；如果事实不足，必须说明不确定，不能脑补。\n"
        "不要把自己包装成能直接读取代码库或服务器状态的助手。\n"
        f"{_project_fact_sheet()}\n"
        f"当前会话信息：profile={profile_hint}, room={room_hint}, user={user_hint}, stream={stream_hint}。"
    )


def _llm_ready(cfg: ServiceConfig) -> bool:
    return (
        cfg.llm_enabled
        and bool(cfg.llm_base_url.strip())
        and bool(cfg.llm_api_key.strip())
        and bool(cfg.llm_model.strip())
    )


def _sigmoid(x: float) -> float:
    clipped = min(20.0, max(-20.0, x))
    return 1.0 / (1.0 + math.exp(-clipped))


def _decode_image_from_base64(data: str) -> Image.Image:
    text = data.strip()
    if text.startswith("data:") and "," in text:
        text = text.split(",", 1)[1]
    try:
        raw = base64.b64decode(text, validate=False)
    except Exception as exc:  # noqa: BLE001
        raise ValueError("invalid_base64_image") from exc

    try:
        return Image.open(io.BytesIO(raw)).convert("RGB")
    except Exception as exc:  # noqa: BLE001
        raise ValueError("invalid_image_bytes") from exc


async def _call_openai_compatible(req: AssistantRequest, cfg: ServiceConfig) -> AssistantResponse:
    base_url = cfg.llm_base_url.rstrip("/")
    url = f"{base_url}/chat/completions"
    headers = {
        "Authorization": f"Bearer {cfg.llm_api_key}",
        "Content-Type": "application/json",
    }

    messages = [{"role": "system", "content": _build_system_prompt(req)}]
    for item in req.history:
        role = str(item.get("role", "")).strip().lower()
        content = str(item.get("content", "")).strip()
        if role not in {"user", "assistant"} or not content:
            continue
        messages.append({"role": role, "content": content})
    messages.append({"role": "user", "content": req.message.strip()})

    payload = {
        "model": cfg.llm_model,
        "temperature": cfg.llm_temperature,
        "max_tokens": cfg.llm_max_tokens,
        "messages": messages,
    }

    start = time.perf_counter()
    async with httpx.AsyncClient(timeout=cfg.llm_timeout_ms / 1000.0) as client:
        response = await client.post(url, headers=headers, json=payload)
        response.raise_for_status()
        body = response.json()

    choices = body.get("choices", [])
    if not choices:
        raise ValueError("LLM 返回中缺少 choices")

    message = choices[0].get("message", {})
    answer = (message.get("content") or "").strip()
    if not answer:
        raise ValueError("LLM 返回空内容")

    latency_ms = int((time.perf_counter() - start) * 1000)
    return AssistantResponse(
        answer=answer,
        provider="openai-compatible",
        model=cfg.llm_model,
        latency_ms=latency_ms,
    )


def _fallback_response(req: AssistantRequest, start: float, note: Optional[str] = None) -> AssistantResponse:
    answer = _build_local_answer(req)
    latency_ms = int((time.perf_counter() - start) * 1000)
    return AssistantResponse(
        answer=answer,
        provider="local",
        model="rule-based-v1",
        latency_ms=latency_ms,
        note=note,
    )


@app.get("/healthz")
def healthz() -> dict:
    return {
        "ok": True,
        "service": "smartmeet-ai",
        "assistant_name": CONFIG.assistant_name,
        "provider_mode": CONFIG.provider_mode,
        "llm_ready": _llm_ready(CONFIG),
        "llm_model": CONFIG.llm_model or "rule-based-v1",
        "segment_ready": SEGMENTER.ready,
        "segment_backend": CONFIG.segment_backend,
        "segment_model": SEGMENTER.model_name,
        "segment_input_width": CONFIG.segment_input_width,
        "segment_input_height": CONFIG.segment_input_height,
        "segment_note": SEGMENTER.note,
    }


@app.post("/assistant", response_model=AssistantResponse)
async def assistant(req: AssistantRequest) -> AssistantResponse:
    start = time.perf_counter()
    mode = CONFIG.provider_mode

    if mode == "local":
        return _fallback_response(req, start)

    if mode not in {"auto", "openai_compatible"}:
        logger.warning("unknown provider_mode=%s, fallback to local", mode)
        return _fallback_response(req, start, note=f"unknown_provider_mode:{mode}")

    if not _llm_ready(CONFIG):
        return _fallback_response(req, start, note="llm_not_configured")

    try:
        return await _call_openai_compatible(req, CONFIG)
    except Exception as exc:  # noqa: BLE001
        logger.warning("llm request failed, fallback to local: %s", exc)
        if not CONFIG.fallback_to_local:
            raise
        return _fallback_response(req, start, note=f"fallback:{type(exc).__name__}")


@app.post("/segment", response_model=SegmentResponse)
async def segment(req: SegmentRequest) -> SegmentResponse:
    start = time.perf_counter()

    if not SEGMENTER.ready:
        raise HTTPException(status_code=503, detail=f"segment_not_ready:{SEGMENTER.note}")

    threshold = req.threshold if req.threshold is not None else CONFIG.segment_threshold

    try:
        image = _decode_image_from_base64(req.image_base64)
        mask_png, width, height, foreground_ratio = SEGMENTER.segment_image(
            image=image,
            threshold=threshold,
            return_soft_mask=req.return_soft_mask,
        )
    except HTTPException:
        raise
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("segment request failed: %s", exc)
        raise HTTPException(status_code=500, detail=f"segment_failed:{type(exc).__name__}") from exc

    latency_ms = int((time.perf_counter() - start) * 1000)
    return SegmentResponse(
        backend=CONFIG.segment_backend,
        model=SEGMENTER.model_name,
        request_id=req.request_id,
        width=width,
        height=height,
        mask_png_base64=base64.b64encode(mask_png).decode("ascii"),
        latency_ms=latency_ms,
        foreground_ratio=foreground_ratio,
        note="soft_mask" if req.return_soft_mask else "binary_mask",
    )
