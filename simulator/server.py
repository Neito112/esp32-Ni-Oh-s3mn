#!/usr/bin/env python3
"""
S.W.Ni-Oh Simulator Backend Server (Kiến trúc ESP-Claw)
Kết nối Mic/Loa PC với Multi-Provider Direct LLM + Fallback 429,
10 MCP Tools Tiếng Việt, Edge/Google TTS (/api/tts không lỗi CORS),
YouTube/SoundCloud Streamer (/api/music_stream), MJPEG Video, BLE Chronos & sw_nioh.bin Inspector.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import html
import json
import math
import mimetypes
import re
import shutil
import ssl
import struct
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

try:
    import edge_tts  # type: ignore
    HAS_EDGE_TTS = True
except Exception:
    HAS_EDGE_TTS = False

try:
    from PIL import Image, ImageDraw  # type: ignore
    HAS_PIL = True
except Exception:
    HAS_PIL = False

SIM_DIR = Path(__file__).resolve().parent
ROOT_DIR = SIM_DIR.parent
CONFIG_FILE = SIM_DIR / "sim_config.json"

VN_TZ = timezone(timedelta(hours=7))

DEFAULT_PIPED = "https://pipedapi.kavin.rocks,https://pipedapi.adminforge.de,https://pipedapi.tokhmi.xyz"

DEFAULT_PROFILES: List[Dict[str, Any]] = [
    {
        "name": "Groq Primary",
        "provider": "groq",
        "base_url": "https://api.groq.com/openai/v1",
        "model": "llama-3.3-70b-versatile",
        "api_key": "",
        "auth_scheme": "bearer",
        "custom_auth_name": "",
        "raw_endpoint": False,
        "enabled": True,
    },
    {
        "name": "OpenRouter Fallback",
        "provider": "openrouter",
        "base_url": "https://openrouter.ai/api/v1",
        "model": "openai/gpt-4o-mini",
        "api_key": "",
        "auth_scheme": "bearer",
        "custom_auth_name": "",
        "raw_endpoint": False,
        "enabled": False,
    },
]

DEFAULT_CONFIG: Dict[str, Any] = {
    "wifi_ssid": "S.W.Ni-Oh-WiFi",
    "wifi_pass": "",
    "stt_api_key": "",
    "llm_profiles": DEFAULT_PROFILES,
    "active_profile_idx": 0,
    "auto_fallback": True,
    "llm_base_url": "https://api.groq.com/openai/v1",
    "llm_model": "llama-3.3-70b-versatile",
    "llm_api_key": "",
    "tts_voice": "vi-VN-HoaiMyNeural",
    "tts_prefer_google": False,
    "search_engine": "ddg_wiki",
    "search_api_key": "",
    "searxng_url": "",
    "piped_instances": DEFAULT_PIPED,
    "media_proxy_url": "",
    "soundcloud_client_id": "",
    "video_res": "240x240",
    "video_fps": 12,
    "video_jpeg_quality": 75,
    "volume": 85,
    "mic_gain": 6,
    "backlight": 85,
    "rotation": 0,
}

SYSTEM_PROMPT_VI = (
    "Bạn là S.W.Ni-Oh, trợ lý AI thông minh nói tiếng Việt tự nhiên, thân thiện và ngắn gọn.\n"
    "QUY TẮC BẮT BUỘC:\n"
    "1. Luôn mở đầu MỌI câu trả lời cuối cùng bằng đúng 1 thẻ cảm xúc trong danh sách sau: "
    "[binh_thuong], [vui_ve], [ngac_nhien], [suy_nghi], [buon], [ngu_gat].\n"
    "2. Trả lời tự nhiên như đang trò chuyện bằng giọng nói (tránh dùng ký hiệu markdown **, #, dấu gạch đầu dòng phức tạp).\n"
    "3. Luôn ưu tiên gọi các công cụ MCP tiếng Việt khi người dùng hỏi thời tiết, giờ/âm lịch, tin tức, tra cứu thông tin, "
    "phát nhạc YouTube/SoundCloud (phat_nhac_truc_tuyen — luôn mặc định nen_tang='youtube' hoặc 'soundcloud', CHỈ chọn 'radio' khi người dùng nói rõ đài Radio/VOV), "
    "xem video trực tiếp lên màn hình (xem_video_truc_tuyen), suy luận sâu, điều khiển thiết bị, gửi ESP-NOW hoặc hẹn giờ Pomodoro.\n"
    "4. Giữ câu trả lời súc tích khoảng 2-4 câu trừ khi người dùng yêu cầu phân tích chi tiết."
)

EMOTION_TAGS = [
    "[binh_thuong]",
    "[vui_ve]",
    "[ngac_nhien]",
    "[suy_nghi]",
    "[buon]",
    "[ngu_gat]",
]

VN_PROVINCES: Dict[str, Tuple[float, float, str]] = {
    "ha noi": (21.0285, 105.8542, "Hà Nội"),
    "hanoi": (21.0285, 105.8542, "Hà Nội"),
    "ho chi minh": (10.8231, 106.6297, "TP. Hồ Chí Minh"),
    "sai gon": (10.8231, 106.6297, "TP. Hồ Chí Minh"),
    "hcm": (10.8231, 106.6297, "TP. Hồ Chí Minh"),
    "da nang": (16.0544, 108.2022, "Đà Nẵng"),
    "hai phong": (20.8449, 106.6881, "Hải Phòng"),
    "can tho": (10.0452, 105.7469, "Cần Thơ"),
    "hue": (16.4637, 107.5909, "Huế"),
    "nha trang": (12.2388, 109.1967, "Nha Trang"),
    "da lat": (11.9404, 108.4583, "Đà Lạt"),
    "vung tau": (10.3460, 107.0843, "Vũng Tàu"),
    "phu quoc": (10.2899, 103.9840, "Phú Quốc"),
    "quang ninh": (20.9505, 107.0734, "Hạ Long"),
    "nghe an": (18.6733, 105.6922, "Vinh"),
    "binh duong": (10.9804, 106.6519, "Bình Dương"),
    "dong nai": (10.9465, 106.8340, "Biên Hòa"),
}

VOV_CHANNELS: Dict[str, Tuple[str, str]] = {
    "vov1": ("http://media.kythuatvov.vn:7000/;stream/1", "VOV1 Thời sự"),
    "vov2": ("http://media.kythuatvov.vn:7002/;stream/1", "VOV2 Văn hóa"),
    "vov3": ("http://media.kythuatvov.vn:7004/;stream/1", "VOV3 Âm nhạc"),
    "vov giao thong": ("http://media.kythuatvov.vn:7006/;stream/1", "VOV Giao thông Hà Nội"),
    "vovgt": ("http://media.kythuatvov.vn:7006/;stream/1", "VOV Giao thông Hà Nội"),
}

INVIDIOUS_INSTANCES = [
    "https://inv.nadeko.net",
    "https://invidious.nerdvpn.de",
    "https://yewtu.be",
]

VNEXPRESS_FEEDS = {
    "thoi_su": "https://vnexpress.net/rss/thoi-su.rss",
    "the_gioi": "https://vnexpress.net/rss/the-gioi.rss",
    "cong_nghe": "https://vnexpress.net/rss/so-hoa.rss",
    "kinh_doanh": "https://vnexpress.net/rss/kinh-doanh.rss",
    "the_thao": "https://vnexpress.net/rss/the-thao.rss",
    "giai_tri": "https://vnexpress.net/rss/giai-tri.rss",
    "tin_moi": "https://vnexpress.net/rss/tin-moi-nhat.rss",
}

CONFIG_LOCK = threading.Lock()
SC_CID_CACHE = {"cid": "", "ts": 0.0}


def load_config() -> Dict[str, Any]:
    with CONFIG_LOCK:
        cfg = json.loads(json.dumps(DEFAULT_CONFIG))
        saved: Dict[str, Any] = {}
        if CONFIG_FILE.exists():
            try:
                raw_saved = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
                if isinstance(raw_saved, dict):
                    saved = raw_saved
                    cfg.update(saved)
            except Exception:
                pass
        has_saved_profiles = isinstance(saved.get("llm_profiles"), list) and len(saved["llm_profiles"]) > 0
        if not has_saved_profiles:
            cfg["llm_profiles"] = json.loads(json.dumps(DEFAULT_PROFILES))
            if saved.get("llm_api_key") or saved.get("llm_base_url"):
                burl = str(saved.get("llm_base_url") or DEFAULT_PROFILES[0]["base_url"]).strip()
                mdl = str(saved.get("llm_model") or DEFAULT_PROFILES[0]["model"]).strip()
                low = burl.lower()
                prov = "gemini" if "generativelanguage.googleapis.com" in low else (
                    "openrouter" if "openrouter.ai" in low else (
                        "deepseek" if "deepseek.com" in low else (
                            "anthropic" if "anthropic.com" in low else "groq"
                        )
                    )
                )
                cfg["llm_profiles"][0].update({
                    "name": f"{prov.capitalize()} Migrated",
                    "provider": prov,
                    "base_url": burl,
                    "model": mdl,
                    "api_key": saved.get("llm_api_key") or "",
                    "auth_scheme": "x-api-key" if prov == "anthropic" else "bearer",
                    "raw_endpoint": low.endswith("/chat/completions") or low.endswith("/messages"),
                    "enabled": True,
                })
        idx = int(cfg.get("active_profile_idx") or 0)
        if idx < 0 or idx >= len(cfg["llm_profiles"]):
            idx = 0
        cfg["active_profile_idx"] = idx
        act = cfg["llm_profiles"][idx]
        cfg["llm_base_url"] = act.get("base_url") or ""
        cfg["llm_model"] = act.get("model") or ""
        cfg["llm_api_key"] = act.get("api_key") or ""
        return cfg


def save_config(patch: Dict[str, Any]) -> Dict[str, Any]:
    with CONFIG_LOCK:
        cfg = json.loads(json.dumps(DEFAULT_CONFIG))
        if CONFIG_FILE.exists():
            try:
                saved = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
                if isinstance(saved, dict):
                    cfg.update(saved)
            except Exception:
                pass

        old_profs = cfg.get("llm_profiles") or []
        if "llm_profiles" in patch and isinstance(patch["llm_profiles"], list):
            new_profs = []
            for i, p in enumerate(patch["llm_profiles"][:16]):
                if not isinstance(p, dict):
                    continue
                item = dict(p)
                if item.get("api_key") == "__KEEP__":
                    kept = ""
                    if i < len(old_profs) and old_profs[i].get("provider") == item.get("provider"):
                        kept = old_profs[i].get("api_key", "")
                    else:
                        for op in old_profs:
                            if op.get("provider") == item.get("provider") and op.get("name") == item.get("name") and op.get("api_key"):
                                kept = op["api_key"]
                                break
                        if not kept and i < len(old_profs):
                            kept = old_profs[i].get("api_key", "")
                    item["api_key"] = kept
                new_profs.append(item)
            if new_profs:
                cfg["llm_profiles"] = new_profs

        for k, v in patch.items():
            if k == "llm_profiles" or k not in DEFAULT_CONFIG:
                continue
            if k in ("stt_api_key", "llm_api_key", "wifi_pass", "search_api_key") and v == "__KEEP__":
                continue
            cfg[k] = v

        idx = int(cfg.get("active_profile_idx") or 0)
        if idx < 0 or idx >= len(cfg["llm_profiles"]):
            idx = 0
        cfg["active_profile_idx"] = idx
        if cfg["llm_profiles"]:
            act = cfg["llm_profiles"][idx]
            cfg["llm_base_url"] = act.get("base_url") or ""
            cfg["llm_model"] = act.get("model") or ""
            cfg["llm_api_key"] = act.get("api_key") or ""

        CONFIG_FILE.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
        return cfg


def strip_vn_diacritics(s: str) -> str:
    rep = {
        "à": "a", "á": "a", "ạ": "a", "ả": "a", "ã": "a",
        "â": "a", "ầ": "a", "ấ": "a", "ậ": "a", "ẩ": "a", "ẫ": "a",
        "ă": "a", "ằ": "a", "ắ": "a", "ặ": "a", "ẳ": "a", "ẵ": "a",
        "è": "e", "é": "e", "ẹ": "e", "ẻ": "e", "ẽ": "e",
        "ê": "e", "ề": "e", "ế": "e", "ệ": "e", "ể": "e", "ễ": "e",
        "ì": "i", "í": "i", "ị": "i", "ỉ": "i", "ĩ": "i",
        "ò": "o", "ó": "o", "ọ": "o", "ỏ": "o", "õ": "o",
        "ô": "o", "ồ": "o", "ố": "o", "ộ": "o", "ổ": "o", "ỗ": "o",
        "ơ": "o", "ờ": "o", "ớ": "o", "ợ": "o", "ở": "o", "ỡ": "o",
        "ù": "u", "ú": "u", "ụ": "u", "ủ": "u", "ũ": "u",
        "ư": "u", "ừ": "u", "ứ": "u", "ự": "u", "ử": "u", "ữ": "u",
        "ỳ": "y", "ý": "y", "ỵ": "y", "ỷ": "y", "ỹ": "y",
        "đ": "d",
    }
    return "".join(rep.get(ch, ch) for ch in s.lower())


def http_fetch(
    url: str,
    method: str = "GET",
    headers: Optional[Dict[str, str]] = None,
    body: Optional[bytes] = None,
    timeout: int = 20,
) -> Tuple[int, Dict[str, str], bytes]:
    hdrs = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) SW-NiOh-Sim/1.0"}
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(url, data=body, headers=hdrs, method=method)
    ctx = ssl.create_default_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            resp_headers = {k.lower(): v for k, v in resp.headers.items()}
            return resp.status, resp_headers, resp.read()
    except urllib.error.HTTPError as e:
        raw = e.read() if e.fp else b""
        return e.code, {k.lower(): v for k, v in (e.headers.items() if e.headers else [])}, raw
    except Exception as e:
        return 0, {}, str(e).encode("utf-8")


# ============================================================================
# Multi-Provider LLM Endpoint Resolver & Auth Builder (ESP-Claw style)
# ============================================================================
def _append_query_kv(url: str, key: str, val: str) -> str:
    sep = "&" if "?" in url else "?"
    return f"{url}{sep}{urllib.parse.quote(key)}={urllib.parse.quote(val)}"


def resolve_chat_url(prof: Dict[str, Any]) -> str:
    u = (prof.get("base_url") or "").strip().rstrip("/")
    if not u:
        return ""
    prov = (prof.get("provider") or "custom").lower()
    raw_ep = bool(prof.get("raw_endpoint"))
    if not raw_ep:
        low = u.lower()
        if prov in ("anthropic", "anthropic_compatible", "minimax_global", "minimax_cn"):
            if not low.endswith("/messages"):
                u += "/messages" if low.endswith("/v1") else "/v1/messages"
        elif prov == "gemini_native":
            if ":generatecontent" not in low:
                model = (prof.get("model") or "gemini-2.5-flash").strip()
                if "/models" not in low:
                    u += "/models"
                u += f"/{model}:generateContent"
        else:
            if not low.endswith("/chat/completions"):
                if any(low.endswith(s) for s in ("/v1", "/openai", "/v1beta/openai", "/api/v1", "/ai/v1", "/v2")):
                    u += "/chat/completions"
                else:
                    u += "/v1/chat/completions"

    if (prof.get("auth_scheme") or "bearer") == "query_param" and prof.get("api_key"):
        qname = (prof.get("custom_auth_name") or "key").strip() or "key"
        u = _append_query_kv(u, qname, prof["api_key"])
    return u


def resolve_models_url(prof: Dict[str, Any]) -> str:
    u = (prof.get("base_url") or "").strip().rstrip("/")
    if not u:
        return ""
    for tail in ("/chat/completions", "/messages"):
        if u.lower().endswith(tail):
            u = u[: -len(tail)]
    prov = (prof.get("provider") or "custom").lower()
    low = u.lower()
    if prov == "gemini_native":
        if not low.endswith("/models"):
            u += "/models"
    else:
        if not low.endswith("/models"):
            if any(low.endswith(s) for s in ("/v1", "/openai", "/v1beta/openai", "/api/v1", "/ai/v1", "/v2")):
                u += "/models"
            else:
                u += "/v1/models"
    if (prof.get("auth_scheme") or "bearer") == "query_param" and prof.get("api_key"):
        qname = (prof.get("custom_auth_name") or "key").strip() or "key"
        u = _append_query_kv(u, qname, prof["api_key"])
    return u


def build_auth_headers(prof: Dict[str, Any]) -> Dict[str, str]:
    headers: Dict[str, str] = {}
    key = (prof.get("api_key") or "").strip()
    scheme = (prof.get("auth_scheme") or "bearer").strip()
    prov = (prof.get("provider") or "").lower()
    if not key or scheme == "query_param":
        return headers
    if scheme == "x-api-key":
        headers["x-api-key"] = key
        if prov == "anthropic":
            headers["anthropic-version"] = "2023-06-01"
    elif scheme == "x-goog-api-key":
        headers["x-goog-api-key"] = key
    elif scheme == "custom_header":
        cname = (prof.get("custom_auth_name") or "").strip()
        if cname:
            headers[cname] = key
    else:
        headers["Authorization"] = f"Bearer {key}"
    return headers


def merge_profile_patch(cfg: Dict[str, Any], req_obj: Dict[str, Any]) -> Dict[str, Any]:
    idx = int(req_obj.get("profile_idx") if req_obj.get("profile_idx") is not None else cfg.get("active_profile_idx", 0))
    profs = cfg.get("llm_profiles") or DEFAULT_PROFILES
    base = dict(profs[idx]) if 0 <= idx < len(profs) else dict(DEFAULT_PROFILES[0])
    patch = req_obj.get("profile") or {}
    if isinstance(patch, dict):
        for k, v in patch.items():
            if k == "api_key" and (not v or v == "__KEEP__"):
                continue
            base[k] = v
    return base


def infer_provider_from_key(key: str) -> Optional[Dict[str, str]]:
    k = (key or "").strip()
    if not k:
        return None
    if k.startswith("gsk_"):
        return {"provider": "groq", "base_url": "https://api.groq.com/openai/v1", "model": "llama-3.3-70b-versatile", "auth_scheme": "bearer"}
    if k.startswith("AIza"):
        return {"provider": "gemini", "base_url": "https://generativelanguage.googleapis.com/v1beta/openai", "model": "gemini-2.5-flash", "auth_scheme": "bearer"}
    if k.startswith("sk-or-v1-") or k.startswith("sk-or-"):
        return {"provider": "openrouter", "base_url": "https://openrouter.ai/api/v1", "model": "openai/gpt-4o-mini", "auth_scheme": "bearer"}
    if k.startswith("sk-ant-"):
        return {"provider": "anthropic", "base_url": "https://api.anthropic.com/v1", "model": "claude-sonnet-4-6", "auth_scheme": "x-api-key"}
    if k.startswith("xai-"):
        return {"provider": "custom", "base_url": "https://api.x.ai/v1", "model": "grok-3-mini", "auth_scheme": "bearer"}
    return None


def pick_best_model(models: List[str], provider: str = "") -> str:
    if not models:
        return ""
    skip_kw = ("embed", "whisper", "tts", "dall-e", "imagen", "moderation", "audio", "realtime", "transcribe", "aqa", "babbage", "davinci")
    chat_models = [m for m in models if not any(w in m.lower() for w in skip_kw)] or models
    prov = (provider or "").lower()
    prefs_by_prov = {
        "gemini": ("gemini-2.5-flash", "gemini-2.5-pro", "gemini-2.0-flash", "gemini-1.5-flash"),
        "gemini_native": ("gemini-2.5-flash", "gemini-2.5-pro", "gemini-2.0-flash"),
        "groq": ("llama-3.3-70b-versatile", "openai/gpt-oss-120b", "qwen/qwen3-32b", "llama-3.1-8b-instant"),
        "openrouter": ("google/gemini-2.5-flash", "openai/gpt-4o-mini", "deepseek/deepseek-chat", ":free"),
        "deepseek": ("deepseek-chat", "deepseek-reasoner"),
        "anthropic": ("claude-sonnet-4", "claude-3-7-sonnet", "claude-3-5-sonnet", "claude-3-5-haiku"),
        "openai": ("gpt-4o-mini", "gpt-4o", "gpt-4.1-mini", "gpt-5"),
        "bailian": ("qwen-plus", "qwen-max", "qwen3", "qwen-turbo"),
        "kimi_global": ("kimi-k2", "moonshot-v1-32k", "moonshot-v1-8k"),
        "kimi_cn": ("kimi-k2", "moonshot-v1-32k", "moonshot-v1-8k"),
    }
    for want in prefs_by_prov.get(prov, ()):
        for m in chat_models:
            if want in m.lower():
                return m
    for fallback_kw in ("flash", "4o-mini", "70b", "chat", "sonnet", "plus", "instruct", "qwen", "llama"):
        for m in chat_models:
            if fallback_kw in m.lower():
                return m
    return chat_models[0]


def fetch_models_for_profile(prof: Dict[str, Any]) -> Dict[str, Any]:
    url = resolve_models_url(prof)
    if not url:
        return {"ok": False, "error": "Thiếu Base URL"}
    st, _, raw = http_fetch(url, headers=build_auth_headers(prof), timeout=15)
    if st < 200 or st >= 300:
        return {"ok": False, "error": f"HTTP {st}: {raw.decode('utf-8', 'ignore')[:180]}"}
    try:
        data = json.loads(raw.decode("utf-8", "ignore"))
        arr = data.get("data") if isinstance(data, dict) else None
        if arr is None and isinstance(data, dict):
            arr = data.get("models")
        if arr is None and isinstance(data, list):
            arr = data
        models: List[str] = []
        for it in (arr or [])[:80]:
            if isinstance(it, dict):
                mid = it.get("id") or it.get("name") or ""
                if mid.startswith("models/"):
                    mid = mid[7:]
                if mid:
                    models.append(mid)
            elif isinstance(it, str):
                models.append(it)
        if not models:
            return {"ok": False, "error": "Endpoint /models không trả danh sách model"}
        best = pick_best_model(models, str(prof.get("provider") or ""))
        return {"ok": True, "models": models, "suggested_model": best}
    except Exception as e:
        return {"ok": False, "error": str(e)}


# ============================================================================
# Thuật toán Âm Lịch Hồ Ngọc Đức (ICT+7)
# ============================================================================
def _jd_from_date(dd: int, mm: int, yy: int) -> int:
    a = (14 - mm) // 12
    y = yy + 4800 - a
    m = mm + 12 * a - 3
    jd = dd + (153 * m + 2) // 5 + 365 * y + y // 4 - y // 100 + y // 400 - 32045
    if jd < 2299161:
        jd = dd + (153 * m + 2) // 5 + 365 * y + y // 4 - 32083
    return jd


def _new_moon(k: int) -> float:
    t = k / 1236.85
    t2 = t * t
    t3 = t2 * t
    dr = math.pi / 180.0
    jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * t2 - 0.000000155 * t3
    jd1 += 0.00033 * math.sin((166.56 + 132.87 * t - 0.009173 * t2) * dr)
    m = 359.2242 + 29.10535608 * k - 0.0000333 * t2 - 0.00000347 * t3
    mpr = 306.0253 + 385.81691806 * k + 0.0107306 * t2 + 0.00001236 * t3
    f = 21.2964 + 390.67050646 * k - 0.0016528 * t2 - 0.00000239 * t3
    c1 = (0.1734 - 0.000393 * t) * math.sin(m * dr) + 0.0021 * math.sin(2 * dr * m)
    c1 -= 0.4068 * math.sin(mpr * dr) + 0.0161 * math.sin(dr * 2 * mpr) - 0.0004 * math.sin(dr * 3 * mpr)
    c1 += 0.0104 * math.sin(dr * 2 * f) - 0.0051 * math.sin(dr * (m + mpr))
    c1 -= 0.0074 * math.sin(dr * (m - mpr)) + 0.0004 * math.sin(dr * (2 * f + m))
    c1 -= 0.0004 * math.sin(dr * (2 * f - m)) - 0.0006 * math.sin(dr * (2 * f + mpr))
    c1 += 0.0010 * math.sin(dr * (2 * f - mpr)) + 0.0005 * math.sin(dr * (2 * mpr + m))
    deltat = (
        0.001 + 0.000839 * t + 0.0002261 * t2 - 0.00000845 * t3 - 0.000000081 * t * t3
        if t < -11
        else -0.000278 + 0.000265 * t + 0.000262 * t2
    )
    return jd1 + c1 - deltat


def _get_new_moon_day(k: int, tz: int = 7) -> int:
    return int(_new_moon(k) + 0.5 + tz / 24.0)


def _get_sun_longitude(jdn: int, tz: int = 7) -> int:
    t = (jdn - 2451545.5 - tz / 24.0) / 36525.0
    t2 = t * t
    dr = math.pi / 180.0
    m = 357.52910 + 35999.05030 * t - 0.0001559 * t2 - 0.00000048 * t * t2
    l0 = 280.46645 + 36000.76983 * t + 0.0003032 * t2
    dl = (1.914600 - 0.004817 * t - 0.000014 * t2) * math.sin(dr * m)
    dl += (0.019993 - 0.000101 * t) * math.sin(dr * 2 * m) + 0.000290 * math.sin(dr * 3 * m)
    l_deg = (l0 + dl) * dr
    l_deg -= math.pi * 2 * int(l_deg / (math.pi * 2))
    return int(l_deg / math.pi * 6)


def _get_lunar_month_11(yy: int, tz: int = 7) -> int:
    off = _jd_from_date(31, 12, yy) - 2415021
    k = int(off / 29.530588853)
    nm = _get_new_moon_day(k, tz)
    if _get_sun_longitude(nm, tz) >= 9:
        nm = _get_new_moon_day(k - 1, tz)
    return nm


def _get_leap_month_offset(a11: int, tz: int = 7) -> int:
    k = int((a11 - 2415021.076998695) / 29.530588853 + 0.5)
    last = 0
    i = 1
    arc = _get_sun_longitude(_get_new_moon_day(k + i, tz), tz)
    while True:
        last = arc
        i += 1
        arc = _get_sun_longitude(_get_new_moon_day(k + i, tz), tz)
        if not (arc != last and i < 14):
            break
    return i - 1


def solar_to_lunar(dd: int, mm: int, yy: int, tz: int = 7) -> Dict[str, Any]:
    day_num = _jd_from_date(dd, mm, yy)
    k = int((day_num - 2415021.076998695) / 29.530588853)
    month_start = _get_new_moon_day(k + 1, tz)
    if month_start > day_num:
        month_start = _get_new_moon_day(k, tz)
    a11 = _get_lunar_month_11(yy, tz)
    b11 = a11
    if a11 >= month_start:
        lunar_year = yy
        a11 = _get_lunar_month_11(yy - 1, tz)
    else:
        lunar_year = yy + 1
        b11 = _get_lunar_month_11(yy + 1, tz)
    lunar_day = day_num - month_start + 1
    diff = int((month_start - a11) / 29)
    lunar_leap = False
    lunar_month = diff + 11
    if b11 - a11 > 365:
        leap_off = _get_leap_month_offset(a11, tz)
        if diff >= leap_off:
            lunar_month = diff + 10
            if diff == leap_off:
                lunar_leap = True
    if lunar_month > 12:
        lunar_month -= 12
    if lunar_month >= 11 and diff < 4:
        lunar_year -= 1

    cans = ["Giáp", "Ất", "Bính", "Đinh", "Mậu", "Kỷ", "Canh", "Tân", "Nhâm", "Quý"]
    chis = ["Tý", "Sửu", "Dần", "Mão", "Thìn", "Tỵ", "Ngọ", "Mùi", "Thân", "Dậu", "Tuất", "Hợi"]
    hoang_dao_map = {
        0: "Tý, Sửu, Mão, Ngọ, Thân, Dậu",
        1: "Dần, Mão, Tỵ, Thân, Tuất, Hợi",
        2: "Tý, Sửu, Thìn, Tỵ, Mùi, Tuất",
        3: "Tý, Dần, Mão, Ngọ, Mùi, Dậu",
        4: "Dần, Thìn, Tỵ, Thân, Dậu, Hợi",
        5: "Sửu, Thìn, Ngọ, Mùi, Tuất, Hợi",
    }
    return {
        "lunar_day": lunar_day,
        "lunar_month": lunar_month,
        "lunar_year": lunar_year,
        "lunar_leap": lunar_leap,
        "can_chi_year": f"{cans[(lunar_year + 6) % 10]} {chis[(lunar_year + 8) % 12]}",
        "can_chi_day": f"{cans[(day_num + 9) % 10]} {chis[(day_num + 1) % 12]}",
        "gio_hoang_dao": hoang_dao_map[(day_num + 1) % 6],
    }


def wmo_code_to_vi(code: int) -> str:
    if code == 0:
        return "Trời quang"
    if code in (1, 2):
        return "Ít mây"
    if code == 3:
        return "Nhiều mây"
    if code in (45, 48):
        return "Sương mù"
    if 51 <= code <= 57:
        return "Mưa phùn"
    if 61 <= code <= 67 or 80 <= code <= 82:
        return "Có mưa"
    if code >= 95:
        return "Mưa dông"
    return "Có mây"


# ============================================================================
# Media & Search Resolvers (YouTube/SoundCloud Default — NO forced Radio!)
# ============================================================================
def scrape_soundcloud_client_id(cfg: Dict[str, Any]) -> str:
    cid = (cfg.get("soundcloud_client_id") or "").strip()
    if cid:
        return cid
    if SC_CID_CACHE["cid"] and (time.time() - SC_CID_CACHE["ts"]) < 3600:
        return SC_CID_CACHE["cid"]
    try:
        st, _, raw = http_fetch("https://soundcloud.com", timeout=10)
        if st == 200:
            page = raw.decode("utf-8", "ignore")
            for js_url in re.findall(r"https://a-v2\.sndcdn\.com/assets/[^\"']+\.js", page)[:5]:
                jst, _, jraw = http_fetch(js_url, timeout=10)
                if jst == 200:
                    m = re.search(r'client_id["\']?\s*[:=]\s*["\']([A-Za-z0-9]{16,48})["\']', jraw.decode("utf-8", "ignore"))
                    if m:
                        SC_CID_CACHE["cid"] = m.group(1)
                        SC_CID_CACHE["ts"] = time.time()
                        return m.group(1)
    except Exception:
        pass
    return ""


def _resolve_yt_dlp(query: str) -> Optional[Tuple[str, str]]:
    exe = shutil.which("yt-dlp")
    cmd_base = [exe] if exe else None
    if not cmd_base:
        try:
            subprocess.run(["python", "-m", "yt_dlp", "--version"], capture_output=True, timeout=3, check=True)
            cmd_base = ["python", "-m", "yt_dlp"]
        except Exception:
            cmd_base = None
    if not cmd_base:
        return None
    target = query if query.startswith(("http://", "https://")) else f"ytsearch1:{query}"
    try:
        r = subprocess.run(
            [*cmd_base, "--no-playlist", "-f", "bestaudio[ext=m4a]/bestaudio", "--print", "%(title)s\n%(url)s", target],
            capture_output=True,
            text=True,
            timeout=15,
        )
        lines = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
        if len(lines) >= 2 and lines[-1].startswith("http"):
            return lines[0], lines[-1]
    except Exception:
        pass
    return None


def _resolve_music_source(query: str, platform: str, cfg: Dict[str, Any]) -> Dict[str, Any]:
    q_clean = query.strip() or "nhac chill lo-fi"
    folded = strip_vn_diacritics(q_clean)
    explicit_radio = any(k in folded for k in ("vov", "radio", "bat dai", "kenh dai"))

    plat = (platform or "youtube").lower().strip()
    if plat == "radio" and not explicit_radio:
        plat = "youtube"
    if not platform and explicit_radio:
        plat = "radio"

    if plat == "radio":
        url, title = VOV_CHANNELS.get(
            next((k for k in VOV_CHANNELS if k in folded), "vov3"),
            (q_clean if q_clean.startswith("http") else VOV_CHANNELS["vov3"][0], f"Radio {q_clean}"),
        )
        return {"ok": True, "platform": "radio", "source_tag": "[Radio]", "title": title, "direct_url": url}

    proxy_base = (cfg.get("media_proxy_url") or "").strip().rstrip("/")
    if proxy_base:
        surl = f"{proxy_base}/stream?q={urllib.parse.quote(q_clean)}&source={urllib.parse.quote(plat)}"
        tag = "[SoundCloud]" if plat == "soundcloud" else "[YouTube]"
        return {"ok": True, "platform": plat, "source_tag": tag, "title": q_clean, "direct_url": surl}

    # SoundCloud search/resolve progressive MP3
    if plat == "soundcloud":
        cid = scrape_soundcloud_client_id(cfg)
        if cid:
            try:
                api_url = (
                    f"https://api-v2.soundcloud.com/resolve?url={urllib.parse.quote(q_clean)}&client_id={urllib.parse.quote(cid)}"
                    if "soundcloud.com" in q_clean
                    else f"https://api-v2.soundcloud.com/search/tracks?q={urllib.parse.quote(q_clean)}&limit=5&client_id={urllib.parse.quote(cid)}"
                )
                st, _, raw = http_fetch(api_url, timeout=12)
                if st == 200:
                    data = json.loads(raw.decode("utf-8", "ignore"))
                    tracks = data.get("collection", []) if "collection" in data else [data]
                    for tr in tracks:
                        title = tr.get("title") or q_clean
                        for tc in (tr.get("media") or {}).get("transcodings", []):
                            fmt = tc.get("format") or {}
                            if fmt.get("protocol") == "progressive" and tc.get("url"):
                                st2, _, raw2 = http_fetch(f"{tc['url']}?client_id={urllib.parse.quote(cid)}", timeout=10)
                                if st2 == 200:
                                    mp3_url = json.loads(raw2.decode("utf-8", "ignore")).get("url")
                                    if mp3_url:
                                        return {"ok": True, "platform": "soundcloud", "source_tag": "[SoundCloud]", "title": title, "direct_url": mp3_url}
            except Exception:
                pass

    # YouTube: 1) yt-dlp cục bộ nếu có 2) Piped instances cấu hình 3) Invidious public 4) Fallback SoundCloud
    ytd = _resolve_yt_dlp(q_clean)
    if ytd:
        return {"ok": True, "platform": "youtube", "source_tag": "[YouTube]", "title": ytd[0], "direct_url": ytd[1]}

    piped_list = [h.strip().rstrip("/") for h in (cfg.get("piped_instances") or DEFAULT_PIPED).split(",") if h.strip()]
    for inst in piped_list:
        try:
            st, _, raw = http_fetch(f"{inst}/search?q={urllib.parse.quote(q_clean)}&filter=music_songs", timeout=8)
            if st != 200:
                st, _, raw = http_fetch(f"{inst}/search?q={urllib.parse.quote(q_clean)}&filter=videos", timeout=8)
            if st == 200:
                items = json.loads(raw.decode("utf-8", "ignore")).get("items") or []
                for it in items:
                    u = it.get("url") or ""
                    if "/watch?v=" in u:
                        vid = u.split("v=")[-1].split("&")[0]
                        st2, _, raw2 = http_fetch(f"{inst}/streams/{vid}", timeout=8)
                        if st2 == 200:
                            sdata = json.loads(raw2.decode("utf-8", "ignore"))
                            streams = sdata.get("audioStreams") or []
                            if streams:
                                pick = next((s for s in streams if "mpeg" in (s.get("mimeType") or "").lower() or "mp4" in (s.get("mimeType") or "").lower()), streams[0])
                                if pick.get("url"):
                                    return {"ok": True, "platform": "youtube", "source_tag": "[YouTube]", "title": sdata.get("title") or it.get("title") or q_clean, "direct_url": pick["url"]}
        except Exception:
            continue

    for inv in INVIDIOUS_INSTANCES:
        try:
            st, _, raw = http_fetch(f"{inv}/api/v1/search?q={urllib.parse.quote(q_clean)}&type=video", timeout=8)
            if st == 200:
                arr = json.loads(raw.decode("utf-8", "ignore"))
                if arr and isinstance(arr, list) and arr[0].get("videoId"):
                    vid = arr[0]["videoId"]
                    st2, _, raw2 = http_fetch(f"{inv}/api/v1/videos/{vid}", timeout=8)
                    if st2 == 200:
                        vdata = json.loads(raw2.decode("utf-8", "ignore"))
                        fmts = [f for f in (vdata.get("adaptiveFormats") or []) if str(f.get("type") or "").startswith("audio/") and f.get("url")]
                        if fmts:
                            return {"ok": True, "platform": "youtube", "source_tag": "[YouTube]", "title": vdata.get("title") or q_clean, "direct_url": fmts[0]["url"]}
        except Exception:
            continue

    # SoundCloud auto fallback trước khi báo lỗi (tuyệt đối không tự nhảy sang Radio VOV!)
    if plat != "soundcloud":
        sc_res = _resolve_music_source(q_clean, "soundcloud", cfg)
        if sc_res.get("ok"):
            return sc_res

    return {
        "ok": False,
        "platform": plat,
        "source_tag": "[YouTube]" if plat == "youtube" else "[SoundCloud]",
        "title": q_clean,
        "error": "Không tìm được luồng nhạc từ YouTube/SoundCloud công cộng. Hãy cài yt-dlp trên PC hoặc nhập Media Gateway URL trong tab Dịch vụ & Tools.",
    }


def _web_search(kw: str, cfg: Dict[str, Any]) -> Dict[str, Any]:
    eng = (cfg.get("search_engine") or "ddg_wiki").strip()
    skey = (cfg.get("search_api_key") or "").strip()

    if eng == "bocha" and skey:
        st, _, raw = http_fetch(
            "https://api.bochaai.com/v1/web-search",
            method="POST",
            headers={"Authorization": f"Bearer {skey}", "Content-Type": "application/json"},
            body=json.dumps({"query": kw, "summary": True, "count": 3}).encode("utf-8"),
            timeout=12,
        )
        if st == 200:
            data = json.loads(raw.decode("utf-8", "ignore"))
            pages = ((data.get("data") or {}).get("webPages") or {}).get("value") or []
            return {"ok": True, "cong_cu": "Bocha Search", "ket_qua": [{"tieu_de": p.get("name"), "mo_ta": p.get("snippet") or p.get("summary")} for p in pages[:3]]}

    if eng == "tavily" and skey:
        st, _, raw = http_fetch(
            "https://api.tavily.com/search",
            method="POST",
            headers={"Content-Type": "application/json"},
            body=json.dumps({"api_key": skey, "query": kw, "max_results": 3, "include_answer": True}).encode("utf-8"),
            timeout=12,
        )
        if st == 200:
            data = json.loads(raw.decode("utf-8", "ignore"))
            return {"ok": True, "cong_cu": "Tavily AI", "tom_tat": data.get("answer") or "", "ket_qua": data.get("results") or []}

    if eng == "brave" and skey:
        st, _, raw = http_fetch(
            f"https://api.search.brave.com/res/v1/web/search?count=3&q={urllib.parse.quote(kw)}",
            headers={"X-Subscription-Token": skey},
            timeout=12,
        )
        if st == 200:
            res = ((json.loads(raw.decode("utf-8", "ignore")).get("web") or {}).get("results") or [])[:3]
            return {"ok": True, "cong_cu": "Brave Search", "ket_qua": [{"tieu_de": r.get("title"), "mo_ta": r.get("description")} for r in res]}

    if eng == "serper" and skey:
        st, _, raw = http_fetch(
            "https://google.serper.dev/search",
            method="POST",
            headers={"X-API-KEY": skey, "Content-Type": "application/json"},
            body=json.dumps({"q": kw, "gl": "vn", "hl": "vi"}).encode("utf-8"),
            timeout=12,
        )
        if st == 200:
            org = (json.loads(raw.decode("utf-8", "ignore")).get("organic") or [])[:3]
            return {"ok": True, "cong_cu": "Serper Google", "ket_qua": [{"tieu_de": r.get("title"), "mo_ta": r.get("snippet")} for r in org]}

    if eng == "searxng" and cfg.get("searxng_url"):
        base = cfg["searxng_url"].strip().rstrip("/")
        st, _, raw = http_fetch(f"{base}/search?format=json&language=vi&q={urllib.parse.quote(kw)}", timeout=12)
        if st == 200:
            res = (json.loads(raw.decode("utf-8", "ignore")).get("results") or [])[:3]
            return {"ok": True, "cong_cu": "SearXNG", "ket_qua": [{"tieu_de": r.get("title"), "mo_ta": r.get("content")} for r in res]}

    # Default free: DuckDuckGo Instant Answer + Wikipedia Tiếng Việt
    ddg_abs = ""
    st_d, _, raw_d = http_fetch(f"https://api.duckduckgo.com/?format=json&no_html=1&skip_disambig=1&q={urllib.parse.quote(kw)}", timeout=10)
    if st_d == 200:
        ddg_abs = json.loads(raw_d.decode("utf-8", "ignore")).get("AbstractText") or ""

    url = f"https://vi.wikipedia.org/api/rest_v1/page/summary/{urllib.parse.quote(kw.replace(' ', '_'))}"
    st, _, raw = http_fetch(url, timeout=10)
    if st != 200:
        st_s, _, raw_s = http_fetch(f"https://vi.wikipedia.org/w/api.php?action=opensearch&search={urllib.parse.quote(kw)}&limit=1&format=json", timeout=10)
        if st_s == 200:
            arr = json.loads(raw_s.decode("utf-8", "ignore"))
            if len(arr) >= 2 and arr[1]:
                st, _, raw = http_fetch(f"https://vi.wikipedia.org/api/rest_v1/page/summary/{urllib.parse.quote(arr[1][0].replace(' ', '_'))}", timeout=10)

    out: Dict[str, Any] = {"ok": True, "cong_cu": "DuckDuckGo + Wikipedia VI"}
    if ddg_abs:
        out["duckduckgo"] = ddg_abs
    if st == 200:
        w = json.loads(raw.decode("utf-8", "ignore"))
        out["tieu_de"] = w.get("title") or kw
        out["tom_tat"] = (w.get("extract") or "")[:600]
    if not ddg_abs and st != 200:
        return {"ok": False, "error": f"Không tìm thấy thông tin phù hợp cho '{kw}'"}
    return out


# ============================================================================
# 10 Công cụ MCP 100% Tiếng Việt
# ============================================================================
def build_mcp_tools_schema() -> List[Dict[str, Any]]:
    def fn(name: str, desc: str, props: Dict[str, Any], req: List[str]) -> Dict[str, Any]:
        return {"type": "function", "function": {"name": name, "description": desc, "parameters": {"type": "object", "properties": props, "required": req}}}

    return [
        fn("xem_thoi_tiet", "Tra cứu thời tiết tại Việt Nam hoặc thế giới và cập nhật lên đồng hồ.", {"dia_diem": {"type": "string"}}, ["dia_diem"]),
        fn("xem_thoi_gian_lich_am", "Xem giờ ICT-7, ngày Dương lịch, ngày Âm lịch Việt Nam, Can Chi và Giờ Hoàng Đạo.", {}, []),
        fn("doc_tin_tuc", "Đọc tin tức mới nhất tiếng Việt từ VNExpress.", {"chu_de": {"type": "string"}, "so_luong": {"type": "integer"}}, ["chu_de"]),
        fn("tra_cuu_thong_tin", "Tìm kiếm thông tin trên Web (DuckDuckGo/Tavily/Brave/Serper/SearXNG) và Wikipedia Tiếng Việt.", {"tu_khoa": {"type": "string"}}, ["tu_khoa"]),
        fn("phat_nhac_truc_tuyen", "Phát nhạc trực tuyến YouTube hoặc SoundCloud (CHỈ chọn radio khi người dùng nói rõ Radio/VOV).", {"tu_khoa_hoac_url": {"type": "string"}, "nen_tang": {"type": "string", "enum": ["youtube", "soundcloud", "radio"]}}, ["tu_khoa_hoac_url"]),
        fn("xem_video_truc_tuyen", "Mở và phát luồng video MJPEG 240x240 hoặc 240x180 trực tiếp lên màn hình ST7789.", {"tu_khoa_hoac_url": {"type": "string"}, "do_phan_giai": {"type": "string", "enum": ["240x240", "240x180"]}}, ["tu_khoa_hoac_url"]),
        fn("suy_luan_cao", "Phân tích suy luận sâu từng bước cho câu hỏi phức tạp.", {"van_de_phuc_tap": {"type": "string"}}, ["van_de_phuc_tap"]),
        fn("dieu_khien_thiet_bi", "Điều khiển phần cứng S.W.Ni-Oh (loa, độ nhạy mic, độ sáng, xoay LCD).", {"loai": {"type": "string", "enum": ["am_luong", "do_nhay_mic", "do_sang_man_hinh", "xoay_man_hinh"]}, "gia_tri": {"type": "integer"}}, ["loai", "gia_tri"]),
        fn("gui_lenh_esp_now", "Gửi gói ESP-NOW 2 chiều tới node khác.", {"loai_goi": {"type": "string", "enum": ["CONTROL", "ALERT", "SYNC_CLOCK"]}, "noi_dung": {"type": "string"}, "mac_dich": {"type": "string"}}, ["loai_goi", "noi_dung"]),
        fn("hen_gio_pomodoro", "Đặt bộ đếm giờ Pomodoro và nhắc bằng giọng nói.", {"so_phut": {"type": "number"}, "ghi_chu": {"type": "string"}}, ["so_phut"]),
    ]


def execute_mcp_tool(tool_name: str, args: Dict[str, Any], cfg: Dict[str, Any]) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
    events: List[Dict[str, Any]] = []

    if tool_name == "xem_thoi_tiet":
        place = str(args.get("dia_diem") or "Hà Nội").strip()
        norm = strip_vn_diacritics(place)
        lat, lon, display_name = None, None, place
        for k, (la, lo, label) in VN_PROVINCES.items():
            if k in norm or norm in k:
                lat, lon, display_name = la, lo, label
                break
        if lat is None:
            st, _, raw = http_fetch(f"https://geocoding-api.open-meteo.com/v1/search?name={urllib.parse.quote(place)}&count=1&language=vi", timeout=10)
            if st == 200:
                res = (json.loads(raw.decode("utf-8", "ignore")).get("results") or [None])[0]
                if res:
                    lat, lon, display_name = res["latitude"], res["longitude"], res.get("name") or place
        if lat is None:
            lat, lon, display_name = 21.0285, 105.8542, "Hà Nội"
        st, _, raw = http_fetch(f"https://api.open-meteo.com/v1/forecast?latitude={lat:.4f}&longitude={lon:.4f}&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=Asia%2FBangkok", timeout=10)
        if st == 200:
            cur = json.loads(raw.decode("utf-8", "ignore")).get("current") or {}
            temp, hum, wmo, wind = cur.get("temperature_2m", 28.0), cur.get("relative_humidity_2m", 75), int(cur.get("weather_code", 1)), cur.get("wind_speed_10m", 8.0)
            cond = wmo_code_to_vi(wmo)
            events.append({"type": "weather_update", "city": display_name, "temp_c": round(float(temp), 1), "humidity": int(hum), "condition": cond, "wmo_code": wmo})
            return {"dia_diem": display_name, "nhiet_do_c": round(float(temp), 1), "do_am_phan_tram": int(hum), "gio_kmh": wind, "mo_ta": cond}, events
        return {"error": f"HTTP {st}"}, events

    if tool_name == "xem_thoi_gian_lich_am":
        now = datetime.now(VN_TZ)
        lunar = solar_to_lunar(now.day, now.month, now.year, 7)
        thu_vi = ["Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy", "Chủ Nhật"][now.weekday()]
        return {
            "gio_hien_tai": now.strftime("%H:%M:%S"),
            "duong_lich": f"{thu_vi}, {now.day:02d}/{now.month:02d}/{now.year}",
            "am_lich": f"{lunar['lunar_day']:02d}/{lunar['lunar_month']:02d}/{lunar['lunar_year']}" + (" (Nhuận)" if lunar["lunar_leap"] else ""),
            "can_chi_nam": lunar["can_chi_year"],
            "can_chi_ngay": lunar["can_chi_day"],
            "gio_hoang_dao": lunar["gio_hoang_dao"],
        }, events

    if tool_name == "doc_tin_tuc":
        topic = strip_vn_diacritics(str(args.get("chu_de") or "thoi_su")).replace(" ", "_")
        count = max(1, min(5, int(args.get("so_luong") or 3)))
        st, _, raw = http_fetch(VNEXPRESS_FEEDS.get(topic, VNEXPRESS_FEEDS["tin_moi"]), timeout=12)
        if st != 200:
            return {"error": f"HTTP {st}"}, events
        root = ET.fromstring(raw.decode("utf-8", "ignore"))
        items = []
        for it in root.findall("./channel/item")[:count]:
            title = (it.findtext("title") or "").strip()
            desc = re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", html.unescape(it.findtext("description") or ""))).strip()[:160]
            items.append({"tieu_de": title, "tom_tat": desc})
        return {"chu_de": topic, "nguon": "VNExpress RSS", "tin_tuc": items}, events

    if tool_name == "tra_cuu_thong_tin":
        return _web_search(str(args.get("tu_khoa") or "").strip(), cfg), events

    if tool_name == "phat_nhac_truc_tuyen":
        q = str(args.get("tu_khoa_hoac_url") or "").strip()
        plat = str(args.get("nen_tang") or "youtube").strip().lower()
        res = _resolve_music_source(q, plat, cfg)
        if not res.get("ok"):
            return {"ok": False, "error": res.get("error")}, events
        play_url = f"/api/music_stream?q={urllib.parse.quote(q)}&platform={urllib.parse.quote(res['platform'])}"
        events.append({"type": "media_play", "kind": "AUDIO_MP3", "source_tag": res["source_tag"], "title": res["title"], "play_url": play_url})
        return {"ok": True, "trang_thai": "dang_phat_nhac", "nguon": res["source_tag"], "bai_hat": res["title"], "stream_url": play_url}, events

    if tool_name == "xem_video_truc_tuyen":
        q = str(args.get("tu_khoa_hoac_url") or "S.W.Ni-Oh Stream").strip()
        res = str(args.get("do_phan_giai") or cfg.get("video_res") or "240x240").strip()
        fps = int(cfg.get("video_fps") or 12)
        qlt = int(cfg.get("video_jpeg_quality") or 75)
        if res not in ("240x240", "240x180"):
            res = "240x240"
        proxy_base = (cfg.get("media_proxy_url") or "").strip().rstrip("/")
        if q.startswith(("http://", "https://")):
            mjpeg_url = f"/api/mjpeg_proxy?url={urllib.parse.quote(q, safe='')}"
            mode = "direct_url"
        elif proxy_base:
            target = f"{proxy_base}/video?q={urllib.parse.quote(q)}&source=youtube&res={res}&fps={fps}&qlt={qlt}"
            mjpeg_url = f"/api/mjpeg_proxy?url={urllib.parse.quote(target, safe='')}"
            mode = "media_proxy"
        else:
            mjpeg_url = f"/api/demo_mjpeg?q={urllib.parse.quote(q)}&res={res}&fps={fps}"
            mode = "simulator_demo_mjpeg"
        events.append({"type": "media_play", "kind": "VIDEO_MJPEG", "source_tag": "[Video]", "title": q, "resolution": res, "play_url": mjpeg_url, "mode": mode})
        return {"ok": True, "trang_thai": "dang_phat_video", "tieu_de": q, "do_phan_giai": res, "mjpeg_url": mjpeg_url}, events

    if tool_name == "suy_luan_cao":
        prob = str(args.get("van_de_phuc_tap") or "").strip()
        ans, _ = run_llm_single_turn(cfg, f"Hãy suy luận từng bước thật rõ ràng bằng tiếng Việt ngắn gọn:\n{prob}", max_tokens=900, temperature=0.3)
        return {"ket_qua_suy_luan": ans}, events

    if tool_name == "dieu_khien_thiet_bi":
        kind = str(args.get("loai") or "").strip()
        val = int(args.get("gia_tri") or 0)
        if kind == "am_luong":
            val = max(0, min(100, val))
        elif kind == "do_nhay_mic":
            val = max(1, min(10, val))
        elif kind == "do_sang_man_hinh":
            val = max(5, min(100, val))
        elif kind == "xoay_man_hinh" and val not in (0, 90, 180, 270):
            val = 0
        events.append({"type": "device_control", "control": kind, "value": val})
        return {"ok": True, "loai": kind, "gia_tri_moi": val}, events

    if tool_name == "gui_lenh_esp_now":
        pkt_type = str(args.get("loai_goi") or "CONTROL").upper()
        payload = str(args.get("noi_dung") or "")
        mac = str(args.get("mac_dich") or "FF:FF:FF:FF:FF:FF")
        events.append({"type": "espnow_tx", "cmd": pkt_type, "payload": payload, "mac": mac})
        return {"ok": True, "loai_goi": pkt_type, "mac_dich": mac, "noi_dung": payload}, events

    if tool_name == "hen_gio_pomodoro":
        mins = float(args.get("so_phut") or 25.0)
        secs = max(3, int(round(mins * 60)))
        note = str(args.get("ghi_chu") or "Hết giờ tập trung!")
        events.append({"type": "pomodoro_start", "seconds": secs, "minutes": mins, "note": note})
        return {"ok": True, "so_giay": secs, "ghi_chu": note}, events

    return {"error": f"Không có công cụ '{tool_name}'"}, events


def extract_emotion(raw_text: str) -> Tuple[str, str]:
    txt = (raw_text or "").strip()
    for tag in EMOTION_TAGS:
        if txt.startswith(tag):
            return tag.strip("[]"), txt[len(tag):].strip()
    return "binh_thuong", txt


def _call_profile_once(
    prof: Dict[str, Any],
    messages: List[Dict[str, Any]],
    tools: Optional[List[Dict[str, Any]]],
    max_tokens: int,
    temperature: float,
) -> Tuple[int, Dict[str, Any]]:
    url = resolve_chat_url(prof)
    prov = (prof.get("provider") or "custom").lower()
    model = (prof.get("model") or "").strip()
    if (not url) and prof.get("api_key"):
        inf = infer_provider_from_key(str(prof.get("api_key")))
        if inf:
            prof = dict(prof, provider=inf["provider"], base_url=inf["base_url"], auth_scheme=inf["auth_scheme"], raw_endpoint=False)
            url = resolve_chat_url(prof)
            prov = inf["provider"]
            if not model:
                model = inf["model"]
    if (not model or model.lower() == "auto") and url:
        mres = fetch_models_for_profile(prof)
        if mres.get("suggested_model"):
            model = mres["suggested_model"]
            prof["model"] = model
    if not url:
        return 400, {"error": "Thiếu Base URL"}
    if not prof.get("api_key") and prov != "ollama":
        return 400, {"error": f"Profile '{prof.get('name')}' chưa có API Key"}

    hdrs = build_auth_headers(prof)
    hdrs["Content-Type"] = "application/json"

    if prov in ("anthropic", "anthropic_compatible", "minimax_global", "minimax_cn"):
        sys_txt = ""
        amsgs = []
        for m in messages:
            if m.get("role") == "system":
                sys_txt = m.get("content") or ""
            else:
                amsgs.append({"role": "assistant" if m.get("role") == "assistant" else "user", "content": m.get("content") or ""})
        payload = {"model": model, "system": sys_txt, "messages": amsgs, "max_tokens": max_tokens}
        st, _, raw = http_fetch(url, method="POST", headers=hdrs, body=json.dumps(payload).encode("utf-8"), timeout=30)
        if st < 200 or st >= 300:
            return st, {"error": raw.decode("utf-8", "ignore")[:240]}
        data = json.loads(raw.decode("utf-8", "ignore"))
        txt = ((data.get("content") or [{}])[0]).get("text") or ""
        return st, {"message": {"content": txt, "tool_calls": []}}

    if prov == "gemini_native":
        merged = "\n".join(f"{m.get('role')}: {m.get('content')}" for m in messages if m.get("content"))
        payload = {"contents": [{"parts": [{"text": merged}]}]}
        st, _, raw = http_fetch(url, method="POST", headers=hdrs, body=json.dumps(payload).encode("utf-8"), timeout=30)
        if st < 200 or st >= 300:
            return st, {"error": raw.decode("utf-8", "ignore")[:240]}
        data = json.loads(raw.decode("utf-8", "ignore"))
        c0 = (data.get("candidates") or [{}])[0]
        parts = ((c0.get("content") or {}).get("parts") or [{}])
        return st, {"message": {"content": parts[0].get("text") or "", "tool_calls": []}}

    body: Dict[str, Any] = {
        "model": model,
        "messages": messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
    }
    if tools:
        body["tools"] = tools
        body["tool_choice"] = "auto"

    st, _, raw = http_fetch(url, method="POST", headers=hdrs, body=json.dumps(body).encode("utf-8"), timeout=30)
    if st < 200 or st >= 300:
        return st, {"error": raw.decode("utf-8", "ignore")[:260]}
    data = json.loads(raw.decode("utf-8", "ignore"))
    msg = ((data.get("choices") or [{}])[0]).get("message") or {}
    return st, {"message": msg}


def run_llm_single_turn(cfg: Dict[str, Any], prompt: str, max_tokens: int = 450, temperature: float = 0.5) -> Tuple[str, int]:
    profs = cfg.get("llm_profiles") or DEFAULT_PROFILES
    idx = min(int(cfg.get("active_profile_idx") or 0), len(profs) - 1)
    st, res = _call_profile_once(
        profs[idx],
        [{"role": "system", "content": "Bạn là S.W.Ni-Oh, trợ lý AI tiếng Việt ngắn gọn."}, {"role": "user", "content": prompt}],
        None,
        max_tokens,
        temperature,
    )
    if st < 200 or st >= 300:
        return f"Lỗi LLM (HTTP {st}): {res.get('error', '')}", st
    return (res["message"].get("content") or "").strip(), st


def run_chat_with_tools(cfg: Dict[str, Any], user_text: str, history: List[Dict[str, str]]) -> Dict[str, Any]:
    profs = cfg.get("llm_profiles") or DEFAULT_PROFILES
    primary = min(int(cfg.get("active_profile_idx") or 0), len(profs) - 1)
    order = [primary]
    if cfg.get("auto_fallback", True):
        for i, p in enumerate(profs):
            if i != primary and p.get("enabled") and p.get("base_url"):
                order.append(i)

    base_msgs: List[Dict[str, Any]] = [{"role": "system", "content": SYSTEM_PROMPT_VI}]
    for h in history[-8:]:
        if h.get("role") in ("user", "assistant") and h.get("content"):
            base_msgs.append({"role": h["role"], "content": h["content"]})
    base_msgs.append({"role": "user", "content": user_text})

    tools = build_mcp_tools_schema()
    last_err = "Chưa cấu hình LLM API Key."

    for pidx in order:
        prof = profs[pidx]
        if not prof.get("api_key") and (prof.get("provider") or "").lower() != "ollama":
            continue
        msgs = list(base_msgs)
        all_events: List[Dict[str, Any]] = []
        tool_logs: List[Dict[str, Any]] = []
        failed_this_profile = False

        for _ in range(5):
            st, res = _call_profile_once(prof, msgs, tools, 500, 0.65)
            if st < 200 or st >= 300:
                last_err = f"[{prof.get('name')}] HTTP {st}: {res.get('error')}"
                failed_this_profile = True
                break

            msg = res["message"]
            tcalls = msg.get("tool_calls") or []
            if not tcalls:
                raw_reply = (msg.get("content") or "").strip()
                emotion, clean_reply = extract_emotion(raw_reply)
                return {
                    "ok": True,
                    "profile_used": prof.get("name"),
                    "emotion": emotion,
                    "reply": clean_reply or "Mình đã hoàn tất.",
                    "raw_reply": raw_reply,
                    "events": all_events,
                    "tool_logs": tool_logs,
                }

            msgs.append({"role": "assistant", "content": msg.get("content") or "", "tool_calls": tcalls})
            for tc in tcalls:
                fn_obj = tc.get("function") or {}
                tname = fn_obj.get("name") or ""
                try:
                    targs = json.loads(fn_obj.get("arguments") or "{}")
                except Exception:
                    targs = {}
                tres, evs = execute_mcp_tool(tname, targs, cfg)
                all_events.extend(evs)
                tool_logs.append({"tool": tname, "args": targs, "result": tres})
                msgs.append({
                    "role": "tool",
                    "tool_call_id": tc.get("id") or str(uuid.uuid4()),
                    "name": tname,
                    "content": json.dumps(tres, ensure_ascii=False),
                })

        if not failed_this_profile:
            return {"ok": True, "emotion": "suy_nghi", "reply": "Mình đã xử lý xong.", "events": all_events, "tool_logs": tool_logs}

    return {"ok": False, "error": last_err}


# ============================================================================
# Edge TTS + Google Translate TTS Synthesizer (No CORS, MP3 stream)
# ============================================================================
def synthesize_tts_mp3(text: str, voice: str, prefer_google: bool) -> Tuple[bytes, str]:
    clean = re.sub(r"[\*#`]+", " ", text).strip()
    if not clean:
        return b"", "empty"

    if not prefer_google and HAS_EDGE_TTS:
        try:
            async def _run() -> bytes:
                comm = edge_tts.Communicate(clean, voice=voice or "vi-VN-HoaiMyNeural")
                chunks = []
                async for msg in comm.stream():
                    if msg.get("type") == "audio":
                        chunks.append(msg["data"])
                return b"".join(chunks)

            loop = asyncio.new_event_loop()
            try:
                audio_bytes = loop.run_until_complete(_run())
            finally:
                loop.close()
            if audio_bytes:
                return audio_bytes, "edge-tts"
        except Exception:
            pass

    segments = [s.strip() for s in re.split(r"(?<=[.!?,\n])\s+", clean) if s.strip()]
    if not segments:
        segments = [clean[:180]]
    out = bytearray()
    for seg in segments:
        part = seg[:180]
        gurl = f"https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=vi&q={urllib.parse.quote(part)}"
        st, _, raw = http_fetch(gurl, headers={"Referer": "https://translate.google.com/"}, timeout=12)
        if st == 200 and raw:
            out.extend(raw)
    return bytes(out), "google-tts"


def inspect_firmware_status() -> Dict[str, Any]:
    bin_path = ROOT_DIR / "build" / "sw_nioh.bin"
    pio_bin = ROOT_DIR / ".pio" / "build" / "esp32s3_supermini" / "firmware.bin"
    if not bin_path.exists() and pio_bin.exists():
        bin_path = pio_bin
    elf_path = ROOT_DIR / "build" / "sw_nioh.elf"
    pio_elf = ROOT_DIR / ".pio" / "build" / "esp32s3_supermini" / "firmware.elf"
    if not elf_path.exists() and pio_elf.exists():
        elf_path = pio_elf
    part_path = ROOT_DIR / "partitions.csv"
    wokwi_path = ROOT_DIR / "wokwi.toml"
    diag_path = ROOT_DIR / "diagram.json"

    partitions: List[Dict[str, Any]] = []
    factory_limit = 3200 * 1024
    if part_path.exists():
        for line in part_path.read_text(encoding="utf-8", errors="ignore").splitlines():
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            cols = [c.strip() for c in s.split(",")]
            if len(cols) >= 5:
                partitions.append({"name": cols[0], "type": cols[1], "subtype": cols[2], "offset": cols[3], "size": cols[4]})
                if cols[0] == "factory":
                    sz = cols[4].upper()
                    if sz.startswith("0X"):
                        factory_limit = int(sz, 16)
                    elif sz.endswith("K"):
                        factory_limit = int(sz[:-1]) * 1024
                    elif sz.endswith("M"):
                        factory_limit = int(sz[:-1]) * 1024 * 1024
                    else:
                        factory_limit = int(sz)

    bin_info: Dict[str, Any] = {"exists": bin_path.exists(), "path": str(bin_path.relative_to(ROOT_DIR)).replace("\\", "/")}
    if bin_path.exists():
        data = bin_path.read_bytes()
        bin_info["size_bytes"] = len(data)
        bin_info["size_kb"] = round(len(data) / 1024.0, 2)
        bin_info["factory_limit_kb"] = factory_limit // 1024
        bin_info["usage_pct"] = round(len(data) * 100.0 / max(1, factory_limit), 1)
        bin_info["sha256_short"] = hashlib.sha256(data).hexdigest()[:16]
        if len(data) >= 24:
            magic, segs, _, _, entry = struct.unpack_from("<BBBBI", data, 0)
            bin_info["esp_magic"] = f"0x{magic:02X}"
            bin_info["entry_point"] = f"0x{entry:08X}"

    return {
        "project_name": "sw_nioh",
        "brand": "S.W.Ni-Oh",
        "bin": bin_info,
        "elf_exists": elf_path.exists(),
        "elf_size_kb": round(elf_path.stat().st_size / 1024.0, 2) if elf_path.exists() else 0,
        "wokwi_ready": wokwi_path.exists() and diag_path.exists(),
        "partitions": partitions,
        "components": sorted([p.name for p in (ROOT_DIR / "components").iterdir() if p.is_dir()]) if (ROOT_DIR / "components").exists() else [],
    }


def _crc8(data: bytes) -> int:
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else ((crc << 1) & 0xFF)
    return crc


class SimHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A003
        pass

    def _cors_headers(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Range")

    def do_OPTIONS(self) -> None:  # noqa: N802
        self.send_response(204)
        self._cors_headers()
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _send_bytes(self, code: int, content_type: str, data: bytes, extra_headers: Optional[Dict[str, str]] = None) -> None:
        self.send_response(code)
        self._cors_headers()
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _send_json(self, code: int, obj: Any) -> None:
        self._send_bytes(code, "application/json; charset=utf-8", json.dumps(obj, ensure_ascii=False).encode("utf-8"))

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length") or "0")
        if length <= 0:
            return b""
        return self.rfile.read(min(length, 16 * 1024 * 1024))

    def do_HEAD(self) -> None:  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        path = "/index.html" if parsed.path == "/" else parsed.path
        if path in ("/index.html", "/sim_engine.js", "/style.css"):
            fpath = SIM_DIR / path.lstrip("/")
            if not fpath.exists():
                self.send_response(404)
                self._cors_headers()
                self.end_headers()
                return
            ctype, _ = mimetypes.guess_type(str(fpath))
            self.send_response(200)
            self._cors_headers()
            self.send_header("Content-Type", f"{ctype or 'application/octet-stream'}; charset=utf-8")
            self.send_header("Content-Length", str(fpath.stat().st_size))
            self.end_headers()
            return
        if path == "/api/tts":
            qs = urllib.parse.parse_qs(parsed.query)
            text = (qs.get("text") or ["Xin chào"])[0]
            voice = (qs.get("voice") or [""])[0]
            cfg = load_config()
            mp3, prov = synthesize_tts_mp3(text, voice or cfg.get("tts_voice", "vi-VN-HoaiMyNeural"), bool(cfg.get("tts_prefer_google")))
            self.send_response(200 if mp3 else 502)
            self._cors_headers()
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("X-TTS-Provider", prov)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(len(mp3)))
            self.end_headers()
            return
        if path in ("/api/config", "/api/firmware_info", "/api/lunar_now", "/api/music", "/api/music_stream"):
            self.send_response(200)
            self._cors_headers()
            self.send_header("Content-Type", "audio/mpeg" if "music" in path else "application/json; charset=utf-8")
            self.end_headers()
            return
        self.send_response(404)
        self._cors_headers()
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        path = "/index.html" if parsed.path == "/" else parsed.path

        if path in ("/index.html", "/sim_engine.js", "/style.css"):
            fpath = SIM_DIR / path.lstrip("/")
            if not fpath.exists():
                self._send_bytes(404, "text/plain", b"Not found")
                return
            ctype, _ = mimetypes.guess_type(str(fpath))
            self._send_bytes(200, f"{ctype or 'application/octet-stream'}; charset=utf-8", fpath.read_bytes())
            return

        if path == "/api/chronos_avrcp":
            try:
                req = json.loads(self._read_body().decode("utf-8"))
                cmd = str(req.get("cmd") or "play_pause")
                code_map = {"play_pause": "0xE0 0x01", "next": "0xE0 0x02", "prev": "0xE0 0x03"}
                self._send_json(200, {"ok": True, "cmd": cmd, "ble_hex": code_map.get(cmd, "0xE0 0x00")})
            except Exception as e:
                self._send_json(400, {"ok": False, "error": str(e)})
            return

        if path == "/api/config":
            cfg = load_config()
            safe = json.loads(json.dumps(cfg))
            safe["has_stt_key"] = bool(cfg.get("stt_api_key"))
            safe["has_search_key"] = bool(cfg.get("search_api_key"))
            safe["stt_api_key"] = "__KEEP__" if cfg.get("stt_api_key") else ""
            safe["search_api_key"] = "__KEEP__" if cfg.get("search_api_key") else ""
            safe["wifi_pass"] = "__KEEP__" if cfg.get("wifi_pass") else ""
            for p in safe.get("llm_profiles", []):
                has_k = bool(p.get("api_key"))
                p["has_key"] = has_k
                p["api_key"] = "__KEEP__" if has_k else ""
            safe["edge_tts_installed"] = HAS_EDGE_TTS
            safe["pil_installed"] = HAS_PIL
            safe["yt_dlp_installed"] = bool(shutil.which("yt-dlp"))
            self._send_json(200, safe)
            return

        if path == "/api/tts":
            qs = urllib.parse.parse_qs(parsed.query)
            text = (qs.get("text") or [""])[0]
            voice = (qs.get("voice") or [""])[0]
            cfg = load_config()
            mp3, prov = synthesize_tts_mp3(text, voice or cfg.get("tts_voice", "vi-VN-HoaiMyNeural"), bool(cfg.get("tts_prefer_google")))
            if not mp3:
                self._send_json(502, {"ok": False, "error": "TTS synthesize rỗng"})
                return
            self._send_bytes(200, "audio/mpeg", mp3, {"X-TTS-Provider": prov, "Accept-Ranges": "bytes"})
            return

        if path in ("/api/music", "/api/music_stream"):
            qs = urllib.parse.parse_qs(parsed.query)
            q = (qs.get("q") or [""])[0]
            plat = (qs.get("platform") or ["youtube"])[0]
            cfg = load_config()
            res = _resolve_music_source(q, plat, cfg)
            if not res.get("ok") or not res.get("direct_url"):
                self._send_json(404, {"ok": False, "error": res.get("error") or "Không tìm thấy bài hát"})
                return
            self._proxy_stream(res["direct_url"], default_ctype="audio/mpeg")
            return

        if path in ("/api/media_proxy", "/api/mjpeg_proxy"):
            qs = urllib.parse.parse_qs(parsed.query)
            target = (qs.get("url") or [""])[0]
            if not target.startswith(("http://", "https://")):
                self._send_bytes(400, "text/plain", b"Invalid url")
                return
            self._proxy_stream(target)
            return

        if path == "/api/demo_mjpeg":
            qs = urllib.parse.parse_qs(parsed.query)
            title = (qs.get("q") or ["S.W.Ni-Oh Video"])[0]
            res = (qs.get("res") or ["240x240"])[0]
            fps = int((qs.get("fps") or ["12"])[0] or 12)
            self._serve_demo_mjpeg(title, res, fps)
            return

        if path == "/api/firmware_info":
            self._send_json(200, inspect_firmware_status())
            return

        if path == "/api/lunar_now":
            now = datetime.now(VN_TZ)
            self._send_json(200, {"now": now.isoformat(), "lunar": solar_to_lunar(now.day, now.month, now.year, 7)})
            return

        if path == "/api/chronos_demo_tick":
            qs = urllib.parse.parse_qs(parsed.query)
            step = int((qs.get("step") or ["0"])[0] or 0) % 4
            now = datetime.now(VN_TZ)
            if step == 0:
                payload = struct.pack(">HBBBBB", now.year, now.month, now.day, now.hour, now.minute, now.second)
                frame = struct.pack(">BBB", 0xAB, 0x00, len(payload) + 3) + bytes([0xFF, 0x93, 0x80]) + payload
                ev = {"kind": "time_battery", "battery": 88, "time": now.strftime("%H:%M:%S %d/%m/%Y"), "hex": frame.hex(" ").upper()}
            elif step == 1:
                w = bytes([0xFF, 0x88, 0x80, 0x01, 29, 72])
                frame = struct.pack(">BBB", 0xAB, 0x00, len(w)) + w
                ev = {"kind": "weather", "temp_c": 29, "humidity": 72, "condition": "Ít mây", "hex": frame.hex(" ").upper()}
            elif step == 2:
                txt = "Minh Anh\0Chiều nay 5h đi cà phê nhé!".encode("utf-8")
                body = bytes([0xFF, 0x72, 0x80, 0x07, 0x01]) + txt
                frame = struct.pack(">BBB", 0xAB, 0x00, len(body)) + body
                ev = {"kind": "notification", "app": "Zalo", "title": "Minh Anh", "body": "Chiều nay 5h đi cà phê nhé!", "hex": frame.hex(" ").upper()}
            else:
                nav_b = bytes([0xFE, 0xEF, 0x80, 0x01, 0x01]) + "Ngã Tư Sở\0\0250 m\0\0Rẽ phải vào Tây Sơn".encode("utf-8")
                frame = struct.pack(">BBB", 0xAB, 0x00, len(nav_b)) + nav_b
                ev = {"kind": "navigation", "icon": 3, "direction": "Rẽ phải", "distance": "250 m", "street": "Tây Sơn -> Ngã Tư Sở", "hex": frame.hex(" ").upper()}
            self._send_json(200, ev)
            return

        self._send_bytes(404, "text/plain", b"Not found")

    def do_POST(self) -> None:  # noqa: N802
        path = urllib.parse.urlparse(self.path).path

        if path == "/api/chronos_avrcp":
            try:
                req = json.loads(self._read_body().decode("utf-8"))
                cmd = str(req.get("cmd") or "play_pause")
                code_map = {"play_pause": "0xE0 0x01", "next": "0xE0 0x02", "prev": "0xE0 0x03"}
                self._send_json(200, {"ok": True, "cmd": cmd, "ble_hex": code_map.get(cmd, "0xE0 0x00")})
            except Exception as e:
                self._send_json(400, {"ok": False, "error": str(e)})
            return

        if path == "/api/config":
            try:
                patch = json.loads(self._read_body().decode("utf-8"))
                save_config(patch)
                self._send_json(200, {"ok": True})
            except Exception as e:
                self._send_json(400, {"ok": False, "error": str(e)})
            return

        if path == "/api/llm_autodetect":
            try:
                req = json.loads(self._read_body().decode("utf-8"))
                prof = merge_profile_patch(load_config(), req)
                inferred = infer_provider_from_key(str(prof.get("api_key") or ""))
                if inferred:
                    prof["provider"] = inferred["provider"]
                    prof["base_url"] = inferred["base_url"]
                    prof["auth_scheme"] = inferred["auth_scheme"]
                    prof["raw_endpoint"] = False
                mres = fetch_models_for_profile(prof)
                # Nếu key sk-... và endpoint hiện tại báo lỗi, thử dò DeepSeek / OpenAI / Moonshot
                if (not mres.get("ok")) and not inferred and str(prof.get("api_key") or "").startswith("sk-"):
                    for cand_p, cand_u in (("openai", "https://api.openai.com/v1"), ("deepseek", "https://api.deepseek.com"), ("kimi_global", "https://api.moonshot.ai/v1")):
                        trial = dict(prof, provider=cand_p, base_url=cand_u, auth_scheme="bearer", raw_endpoint=False)
                        r2 = fetch_models_for_profile(trial)
                        if r2.get("ok"):
                            prof = trial
                            mres = r2
                            break
                suggested = mres.get("suggested_model") or (inferred or {}).get("model") or prof.get("model") or ""
                self._send_json(200, {
                    "ok": bool(mres.get("ok") or inferred),
                    "provider": prof.get("provider") or "custom",
                    "base_url": prof.get("base_url") or "",
                    "auth_scheme": prof.get("auth_scheme") or "bearer",
                    "suggested_model": suggested,
                    "models": mres.get("models") or ([suggested] if suggested else []),
                    "error": "" if mres.get("ok") else (mres.get("error") or ""),
                })
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        if path == "/api/llm_models":
            try:
                req = json.loads(self._read_body().decode("utf-8"))
                prof = merge_profile_patch(load_config(), req)
                self._send_json(200, fetch_models_for_profile(prof))
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        if path == "/api/llm_test":
            try:
                req = json.loads(self._read_body().decode("utf-8"))
                prof = merge_profile_patch(load_config(), req)
                t0 = time.perf_counter()
                st, res = _call_profile_once(
                    prof,
                    [{"role": "user", "content": "Trả lời đúng 1 câu ngắn tiếng Việt xác nhận S.W.Ni-Oh kết nối thành công."}],
                    None,
                    120,
                    0.3,
                )
                lat = int((time.perf_counter() - t0) * 1000)
                if 200 <= st < 300:
                    self._send_json(200, {"ok": True, "latency_ms": lat, "reply": (res["message"].get("content") or "").strip()})
                else:
                    self._send_json(200, {"ok": False, "latency_ms": lat, "error": f"HTTP {st}: {res.get('error', '')}"})
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        if path == "/api/stt":
            cfg = load_config()
            stt_key = (cfg.get("stt_api_key") or "").strip()
            if not stt_key:
                # Tự tìm Groq/OpenAI key từ các LLM profile đã bật
                for p in cfg.get("llm_profiles", []):
                    if (p.get("provider") in ("groq", "custom")) and p.get("api_key"):
                        stt_key = p["api_key"]
                        break
                if not stt_key:
                    stt_key = (cfg.get("llm_api_key") or "").strip()
            if not stt_key:
                self._send_json(400, {"ok": False, "error": "Chưa có Groq STT API Key hoặc LLM API Key."})
                return
            wav_bytes = self._read_body()
            if len(wav_bytes) < 64:
                self._send_json(400, {"ok": False, "error": "Dữ liệu âm thanh quá ngắn."})
                return

            stt_url = "https://api.openai.com/v1/audio/transcriptions" if "api.openai.com" in (cfg.get("llm_base_url") or "") and not cfg.get("stt_api_key") else "https://api.groq.com/openai/v1/audio/transcriptions"
            stt_model = "whisper-1" if "api.openai.com" in stt_url else "whisper-large-v3-turbo"
            boundary = "----SWNiOhWavBoundary987654"
            body = (
                f"--{boundary}\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n{stt_model}\r\n"
                f"--{boundary}\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nvi\r\n"
                f"--{boundary}\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
                f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
                f"Content-Type: audio/wav\r\n\r\n"
            ).encode("utf-8") + wav_bytes + f"\r\n--{boundary}--\r\n".encode("utf-8")

            st, _, raw = http_fetch(stt_url, method="POST", headers={"Authorization": f"Bearer {stt_key}", "Content-Type": f"multipart/form-data; boundary={boundary}"}, body=body, timeout=25)
            if st != 200:
                self._send_json(st or 502, {"ok": False, "error": raw.decode("utf-8", "ignore")[:240]})
                return
            txt = (json.loads(raw.decode("utf-8", "ignore")).get("text") or "").strip()
            self._send_json(200, {"ok": True, "text": txt})
            return

        if path == "/api/chat":
            try:
                req = json.loads(self._read_body().decode("utf-8"))
                res = run_chat_with_tools(load_config(), str(req.get("message") or ""), req.get("history") or [])
                self._send_json(200, res)
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        if path == "/api/mcp":
            try:
                req = json.loads(self._read_body().decode("utf-8"))
                out, evs = execute_mcp_tool(str(req.get("tool") or ""), req.get("args") or {}, load_config())
                self._send_json(200, {"ok": True, "result": out, "events": evs})
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        if path == "/api/tts":
            try:
                req = json.loads(self._read_body().decode("utf-8"))
                cfg = load_config()
                mp3, prov = synthesize_tts_mp3(str(req.get("text") or ""), str(req.get("voice") or cfg.get("tts_voice") or "vi-VN-HoaiMyNeural"), bool(cfg.get("tts_prefer_google")))
                if not mp3:
                    self._send_json(502, {"ok": False, "error": "Không tổng hợp được TTS"})
                    return
                self._send_bytes(200, "audio/mpeg", mp3, {"X-TTS-Provider": prov, "Accept-Ranges": "bytes"})
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        if path == "/api/espnow_packet":
            req = json.loads(self._read_body().decode("utf-8"))
            cmd_map = {"PTT_AUDIO": 1, "CONTROL": 2, "ALERT": 3, "SYNC_CLOCK": 4, "ACK": 5}
            cmd_name = str(req.get("cmd") or "ALERT").upper()
            seq = int(req.get("seq") or 1) & 0xFF
            payload_b = str(req.get("payload") or "").encode("utf-8")[:200]
            header = struct.pack("<HBBB", 0x5A43, cmd_map.get(cmd_name, 3), seq, len(payload_b))
            crc = _crc8(header + payload_b)
            pkt = header + payload_b + bytes([crc])
            self._send_json(200, {"ok": True, "magic": "0x5A43", "cmd": cmd_name, "seq": seq, "crc8": f"0x{crc:02X}", "hex": pkt.hex(" ").upper(), "size": len(pkt)})
            return

        self._send_bytes(404, "text/plain", b"Not found")

    def _proxy_stream(self, target_url: str, default_ctype: str = "application/octet-stream") -> None:
        hdrs = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) SW-NiOh-Sim/1.0"}
        if self.headers.get("Range"):
            hdrs["Range"] = self.headers["Range"]
        req = urllib.request.Request(target_url, headers=hdrs)
        ctx = ssl.create_default_context()
        try:
            with urllib.request.urlopen(req, timeout=15, context=ctx) as resp:
                ctype = resp.headers.get("Content-Type") or default_ctype
                self.send_response(resp.status or 200)
                self._cors_headers()
                self.send_header("Content-Type", ctype)
                if resp.headers.get("Content-Length"):
                    self.send_header("Content-Length", resp.headers["Content-Length"])
                if resp.headers.get("Content-Range"):
                    self.send_header("Content-Range", resp.headers["Content-Range"])
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                while True:
                    chunk = resp.read(16384)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            try:
                self._send_bytes(502, "text/plain", str(e).encode("utf-8"))
            except Exception:
                pass

    def _serve_demo_mjpeg(self, title: str, res: str, fps: int = 12) -> None:
        if not HAS_PIL:
            self._send_bytes(503, "text/plain", b"Pillow chua cai dat, hay cau hinh Media Proxy URL")
            return
        w, h = (240, 180) if res == "240x180" else (240, 240)
        fps = max(5, min(25, fps))
        boundary = "swniohmjpeg"
        self.send_response(200)
        self._cors_headers()
        self.send_header("Content-Type", f"multipart/x-mixed-replace; boundary={boundary}")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

        import io
        frame_idx = 0
        try:
            while True:
                img = Image.new("RGB", (w, h), (8, 16, 28))
                draw = ImageDraw.Draw(img)
                t = frame_idx * 0.14
                for i in range(18):
                    y_line = int((h * 0.5) + math.sin(t + i * 0.35) * (h * 0.25))
                    color = (0, min(255, 130 + i * 6), 255)
                    draw.ellipse((14 + i * 11, y_line - 6, 26 + i * 11, y_line + 6), outline=color, width=2)
                cx, cy = w // 2, h // 2
                r = int(28 + 16 * abs(math.sin(t * 0.8)))
                draw.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(0, 229, 255), width=2)
                draw.rectangle((6, 6, w - 7, h - 7), outline=(0, 120, 200), width=1)
                draw.text((12, 12), f"S.W.Ni-Oh MJPEG {w}x{h} @{fps}FPS", fill=(0, 229, 255))
                draw.text((12, 30), f"Stream: {title[:26]}", fill=(210, 238, 255))
                draw.text((12, h - 22), f"Frame #{frame_idx:04d} | BTN_MAIN: Thoat", fill=(140, 200, 235))

                buf = io.BytesIO()
                img.save(buf, format="JPEG", quality=78)
                jpg = buf.getvalue()
                part = (f"--{boundary}\r\nContent-Type: image/jpeg\r\nContent-Length: {len(jpg)}\r\n\r\n").encode("ascii") + jpg + b"\r\n"
                self.wfile.write(part)
                self.wfile.flush()
                frame_idx += 1
                time.sleep(1.0 / fps)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main() -> None:
    parser = argparse.ArgumentParser(description="S.W.Ni-Oh Simulator Server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    srv = ThreadingHTTPServer((args.host, args.port), SimHandler)
    print(f"=== S.W.Ni-Oh Simulator đang chạy tại http://{args.host}:{args.port} ===")
    print(f"    edge-tts: {'OK' if HAS_EDGE_TTS else 'Fallback Google TTS'} | Pillow MJPEG: {'OK' if HAS_PIL else 'Proxy Only'} | yt-dlp: {'OK' if shutil.which('yt-dlp') else 'Piped/Invidious/SC'}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nĐã dừng S.W.Ni-Oh Simulator.")


if __name__ == "__main__":
    main()
