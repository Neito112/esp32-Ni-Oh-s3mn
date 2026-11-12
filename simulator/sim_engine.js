// S.W.Ni-Oh Simulator Engine (ESP-Claw 1:1 Multi-Provider + Pure Geometric Squircle Eyes + Direct S3Mini I2S1/ST7789)

(() => {
  "use strict";

  const byId = (id) => document.getElementById(id);

  const LCD_W = 240;
  const LCD_H = 240;
  const EYE_W = 56;
  const EYE_H = 56;
  const EYE_RAD = 15;
  const EYE_LEFT_X = 48;
  const EYE_RIGHT_X = 136;
  const EYE_Y = 64;

  const WAVE_N = 11;
  const WAVE_BAR_W = 10;
  const WAVE_BAR_GAP = 6;
  const WAVE_X_START = 35;
  const WAVE_Y = 175;

  const BTN_DEBOUNCE_MS = 30;
  const BTN_LONG_RESET_MS = 1600;
  const BTN_LONG_PORTAL_MS = 5000;
  const IDLE_CLOCK_MS = 5000;

  // ESP-Claw LlmPage Presets 1:1 (+ Groq, OpenRouter, Gemini, Ollama)
  const PRESET_BUTTONS = [
    { id: "openai",               label: "OpenAI",               base_url: "https://api.openai.com/v1",                             model: "gpt-4o-mini",                             auth_scheme: "bearer",         advanced: false },
    { id: "bailian",              label: "Alibaba Bailian",      base_url: "https://dashscope.aliyuncs.com/compatible-mode/v1",       model: "qwen-plus",                               auth_scheme: "bearer",         advanced: false },
    { id: "deepseek",             label: "DeepSeek",             base_url: "https://api.deepseek.com",                              model: "deepseek-chat",                           auth_scheme: "bearer",         advanced: false },
    { id: "anthropic",            label: "Anthropic",            base_url: "https://api.anthropic.com/v1",                          model: "claude-sonnet-4-6",                       auth_scheme: "x-api-key",      advanced: false },
    { id: "kimi_global",          label: "Kimi (Global)",        base_url: "https://api.moonshot.ai/v1",                            model: "kimi-k2.5",                               auth_scheme: "bearer",         advanced: false },
    { id: "kimi_cn",              label: "Kimi (CN)",            base_url: "https://api.moonshot.cn/v1",                            model: "kimi-k2.5",                               auth_scheme: "bearer",         advanced: false },
    { id: "minimax_global",       label: "MiniMax (Global)",     base_url: "https://api.minimax.io/anthropic/v1",                   model: "MiniMax-M2.7",                            auth_scheme: "x-api-key",      advanced: false },
    { id: "minimax_cn",           label: "MiniMax (CN)",         base_url: "https://api.minimaxi.com/anthropic/v1",                 model: "MiniMax-M2.7",                            auth_scheme: "x-api-key",      advanced: false },
    { id: "groq",                 label: "Groq",                 base_url: "https://api.groq.com/openai/v1",                        model: "llama-3.3-70b-versatile",                 auth_scheme: "bearer",         advanced: false },
    { id: "openrouter",           label: "OpenRouter",           base_url: "https://openrouter.ai/api/v1",                          model: "openai/gpt-4o-mini",                      auth_scheme: "bearer",         advanced: false },
    { id: "gemini",               label: "Google Gemini",        base_url: "https://generativelanguage.googleapis.com/v1beta/openai", model: "gemini-2.5-flash",                      auth_scheme: "bearer",         advanced: false },
    { id: "ollama",               label: "Ollama / vLLM",        base_url: "http://127.0.0.1:11434/v1",                             model: "qwen2.5:7b",                              auth_scheme: "bearer",         advanced: false },
    { id: "openai_compatible",    label: "OpenAI Compatible",    base_url: "https://api.openai.com/v1",                             model: "gpt-4o-mini",                             auth_scheme: "bearer",         advanced: true },
    { id: "anthropic_compatible", label: "Anthropic Compatible", base_url: "https://api.anthropic.com/v1",                          model: "claude-sonnet-4-6",                       auth_scheme: "x-api-key",      advanced: true },
  ];

  const PROVIDER_PRESETS = Object.fromEntries(PRESET_BUTTONS.map((p) => [p.id, p]));
  Object.assign(PROVIDER_PRESETS, {
    gemini_native: { label: "Gemini Native", base_url: "https://generativelanguage.googleapis.com/v1beta", model: "gemini-2.5-flash", auth_scheme: "x-goog-api-key", advanced: true },
    custom:        { label: "Custom",        base_url: "", model: "", auth_scheme: "bearer", advanced: true },
  });

  const canvas = byId("st7789Canvas");
  const ctx = canvas ? canvas.getContext("2d") : null;
  const lcdViewport = byId("lcdViewport");
  const mjpegOverlay = byId("mjpegOverlay");
  const mjpegImg = byId("mjpegImg");
  const mjpegTitleBar = byId("mjpegTitleBar");
  const simAudio = byId("simAudio");
  const serialConsole = byId("serialConsole");
  const chatBox = byId("chatBox");
  const chatInput = byId("chatInput");
  const btnHoldFill = byId("btnHoldFill");

  const state = {
    fsm: "IDLE",
    emotion: "binh_thuong",
    screen: "eyes",
    lastActivityAt: performance.now(),
    volume: 85,
    micGain: 6,
    backlight: 85,
    rotation: 0,
    ttsVoice: "vi-VN-HoaiMyNeural",
    osdTimer: 0,
    notif: null,
    musicBanner: null,
    nav: null,
    weather: { city: "Hà Nội", temp_c: "--", humidity: "--", condition: "Chờ dữ liệu" },
    lunar: { text: "Âm lịch: --/--" },
    chronos: {
      connected: false,
      modeText: 'Advertising "S.W.Ni-Oh"',
      phoneBattery: 100,
      lastTimeSync: "Chưa nhận (chờ 0x93)",
      lastNotif: "Chưa có (chờ 0x72)",
      rxPackets: 0,
      lastHex: "",
      autoDemoTimer: null,
      autoDemoStep: 0,
    },
    cfg: {
      llm_profiles: [{ name: "Groq Primary", provider: "groq", base_url: "https://api.groq.com/openai/v1", model: "llama-3.3-70b-versatile", auth_scheme: "bearer", enabled: true }],
      active_profile_idx: 0,
      auto_fallback: true,
    },
    curProfIdx: 0,
    chatHistory: [],
    waveBins: new Float32Array(WAVE_N),
    syntheticSpeechLevel: 0,
    emoAnim: { t0: performance.now(), blinkUntil: 0, nextBlink: performance.now() + 2600 },
    audioCtx: null,
    ttsAudioElem: null,
    micActive: false,
    micStream: null,
    micProc: null,
    micAnalyser: null,
    micSampleRate: 16000,
    vad: { recording: false, chunks: [], speechFrames: 0, silenceFrames: 0, maxRecordMs: 12000, startedAt: 0 },
    espnowSeq: 1,
  };
  window.simState = state;

  // =========================================================================
  // 1. PURE GEOMETRIC SQUIRCLE EYES + 60FPS ST7789 RENDERER (0% BLACK MASKING!)
  // =========================================================================
  function roundRect(x, y, w, h, r) {
    ctx.beginPath();
    const rr = Math.max(0, Math.min(r, w / 2, h / 2));
    if (typeof ctx.roundRect === "function") {
      ctx.roundRect(x, y, w, h, rr);
    } else {
      ctx.moveTo(x + rr, y);
      ctx.arcTo(x + w, y, x + w, y + h, rr);
      ctx.arcTo(x + w, y + h, x, y + h, rr);
      ctx.arcTo(x, y + h, x, y, rr);
      ctx.arcTo(x, y, x + w, y, rr);
    }
    ctx.closePath();
  }

  function updateWaveBins(now) {
    if (state.fsm === "LISTENING" && state.micAnalyser) {
      const freq = new Uint8Array(state.micAnalyser.frequencyBinCount);
      state.micAnalyser.getByteFrequencyData(freq);
      for (let i = 0; i < WAVE_N; i++) {
        const idx = Math.min(freq.length - 1, Math.floor((i * freq.length) / (WAVE_N + 2)) + 1);
        state.waveBins[i] = state.waveBins[i] * 0.45 + (freq[idx] / 255.0) * 0.55;
      }
      return;
    }
    const speakingOrPlaying =
      state.syntheticSpeechLevel > 0.05 ||
      (state.ttsAudioElem && !state.ttsAudioElem.paused) ||
      (simAudio && !simAudio.paused);
    if (speakingOrPlaying) {
      const amp = state.syntheticSpeechLevel > 0.05 ? state.syntheticSpeechLevel : 0.78;
      for (let i = 0; i < WAVE_N; i++) {
        const w = 0.22 + 0.62 * Math.abs(Math.sin(now * 0.015 + i * 0.85)) * amp;
        state.waveBins[i] = state.waveBins[i] * 0.4 + w * 0.6;
      }
      return;
    }
    for (let i = 0; i < WAVE_N; i++) state.waveBins[i] *= 0.82;
  }

  function drawSquircleEye(cx, cy, w, h, rad, angleDeg, opa, blur) {
    ctx.save();
    ctx.globalAlpha = opa;
    ctx.translate(cx, cy);
    if (angleDeg) ctx.rotate((angleDeg * Math.PI) / 180);
    ctx.shadowColor = "#0077FF";
    ctx.shadowBlur = blur;
    const g = ctx.createLinearGradient(0, -h / 2, 0, h / 2);
    g.addColorStop(0, "#00B4FF");
    g.addColorStop(1, "#00E5FF");
    ctx.fillStyle = g;
    roundRect(-w / 2, -h / 2, w, h, Math.min(rad, h / 2, w / 2));
    ctx.fill();
    ctx.restore();
  }

  function drawHappyArchEye(cx, cy, w, h, bounceY, blur) {
    ctx.save();
    ctx.translate(cx, cy + bounceY);
    ctx.shadowColor = "#0077FF";
    ctx.shadowBlur = blur;
    const g = ctx.createLinearGradient(0, -h * 0.35, 0, h * 0.2);
    g.addColorStop(0, "#00E5FF");
    g.addColorStop(1, "#00B4FF");
    ctx.strokeStyle = g;
    ctx.lineWidth = 10;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    ctx.beginPath();
    ctx.moveTo(-w * 0.44, h * 0.14);
    ctx.quadraticCurveTo(0, -h * 0.42, w * 0.44, h * 0.14);
    ctx.stroke();
    ctx.restore();
  }

  function drawEyesScreen(now) {
    ctx.fillStyle = "#000000";
    ctx.fillRect(0, 0, LCD_W, LCD_H);
    ctx.fillStyle = "#2F7CA8";
    ctx.font = "600 11px Segoe UI, sans-serif";
    ctx.textAlign = "center";
    ctx.fillText("S.W.Ni-Oh", LCD_W / 2, 18);

    const dt = now - state.emoAnim.t0;
    const emo = state.emotion;

    let cxl = EYE_LEFT_X + EYE_W / 2;
    let cxr = EYE_RIGHT_X + EYE_W / 2;
    let cyl = EYE_Y + EYE_H / 2;
    let cyr = EYE_Y + EYE_H / 2;
    let lw = EYE_W, rw = EYE_W, lh = EYE_H, rh = EYE_H;
    let radL = EYE_RAD, radR = EYE_RAD;
    let angL = 0, angR = 0;
    let opa = 1.0, shadowBlur = 18;

    if (emo === "binh_thuong") {
      // Chớp mắt trực tiếp bằng thu nhỏ height: 56px -> 4px -> 56px (KHÔNG dùng mí đen che đè!)
      if (now >= state.emoAnim.nextBlink) {
        state.emoAnim.blinkUntil = now + 150;
        state.emoAnim.nextBlink = now + 2600 + Math.random() * 2200;
      }
      if (now < state.emoAnim.blinkUntil) {
        const phase = Math.abs((state.emoAnim.blinkUntil - now) / 75 - 1); // 1 -> 0 -> 1
        lh = rh = Math.max(4, Math.round(4 + (EYE_H - 4) * phase));
        radL = radR = Math.min(EYE_RAD, lh / 2);
      }
      drawSquircleEye(cxl, cyl, lw, lh, radL, 0, opa, shadowBlur);
      drawSquircleEye(cxr, cyr, rw, rh, radR, 0, opa, shadowBlur);
    } else if (emo === "vui_ve") {
      // Vẽ trực tiếp vòm cung cười ^ ^ thuần vector (0% vệt đen!)
      const b = -3 + 3 * Math.sin(now * 0.009);
      drawHappyArchEye(cxl, cyl, EYE_W, EYE_H, b, 20);
      drawHappyArchEye(cxr, cyr, EYE_W, EYE_H, b, 20);
    } else if (emo === "dang_nghe") {
      const k = Math.min(1, dt / 180);
      lh = rh = EYE_H + 6 * k;
      radL = radR = 16;
      shadowBlur = 24;
      opa = 0.84 + 0.16 * Math.sin(now * 0.006);
      drawSquircleEye(cxl, cyl - 3 * k, lw, lh, radL, 0, opa, shadowBlur);
      drawSquircleEye(cxr, cyr - 3 * k, rw, rh, radR, 0, opa, shadowBlur);
    } else if (emo === "suy_nghi") {
      const k = Math.min(1, dt / 240);
      lh = EYE_H - (EYE_H - 24) * k;
      radL = 12;
      cxr = EYE_RIGHT_X + EYE_W / 2 + 8 * k;
      cyr = EYE_Y + EYE_H / 2 - 6 * k;
      rh = EYE_H - 4 * k;
      drawSquircleEye(cxl, cyl + 2 * k, lw, lh, radL, 0, opa, shadowBlur);
      drawSquircleEye(cxr, cyr, rw, rh, radR, 0, opa, shadowBlur);
    } else if (emo === "ngac_nhien") {
      const dw = Math.round(10 * Math.sin(Math.min(1, dt / 350) * Math.PI));
      lw = rw = EYE_W + dw;
      lh = rh = EYE_H + dw;
      radL = radR = 18;
      shadowBlur = 26;
      drawSquircleEye(cxl, cyl, lw, lh, radL, 0, opa, shadowBlur);
      drawSquircleEye(cxr, cyr, rw, rh, radR, 0, opa, shadowBlur);
    } else if (emo === "buon") {
      // Thu dẹt & xoay nghiêng chính khối squircle (-14° / +14°), không che mảng đen
      const k = Math.min(1, dt / 320);
      lh = rh = EYE_H - (EYE_H - 32) * k;
      angL = -14 * k;
      angR = 14 * k;
      radL = radR = 13;
      opa = 1 - 0.35 * k;
      drawSquircleEye(cxl, cyl + 4 * k, lw, lh, radL, angL, opa, shadowBlur);
      drawSquircleEye(cxr, cyr + 4 * k, rw, rh, radR, angR, opa, shadowBlur);
    } else if (emo === "ngu_gat") {
      const k = Math.min(1, dt / 1600);
      lh = rh = Math.max(4, Math.round(EYE_H - (EYE_H - 4) * k));
      radL = radR = Math.min(EYE_RAD, lh / 2);
      opa = 1 - 0.55 * k;
      drawSquircleEye(cxl, cyl, lw, lh, radL, 0, opa, shadowBlur);
      drawSquircleEye(cxr, cyr, rw, rh, radR, 0, opa, shadowBlur);
    } else {
      drawSquircleEye(cxl, cyl, lw, lh, radL, 0, opa, shadowBlur);
      drawSquircleEye(cxr, cyr, rw, rh, radR, 0, opa, shadowBlur);
    }

    if (state.musicBanner) {
      ctx.save();
      ctx.fillStyle = "rgba(13, 30, 46, 0.92)";
      ctx.strokeStyle = "#00B4FF";
      ctx.lineWidth = 1;
      roundRect(8, 136, 224, 26, 8);
      ctx.fill();
      ctx.stroke();
      ctx.beginPath();
      ctx.rect(14, 137, 212, 24);
      ctx.clip();
      const txt = `${state.musicBanner.sourceTag} ${state.musicBanner.title}`;
      ctx.fillStyle = "#00E5FF";
      ctx.font = "600 12px Segoe UI, sans-serif";
      ctx.textAlign = "left";
      const tw = ctx.measureText(txt).width;
      let sx = 18;
      if (tw > 204) {
        state.musicBanner.scrollOffset = (state.musicBanner.scrollOffset + 0.7) % (tw + 40);
        sx = 18 - state.musicBanner.scrollOffset;
      }
      ctx.fillText(txt, sx, 153);
      if (tw > 204) ctx.fillText(txt, sx + tw + 40, 153);
      ctx.restore();
    }

    updateWaveBins(now);
    for (let i = 0; i < WAVE_N; i++) {
      const maxH = 44 - Math.abs(i - 5) * 4;
      const barH = Math.max(4, Math.round(4 + state.waveBins[i] * (maxH - 4)));
      const x = WAVE_X_START + i * (WAVE_BAR_W + WAVE_BAR_GAP);
      const y = WAVE_Y + Math.round((44 - barH) / 2);
      const g = ctx.createLinearGradient(0, y, 0, y + barH);
      g.addColorStop(0, "#00B4FF");
      g.addColorStop(1, "#00E5FF");
      ctx.fillStyle = g;
      roundRect(x, y, WAVE_BAR_W, barH, 3);
      ctx.fill();
    }
  }

  function drawClockScreen() {
    ctx.fillStyle = "#000000";
    ctx.fillRect(0, 0, LCD_W, LCD_H);
    ctx.fillStyle = "#2F7CA8";
    ctx.font = "600 11px Segoe UI, sans-serif";
    ctx.textAlign = "center";
    ctx.fillText("S.W.Ni-Oh", LCD_W / 2, 15);

    const d = new Date();
    const thu = ["Chủ Nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy"][d.getDay()];
    ctx.fillStyle = "#FFFFFF";
    ctx.font = "700 44px Segoe UI, sans-serif";
    ctx.fillText(`${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`, 102, 62);
    ctx.fillStyle = "#00E5FF";
    ctx.font = "700 22px Segoe UI, sans-serif";
    ctx.textAlign = "left";
    ctx.fillText(`:${String(d.getSeconds()).padStart(2, "0")}`, 164, 62);
    ctx.fillStyle = "#DDDDDD";
    ctx.font = "500 13px Segoe UI, sans-serif";
    ctx.textAlign = "center";
    ctx.fillText(`${thu}, ${String(d.getDate()).padStart(2, "0")}/${String(d.getMonth() + 1).padStart(2, "0")}/${d.getFullYear()}`, LCD_W / 2, 86);
    ctx.fillStyle = "#FFD700";
    ctx.font = "600 13px Segoe UI, sans-serif";
    ctx.fillText(state.lunar.text, LCD_W / 2, 108);

    ctx.fillStyle = "#101C28";
    ctx.strokeStyle = "#1E3A52";
    ctx.lineWidth = 1;
    roundRect(10, 122, 220, 40, 10);
    ctx.fill();
    ctx.stroke();

    ctx.fillStyle = String(state.weather.condition).includes("Mưa") ? "#00B4FF" : "#FFD700";
    ctx.beginPath();
    ctx.arc(24, 142, 7, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = "#87CEEB";
    ctx.font = "700 15px Segoe UI, sans-serif";
    ctx.textAlign = "left";
    ctx.fillText(`${state.weather.temp_c}°C`, 38, 147);
    ctx.fillStyle = "#A0C8E0";
    ctx.font = "500 13px Segoe UI, sans-serif";
    ctx.fillText(`${state.weather.humidity}%`, 92, 147);
    ctx.fillStyle = "#FFFFFF";
    ctx.font = "500 12px Segoe UI, sans-serif";
    ctx.textAlign = "right";
    ctx.fillText(String(state.weather.condition).slice(0, 14), 220, 146);

    if (state.nav) {
      ctx.fillStyle = "#133352";
      ctx.strokeStyle = "#00B4FF";
      roundRect(10, 170, 220, 56, 10);
      ctx.fill();
      ctx.stroke();
      const arrows = { 1: "↑ Đi thẳng", 2: "← Rẽ trái", 3: "→ Rẽ phải", 4: "↺ Quay đầu", 5: "◎ Vòng xuyến" };
      ctx.fillStyle = "#FFFFFF";
      ctx.font = "700 13px Segoe UI, sans-serif";
      ctx.textAlign = "left";
      ctx.fillText(arrows[state.nav.icon] || `↑ ${state.nav.direction}`, 18, 190);
      ctx.fillStyle = "#00E5FF";
      ctx.textAlign = "right";
      ctx.fillText(state.nav.distance, 222, 190);
      ctx.fillStyle = "#CCCCCC";
      ctx.font = "500 12px Segoe UI, sans-serif";
      ctx.textAlign = "left";
      ctx.fillText(String(state.nav.street || "").slice(0, 30), 18, 212);
    }
  }

  function drawOverlays(now) {
    if (now < state.osdTimer) {
      ctx.save();
      ctx.fillStyle = "rgba(22, 34, 46, 0.94)";
      ctx.strokeStyle = "#00B4FF";
      roundRect(32, 97, 176, 46, 12);
      ctx.fill();
      ctx.stroke();
      ctx.fillStyle = "#FFFFFF";
      ctx.font = "600 13px Segoe UI, sans-serif";
      ctx.textAlign = "center";
      ctx.fillText(`Âm lượng: ${state.volume}%`, LCD_W / 2, 117);
      ctx.fillStyle = "#334455";
      roundRect(50, 127, 140, 7, 4);
      ctx.fill();
      ctx.fillStyle = "#00E5FF";
      roundRect(50, 127, Math.max(4, (140 * state.volume) / 100), 7, 4);
      ctx.fill();
      ctx.restore();
    }
    if (state.notif && now < state.notif.expiresAt) {
      ctx.save();
      ctx.fillStyle = "rgba(24, 35, 46, 0.94)";
      ctx.strokeStyle = "#00B4FF";
      roundRect(8, 174, 224, 58, 10);
      ctx.fill();
      ctx.stroke();
      ctx.fillStyle = "#00E5FF";
      ctx.font = "700 11px Segoe UI, sans-serif";
      ctx.textAlign = "left";
      ctx.fillText(String(state.notif.app || "S.W.Ni-Oh"), 16, 190);
      ctx.fillStyle = "#FFFFFF";
      ctx.font = "600 12px Segoe UI, sans-serif";
      ctx.fillText(String(state.notif.title || "").slice(0, 28), 16, 206);
      ctx.fillStyle = "#CCCCCC";
      ctx.font = "500 11px Segoe UI, sans-serif";
      ctx.fillText(String(state.notif.body || "").slice(0, 32), 16, 223);
      ctx.restore();
    }
  }

  function renderLcdLoop(now) {
    try {
      const cdEl = byId("lblIdleCountdown");
      if (state.fsm === "IDLE" && state.screen === "eyes") {
        const rem = Math.max(0, (IDLE_CLOCK_MS - (now - state.lastActivityAt)) / 1000);
        if (cdEl) cdEl.textContent = rem > 0 ? `Dashboard sau: ${rem.toFixed(1)}s` : "Đang bật Dashboard";
        if (rem <= 0) switchScreen("clock");
      } else if (state.screen === "clock") {
        if (cdEl) cdEl.textContent = "Đang ở Clock Dashboard";
      } else if (cdEl) {
        cdEl.textContent = `Trạng thái: ${state.fsm}`;
      }
      if (ctx) {
        if (state.screen === "clock") drawClockScreen();
        else drawEyesScreen(now);
        drawOverlays(now);
      }
    } catch (err) {
      console.error("LCD render error:", err);
    }
    requestAnimationFrame(renderLcdLoop);
  }
  requestAnimationFrame(renderLcdLoop);

  // =========================================================================
  // 2. Logging, State & LCD Helpers
  // =========================================================================
  function logSerial(level, tag, msg) {
    if (!serialConsole) return;
    const t = Math.floor(performance.now());
    const div = document.createElement("div");
    div.className = `log-${level}`;
    const prefix = level === "BLE" || level === "NOW" ? "I" : level;
    div.textContent = `${prefix} (${t}) ${tag}: ${msg}`;
    serialConsole.appendChild(div);
    while (serialConsole.children.length > 350) serialConsole.removeChild(serialConsole.firstChild);
    serialConsole.scrollTop = serialConsole.scrollHeight;
  }

  function appendChat(role, text) {
    if (!chatBox) return;
    const d = document.createElement("div");
    d.className = `msg ${role}`;
    d.textContent = text;
    chatBox.appendChild(d);
    chatBox.scrollTop = chatBox.scrollHeight;
  }

  function touchActivity() {
    state.lastActivityAt = performance.now();
    if (state.screen === "clock" && state.fsm !== "VIDEO") switchScreen("eyes");
  }

  function switchScreen(mode) {
    state.screen = mode;
    const pill = byId("pillScreen");
    if (pill) {
      pill.textContent = mode === "eyes" ? "Màn hình: Mắt & Sóng âm" : mode === "clock" ? "Màn hình: Đồng hồ & Âm lịch" : "Màn hình: Video MJPEG";
    }
  }

  function setFsm(fsm) {
    state.fsm = fsm;
    const p = byId("pillFsm");
    if (p) p.textContent = `FSM: ${fsm}`;
  }

  function setEmotion(emo) {
    state.emotion = emo;
    state.emoAnim.t0 = performance.now();
    document.querySelectorAll(".emo-btn").forEach((b) => b.classList.toggle("active", b.dataset.emo === emo));
  }

  function showToast(app, title, body, ms = 4200) {
    state.notif = { app, title, body, expiresAt: performance.now() + ms };
  }

  function showVolOsd() {
    state.osdTimer = performance.now() + 1800;
  }

  function applyLcdHardware() {
    if (lcdViewport) {
      const pct = state.emotion === "ngu_gat" ? Math.min(state.backlight, 25) : state.backlight;
      lcdViewport.style.filter = `brightness(${Math.max(0.18, pct / 100).toFixed(2)})`;
      lcdViewport.style.transform = `rotate(${state.rotation}deg)`;
    }
    if (simAudio) simAudio.volume = Math.max(0, Math.min(1, state.volume / 100));
    if (state.ttsAudioElem) state.ttsAudioElem.volume = Math.max(0, Math.min(1, state.volume / 100));
  }

  // =========================================================================
  // 3. Fail-Safe Audio Engine (Chime + /api/tts MP3 + Web SpeechSynthesis vi-VN)
  // =========================================================================
  function ensureAudioContext() {
    if (!state.audioCtx) {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (AC) state.audioCtx = new AC();
    }
    if (state.audioCtx && state.audioCtx.state === "suspended") {
      state.audioCtx.resume().catch(() => {});
    }
    return state.audioCtx;
  }
  ["pointerdown", "keydown", "click"].forEach((evt) => {
    window.addEventListener(evt, ensureAudioContext, { passive: true });
  });

  function playChime(type) {
    try {
      const ac = ensureAudioContext();
      if (!ac) return;
      const tones = {
        WAKE: [[660, 0.08], [880, 0.11]],
        DONE: [[880, 0.08], [1174, 0.12]],
        ALERT: [[988, 0.09], [784, 0.09], [988, 0.11]],
        ERROR: [[440, 0.12], [311, 0.16]],
        CLICK: [[1050, 0.04]],
      }[type] || [[800, 0.06]];

      let when = ac.currentTime;
      for (const [freq, dur] of tones) {
        const osc = ac.createOscillator();
        const gain = ac.createGain();
        osc.type = "sine";
        osc.frequency.setValueAtTime(freq, when);
        gain.gain.setValueAtTime(Math.max(0.03, (state.volume / 100) * 0.18), when);
        gain.gain.exponentialRampToValueAtTime(0.0001, when + dur);
        osc.connect(gain);
        gain.connect(ac.destination);
        osc.start(when);
        osc.stop(when + dur + 0.01);
        when += dur + 0.02;
      }
    } catch (_) {}
  }

  function stopMediaPlayback(silent = false) {
    if (state.ttsAudioElem) {
      try {
        state.ttsAudioElem.pause();
        state.ttsAudioElem.removeAttribute("src");
      } catch (_) {}
      state.ttsAudioElem = null;
    }
    if (window.speechSynthesis && window.speechSynthesis.speaking) {
      window.speechSynthesis.cancel();
    }
    state.syntheticSpeechLevel = 0;
    if (simAudio && !simAudio.paused) simAudio.pause();
    if (simAudio) {
      simAudio.removeAttribute("src");
      simAudio.load();
    }
    state.musicBanner = null;
    if (mjpegOverlay && mjpegOverlay.classList.contains("visible")) {
      mjpegOverlay.classList.remove("visible");
      if (mjpegImg) mjpegImg.removeAttribute("src");
    }
    if (["MUSIC", "VIDEO", "SPEAKING"].includes(state.fsm)) {
      setFsm("IDLE");
      switchScreen("eyes");
      setEmotion("binh_thuong");
    }
    if (!silent) logSerial("I", "AudioHal", "Đã dừng phát Nhạc / Video / TTS trên I2S1.");
  }

  function speakWebSpeechFallback(clean) {
    return new Promise((resolve) => {
      if (!("speechSynthesis" in window)) { resolve(); return; }
      try {
        window.speechSynthesis.cancel();
        const u = new SpeechSynthesisUtterance(clean);
        u.lang = "vi-VN";
        u.volume = Math.max(0.15, state.volume / 100);
        u.rate = 1.02;
        const viVoice = window.speechSynthesis.getVoices().find((v) => v.lang && v.lang.toLowerCase().startsWith("vi"));
        if (viVoice) u.voice = viVoice;
        state.syntheticSpeechLevel = 0.78;
        let done = false;
        const finish = () => {
          if (done) return;
          done = true;
          state.syntheticSpeechLevel = 0;
          resolve();
        };
        u.onend = finish;
        u.onerror = finish;
        window.speechSynthesis.speak(u);
        setTimeout(finish, Math.min(20000, Math.max(2500, clean.length * 95)));
      } catch (_) {
        state.syntheticSpeechLevel = 0;
        resolve();
      }
    });
  }

  async function speakTts(text) {
    const clean = (text || "").replace(/[*#`]+/g, " ").trim();
    if (!clean) return;
    setFsm("SPEAKING");
    ensureAudioContext();

    const ttsUrl = `/api/tts?text=${encodeURIComponent(clean)}&voice=${encodeURIComponent(state.ttsVoice || "vi-VN-HoaiMyNeural")}`;
    const audioPlayed = await new Promise((resolve) => {
      try {
        const audio = new Audio(ttsUrl);
        state.ttsAudioElem = audio;
        audio.volume = Math.max(0.1, Math.min(1, state.volume / 100));
        state.syntheticSpeechLevel = 0.78;
        let settled = false;
        const done = (ok) => {
          if (settled) return;
          settled = true;
          state.syntheticSpeechLevel = 0;
          if (state.ttsAudioElem === audio) state.ttsAudioElem = null;
          resolve(ok);
        };
        audio.onended = () => done(true);
        audio.onerror = () => done(false);
        const p = audio.play();
        if (p && typeof p.then === "function") {
          p.then(() => {
            logSerial("I", "AudioHal", `I2S_NUM_1 MAX98357A (GPIO44) phát TTS (${Math.round(audio.volume * 100)}%)`);
          }).catch((err) => {
            logSerial("W", "AudioHal", `Audio.play() chặn (${err.message}) -> Fallback speechSynthesis vi-VN`);
            done(false);
          });
        }
        setTimeout(() => done(true), 25000);
      } catch (e) {
        state.syntheticSpeechLevel = 0;
        resolve(false);
      }
    });

    if (!audioPlayed) await speakWebSpeechFallback(clean);
    if (state.fsm === "SPEAKING") setFsm("IDLE");
    touchActivity();
  }

  async function startMusicPlayback(sourceTag, title, playUrl) {
    stopMediaPlayback(true);
    touchActivity();
    setFsm("MUSIC");
    setEmotion("vui_ve");
    state.musicBanner = { sourceTag, title, scrollOffset: 0 };
    if (!simAudio) return;
    simAudio.src = playUrl;
    simAudio.volume = Math.max(0.1, Math.min(1, state.volume / 100));
    state.syntheticSpeechLevel = 0.68;
    try {
      await simAudio.play();
      logSerial("I", "AudioHal", `S3Mini tải MP3 + minimp3 -> I2S_NUM_1 (GPIO44): ${sourceTag} ${title}`);
    } catch (e) {
      state.syntheticSpeechLevel = 0;
      logSerial("W", "AudioHal", `Lỗi phát nhạc (${sourceTag}): ${e.message}`);
      showToast("Phát nhạc S3Mini", "Không phát được stream", e.message);
    }
  }

  let mjpegHideTimer = null;
  function enterVideoMode(title, playUrl) {
    stopMediaPlayback(true);
    setFsm("VIDEO");
    switchScreen("video");
    if (mjpegTitleBar) {
      mjpegTitleBar.textContent = title || "Video S.W.Ni-Oh";
      mjpegTitleBar.style.display = "block";
    }
    if (mjpegImg) mjpegImg.src = playUrl;
    if (mjpegOverlay) mjpegOverlay.classList.add("visible");
    if (mjpegHideTimer) clearTimeout(mjpegHideTimer);
    mjpegHideTimer = setTimeout(() => { if (mjpegTitleBar) mjpegTitleBar.style.display = "none"; }, 3000);
    logSerial("I", "UiEyesClock", `S3Mini MJPEG Video Mode 240x240: ${title}`);
  }

  // =========================================================================
  // 4. PC Microphone VAD -> 16kHz WAV -> Whisper STT
  // =========================================================================
  function floatTo16kWav(chunks, srcRate) {
    let totalLen = 0;
    for (const c of chunks) totalLen += c.length;
    const merged = new Float32Array(totalLen);
    let off = 0;
    for (const c of chunks) { merged.set(c, off); off += c.length; }

    const dstRate = 16000;
    const ratio = srcRate / dstRate;
    const outLen = Math.max(1, Math.floor(merged.length / ratio));
    const pcm = new Int16Array(outLen);
    for (let i = 0; i < outLen; i++) {
      const s = Math.max(-1, Math.min(1, merged[Math.min(merged.length - 1, Math.floor(i * ratio))]));
      pcm[i] = s < 0 ? s * 0x8000 : s * 0x7fff;
    }
    const buf = new ArrayBuffer(44 + pcm.length * 2);
    const view = new DataView(buf);
    const ws = (p, str) => { for (let i = 0; i < str.length; i++) view.setUint8(p + i, str.charCodeAt(i)); };
    ws(0, "RIFF"); view.setUint32(4, 36 + pcm.length * 2, true); ws(8, "WAVE"); ws(12, "fmt ");
    view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
    view.setUint32(24, dstRate, true); view.setUint32(28, dstRate * 2, true); view.setUint16(32, 2, true); view.setUint16(34, 16, true);
    ws(36, "data"); view.setUint32(40, pcm.length * 2, true);
    for (let i = 0; i < pcm.length; i++) view.setInt16(44 + i * 2, pcm[i], true);
    return new Blob([buf], { type: "audio/wav" });
  }

  async function enablePcMic() {
    const btnMic = byId("btnToggleMic");
    const pillMic = byId("pillMic");
    if (state.micActive) {
      state.micActive = false;
      state.vad.recording = false;
      if (state.micProc) { state.micProc.disconnect(); state.micProc = null; }
      if (state.micStream) { state.micStream.getTracks().forEach((t) => t.stop()); state.micStream = null; }
      if (btnMic) btnMic.textContent = "Bật Mic PC (VAD)";
      if (pillMic) { pillMic.textContent = "Mic PC: Chờ"; pillMic.classList.remove("active"); }
      return;
    }
    try {
      const ac = ensureAudioContext();
      const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true } });
      state.micStream = stream;
      state.micSampleRate = ac ? ac.sampleRate : 16000;
      const src = ac.createMediaStreamSource(stream);
      state.micAnalyser = ac.createAnalyser();
      state.micAnalyser.fftSize = 64;
      const proc = ac.createScriptProcessor(2048, 1, 1);
      proc.onaudioprocess = (ev) => {
        if (!state.micActive) return;
        const input = ev.inputBuffer.getChannelData(0);
        let sum = 0;
        for (let i = 0; i < input.length; i++) sum += input[i] * input[i];
        const rms = Math.sqrt(sum / input.length);
        const thresh = Math.max(0.008, 0.045 - state.micGain * 0.0032);
        if (state.fsm === "IDLE" && rms > thresh) startVoiceTurnFromMic(true);
        if (state.vad.recording) {
          state.vad.chunks.push(new Float32Array(input));
          if (rms > thresh) { state.vad.speechFrames++; state.vad.silenceFrames = 0; }
          else state.vad.silenceFrames++;
          const elapsed = performance.now() - state.vad.startedAt;
          const silLim = Math.ceil((1.2 * state.micSampleRate) / 2048);
          if ((state.vad.speechFrames >= 2 && state.vad.silenceFrames >= silLim) || elapsed >= state.vad.maxRecordMs) {
            finishMicCapture();
          }
        }
      };
      src.connect(state.micAnalyser);
      state.micAnalyser.connect(proc);
      proc.connect(ac.destination);
      state.micProc = proc;
      state.micActive = true;
      if (btnMic) btnMic.textContent = "Tắt Mic PC";
      if (pillMic) { pillMic.textContent = "Mic PC: Bật (VAD)"; pillMic.classList.add("active"); }
      logSerial("I", "AudioHal", `Mic INMP441/PC bật (${state.micSampleRate}Hz -> 16kHz WAV)`);
    } catch (e) {
      logSerial("E", "AudioHal", `Không mở được Mic PC: ${e.message}`);
      appendChat("sys", `Lỗi mở Mic PC: ${e.message}`);
    }
  }

  function startVoiceTurnFromMic(vad = false) {
    if (state.vad.recording) return;
    stopMediaPlayback(true);
    touchActivity();
    setFsm("LISTENING");
    setEmotion("dang_nghe");
    if (!vad) playChime("WAKE");
    state.vad.recording = true;
    state.vad.chunks = [];
    state.vad.speechFrames = vad ? 2 : 0;
    state.vad.silenceFrames = 0;
    state.vad.startedAt = performance.now();
    logSerial("I", "AudioHal", "Đang thu âm VAD...");
  }

  async function finishMicCapture() {
    if (!state.vad.recording) return;
    state.vad.recording = false;
    const chunks = state.vad.chunks;
    state.vad.chunks = [];
    if (chunks.length < 2) { setFsm("IDLE"); setEmotion("binh_thuong"); return; }

    setFsm("THINKING");
    setEmotion("suy_nghi");
    const wavBlob = floatTo16kWav(chunks, state.micSampleRate);
    try {
      const data = await (await fetch("/api/stt", { method: "POST", headers: { "Content-Type": "audio/wav" }, body: wavBlob })).json();
      if (!data.ok || !data.text) {
        appendChat("sys", `STT: ${data.error || "Không nghe rõ"}`);
        playChime("ERROR"); setFsm("IDLE"); setEmotion("binh_thuong");
        return;
      }
      logSerial("I", "NiOhAgent", `STT: "${data.text}"`);
      await processUserMessage(data.text);
    } catch (e) {
      logSerial("E", "NiOhAgent", `Lỗi STT: ${e.message}`);
      setFsm("IDLE"); setEmotion("buon");
    }
  }

  // =========================================================================
  // 5. Chat, MCP Side Effects & Hardware Controls
  // =========================================================================
  function applySideEffectEvents(events) {
    if (!Array.isArray(events)) return;
    for (const ev of events) {
      if (ev.type === "weather_update") {
        state.weather = { city: ev.city, temp_c: ev.temp_c, humidity: ev.humidity, condition: ev.condition };
      } else if (ev.type === "device_control") {
        if (ev.control === "am_luong") { state.volume = ev.value; if (byId("cfgVolume")) byId("cfgVolume").value = ev.value; showVolOsd(); }
        else if (ev.control === "do_nhay_mic") { state.micGain = ev.value; if (byId("cfgMicGain")) byId("cfgMicGain").value = ev.value; }
        else if (ev.control === "do_sang_man_hinh") { state.backlight = ev.value; if (byId("cfgBacklight")) byId("cfgBacklight").value = ev.value; }
        else if (ev.control === "xoay_man_hinh") { state.rotation = ev.value; if (byId("cfgRotation")) byId("cfgRotation").value = String(ev.value); }
        applyLcdHardware();
        showToast("Thiết bị", `Đã chỉnh ${ev.control}`, `Giá trị: ${ev.value}`);
      } else if (ev.type === "espnow_tx") {
        logSerial("NOW", "EspNowMesh", `TX ${ev.cmd} -> ${ev.mac}: ${ev.payload}`);
        showToast("ESP-NOW TX", ev.cmd, ev.payload);
      } else if (ev.type === "pomodoro_start") {
        showToast("Pomodoro", `Đặt ${ev.seconds} giây`, ev.note);
        setTimeout(async () => {
          playChime("ALERT"); touchActivity(); setEmotion("ngac_nhien");
          showToast("Hết giờ Pomodoro!", ev.note, "Nhấn nút để tiếp tục");
          appendChat("bot", `[Pomodoro] ${ev.note}`);
          await speakTts(`Đã hết giờ rồi! ${ev.note}`);
        }, ev.seconds * 1000);
      } else if (ev.type === "media_play") {
        if (ev.kind === "VIDEO_MJPEG") setTimeout(() => enterVideoMode(ev.title, ev.play_url), 250);
        else if (ev.kind === "AUDIO_MP3") setTimeout(() => startMusicPlayback(ev.source_tag, ev.title, ev.play_url), 250);
      }
    }
  }

  async function processUserMessage(userText) {
    ensureAudioContext();
    touchActivity();
    appendChat("user", userText);
    setFsm("THINKING");
    setEmotion("suy_nghi");
    logSerial("I", "NiOhAgent", `User: "${userText}"`);

    try {
      const data = await (await fetch("/api/chat", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ message: userText, history: state.chatHistory }),
      })).json();
      if (!data.ok) {
        appendChat("sys", data.error || "Lỗi LLM");
        logSerial("E", "NiOhAgent", data.error || "Lỗi LLM");
        playChime("ERROR"); setEmotion("buon"); setFsm("IDLE");
        return;
      }
      if (Array.isArray(data.tool_logs)) {
        for (const tl of data.tool_logs) logSerial("I", "McpVN", `${tl.tool}(${JSON.stringify(tl.args)})`);
      }
      state.chatHistory.push({ role: "user", content: userText }, { role: "assistant", content: data.raw_reply || data.reply });
      if (state.chatHistory.length > 16) state.chatHistory = state.chatHistory.slice(-16);

      setEmotion(data.emotion || "binh_thuong");
      appendChat("bot", `[${data.emotion}] ${data.reply}`);
      await speakTts(data.reply);
      applySideEffectEvents(data.events);
    } catch (e) {
      logSerial("E", "NiOhAgent", `Chat error: ${e.message}`);
      setFsm("IDLE"); setEmotion("buon");
    }
  }

  const pressState = { mainDownAt: 0, timerId: null };
  function handleBtnMainDown() {
    if (pressState.mainDownAt) return;
    if (byId("btnMain")) byId("btnMain").classList.add("pressed");
    pressState.mainDownAt = performance.now();
    pressState.timerId = setInterval(() => {
      if (!pressState.mainDownAt || !btnHoldFill) return;
      btnHoldFill.style.width = `${Math.min(100, ((performance.now() - pressState.mainDownAt) / BTN_LONG_PORTAL_MS) * 100)}%`;
    }, 30);
  }
  function handleBtnMainUp() {
    if (!pressState.mainDownAt) return;
    const held = performance.now() - pressState.mainDownAt;
    pressState.mainDownAt = 0;
    clearInterval(pressState.timerId);
    if (btnHoldFill) btnHoldFill.style.width = "0%";
    if (byId("btnMain")) byId("btnMain").classList.remove("pressed");
    if (held < BTN_DEBOUNCE_MS) return;

    if (held >= BTN_LONG_PORTAL_MS) {
      stopMediaPlayback(true); playChime("ALERT"); setFsm("PORTAL"); setEmotion("ngac_nhien");
      showToast("Web Portal", "Wi-Fi: S.W.Ni-Oh-Setup", "Mở http://192.168.4.1");
      const tc = document.querySelector('[data-tab="tabConfig"]');
      if (tc) tc.click();
      return;
    }
    if (held >= BTN_LONG_RESET_MS) {
      stopMediaPlayback(true); state.chatHistory = []; playChime("DONE");
      setFsm("IDLE"); switchScreen("eyes"); setEmotion("binh_thuong");
      showToast("Hệ thống", "Đã đặt lại phiên", "Xóa ngữ cảnh hội thoại");
      touchActivity();
      return;
    }
    if (["VIDEO", "MUSIC", "SPEAKING"].includes(state.fsm)) {
      stopMediaPlayback(false); playChime("CLICK");
      return;
    }
    if (state.vad.recording) { finishMicCapture(); return; }
    if (!state.micActive) enablePcMic().then(() => { if (state.micActive) startVoiceTurnFromMic(false); });
    else startVoiceTurnFromMic(false);
  }

  function adjustVolume(delta) {
    touchActivity();
    state.volume = Math.max(0, Math.min(100, state.volume + delta));
    if (byId("cfgVolume")) byId("cfgVolume").value = state.volume;
    applyLcdHardware();
    showVolOsd();
    playChime("CLICK");
  }

  // =========================================================================
  // 6. ESP-Claw LlmPage 1:1 Presets + Auto-Pick Model từ Key + Fallback 429
  // =========================================================================
  function renderPresetButtons() {
    const box = byId("presetButtons");
    if (!box || !state.cfg) return;
    const p = (state.cfg.llm_profiles || [])[state.curProfIdx] || {};
    box.innerHTML = PRESET_BUTTONS.map((pr) =>
      `<button type="button" class="${p.provider === pr.id ? "primary" : "secondary"}" data-preset="${pr.id}" style="padding:5px 10px;font-size:12px;">${pr.label}</button>`
    ).join("");
    box.querySelectorAll("button[data-preset]").forEach((btn) => {
      btn.addEventListener("click", () => {
        const pr = PROVIDER_PRESETS[btn.dataset.preset];
        if (!pr) return;
        const prof = state.cfg.llm_profiles[state.curProfIdx];
        prof.provider = pr.id;
        prof.name = state.curProfIdx === state.cfg.active_profile_idx ? `${pr.label} Primary` : `${pr.label} Fallback`;
        prof.base_url = pr.base_url;
        prof.model = pr.model;
        prof.auth_scheme = pr.auth_scheme;
        prof.raw_endpoint = false;
        syncCurProfileToUI();
        renderProfileSelect();
        renderPresetButtons();
        const adv = byId("llmAdvancedBox");
        if (adv) adv.open = !!pr.advanced;
        if (byId("llmTestResult")) {
          byId("llmTestResult").textContent = `Đã áp dụng preset ${pr.label} (${pr.model}). Chỉ cần dán API Key và nhấn Save!`;
        }
      });
    });
  }

  async function autoDetectFromKey(forceReplaceModel = false) {
    readUIToCurProfile();
    const prof = (state.cfg && state.cfg.llm_profiles[state.curProfIdx]) || {};
    if (!prof.api_key && !prof.has_key) return;
    const out = byId("llmTestResult");
    if (out) out.textContent = "Đang tự nhận diện Key & chọn Model phù hợp nhất (/models)...";
    try {
      const res = await (await fetch("/api/llm_autodetect", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ profile_idx: state.curProfIdx, profile: prof }),
      })).json();
      if (res.provider) prof.provider = res.provider;
      if (res.base_url) prof.base_url = res.base_url;
      if (res.auth_scheme) prof.auth_scheme = res.auth_scheme;
      prof.raw_endpoint = false;
      if (Array.isArray(res.models) && byId("modelDatalist")) {
        byId("modelDatalist").innerHTML = res.models.map((m) => `<option value="${m}"></option>`).join("");
      }
      if (res.suggested_model && (forceReplaceModel || !prof.model || res.provider)) {
        prof.model = res.suggested_model;
      }
      syncCurProfileToUI();
      renderPresetButtons();
      if (out) {
        out.textContent = res.ok
          ? `Đã tự nhận diện [${res.provider}] và chọn Model: ${res.suggested_model} (tìm thấy ${(res.models || []).length} model hợp lệ từ Key).`
          : `Chưa lấy được /models (${res.error || "kiểm tra lại Key hoặc chọn Preset"}).`;
      }
    } catch (e) {
      if (out) out.textContent = `Lỗi tự nhận diện Key: ${e.message}`;
    }
  }

  function renderProfileSelect() {
    const sel = byId("profileSelect");
    if (!sel || !state.cfg) return;
    const profs = state.cfg.llm_profiles || [];
    sel.innerHTML = profs.map((p, i) => `<option value="${i}">#${i + 1}: ${p.name || p.provider}${i === state.cfg.active_profile_idx ? " ★ Primary" : " (Fallback)"}</option>`).join("");
    sel.value = String(state.curProfIdx);
  }

  function syncCurProfileToUI() {
    const profs = (state.cfg && state.cfg.llm_profiles) || [];
    const p = profs[state.curProfIdx] || {};
    if (byId("profName")) byId("profName").value = p.name || "";
    if (byId("profProvider")) byId("profProvider").value = PROVIDER_PRESETS[p.provider] ? p.provider : "custom";
    if (byId("profAuthScheme")) byId("profAuthScheme").value = p.auth_scheme || "bearer";
    if (byId("profCustomAuth")) byId("profCustomAuth").value = p.custom_auth_name || "";
    if (byId("profBaseUrl")) byId("profBaseUrl").value = p.base_url || "";
    if (byId("profRawEndpoint")) byId("profRawEndpoint").checked = !!p.raw_endpoint;
    if (byId("profModel")) byId("profModel").value = p.model || "";
    if (byId("profApiKey")) {
      byId("profApiKey").value = "";
      byId("profApiKey").placeholder = p.has_key ? "Đã lưu API Key (dán key mới để đổi)" : "Dán API Key (gsk_..., AIza..., sk-or-..., sk-...)";
    }
    if (byId("profSetPrimary")) byId("profSetPrimary").checked = (state.curProfIdx === state.cfg.active_profile_idx);
    if (byId("profEnabled")) byId("profEnabled").checked = p.enabled !== false;
  }

  function readUIToCurProfile() {
    const profs = (state.cfg && state.cfg.llm_profiles) || [];
    const p = profs[state.curProfIdx];
    if (!p) return;
    if (byId("profName")) p.name = byId("profName").value.trim() || "Profile";
    if (byId("profProvider")) p.provider = byId("profProvider").value;
    if (byId("profAuthScheme")) p.auth_scheme = byId("profAuthScheme").value;
    if (byId("profCustomAuth")) p.custom_auth_name = byId("profCustomAuth").value.trim();
    if (byId("profBaseUrl")) p.base_url = byId("profBaseUrl").value.trim();
    if (byId("profRawEndpoint")) p.raw_endpoint = byId("profRawEndpoint").checked;
    if (byId("profModel")) p.model = byId("profModel").value.trim();
    const keyEl = byId("profApiKey");
    if (keyEl && keyEl.value.trim()) { p.api_key = keyEl.value.trim(); p.has_key = true; }
    if (byId("profEnabled")) p.enabled = byId("profEnabled").checked;
    if (byId("profSetPrimary") && byId("profSetPrimary").checked) {
      state.cfg.active_profile_idx = state.curProfIdx;
    }
  }

  async function loadConfigFromServer() {
    const cfg = await (await fetch("/api/config")).json();
    state.cfg = cfg;
    state.curProfIdx = cfg.active_profile_idx || 0;
    if (byId("cfgAutoFallback")) byId("cfgAutoFallback").checked = cfg.auto_fallback !== false;
    renderProfileSelect();
    syncCurProfileToUI();
    renderPresetButtons();

    if (byId("cfgSearchEngine")) byId("cfgSearchEngine").value = cfg.search_engine || "ddg_wiki";
    if (byId("cfgSearchKey")) byId("cfgSearchKey").placeholder = cfg.has_search_key ? "Đã lưu (để trống để giữ)" : "Nhập Search API Key";
    if (byId("cfgSearxngUrl")) byId("cfgSearxngUrl").value = cfg.searxng_url || "";
    if (byId("cfgPipedInstances")) byId("cfgPipedInstances").value = cfg.piped_instances || "";
    if (byId("cfgMediaProxy")) byId("cfgMediaProxy").value = cfg.media_proxy_url || "";
    if (byId("cfgScCid")) byId("cfgScCid").value = cfg.soundcloud_client_id || "";
    if (byId("cfgVideoRes")) byId("cfgVideoRes").value = cfg.video_res || "240x240";
    if (byId("cfgVideoFps")) byId("cfgVideoFps").value = String(cfg.video_fps || 12);
    if (byId("cfgVideoQuality")) byId("cfgVideoQuality").value = cfg.video_jpeg_quality || 75;

    if (byId("cfgSttKey")) byId("cfgSttKey").placeholder = cfg.has_stt_key ? "Đã lưu (để trống để giữ)" : "gsk_...";
    state.ttsVoice = cfg.tts_voice || "vi-VN-HoaiMyNeural";
    if (byId("cfgTtsVoice")) byId("cfgTtsVoice").value = state.ttsVoice;
    if (byId("cfgTtsGoogle")) byId("cfgTtsGoogle").value = String(!!cfg.tts_prefer_google);
    state.volume = Number(cfg.volume ?? 85);
    state.micGain = Number(cfg.mic_gain ?? 6);
    state.backlight = Number(cfg.backlight ?? 85);
    state.rotation = Number(cfg.rotation ?? 0);
    if (byId("cfgVolume")) byId("cfgVolume").value = state.volume;
    if (byId("cfgMicGain")) byId("cfgMicGain").value = state.micGain;
    if (byId("cfgBacklight")) byId("cfgBacklight").value = state.backlight;
    if (byId("cfgRotation")) byId("cfgRotation").value = String(state.rotation);
    applyLcdHardware();
  }

  async function saveAllConfig() {
    readUIToCurProfile();
    state.cfg.auto_fallback = byId("cfgAutoFallback") ? byId("cfgAutoFallback").checked : true;
    state.volume = Number((byId("cfgVolume") && byId("cfgVolume").value) || 85);
    state.micGain = Number((byId("cfgMicGain") && byId("cfgMicGain").value) || 6);
    state.backlight = Number((byId("cfgBacklight") && byId("cfgBacklight").value) || 85);
    state.rotation = Number((byId("cfgRotation") && byId("cfgRotation").value) || 0);
    state.ttsVoice = (byId("cfgTtsVoice") && byId("cfgTtsVoice").value) || "vi-VN-HoaiMyNeural";
    applyLcdHardware();

    const payload = {
      llm_profiles: state.cfg.llm_profiles.map((p) => ({ ...p, api_key: p.api_key && p.api_key !== "__KEEP__" ? p.api_key : "__KEEP__" })),
      active_profile_idx: state.cfg.active_profile_idx || 0,
      auto_fallback: state.cfg.auto_fallback,
      search_engine: byId("cfgSearchEngine") ? byId("cfgSearchEngine").value : "ddg_wiki",
      search_api_key: (byId("cfgSearchKey") && byId("cfgSearchKey").value.trim()) || "__KEEP__",
      searxng_url: byId("cfgSearxngUrl") ? byId("cfgSearxngUrl").value.trim() : "",
      piped_instances: byId("cfgPipedInstances") ? byId("cfgPipedInstances").value.trim() : "",
      media_proxy_url: byId("cfgMediaProxy") ? byId("cfgMediaProxy").value.trim() : "",
      soundcloud_client_id: byId("cfgScCid") ? byId("cfgScCid").value.trim() : "",
      video_res: byId("cfgVideoRes") ? byId("cfgVideoRes").value : "240x240",
      video_fps: Number((byId("cfgVideoFps") && byId("cfgVideoFps").value) || 12),
      video_jpeg_quality: Number((byId("cfgVideoQuality") && byId("cfgVideoQuality").value) || 75),
      stt_api_key: (byId("cfgSttKey") && byId("cfgSttKey").value.trim()) || "__KEEP__",
      tts_voice: state.ttsVoice,
      tts_prefer_google: byId("cfgTtsGoogle") ? byId("cfgTtsGoogle").value === "true" : false,
      volume: state.volume,
      mic_gain: state.micGain,
      backlight: state.backlight,
      rotation: state.rotation,
    };
    const r = await (await fetch("/api/config", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload) })).json();
    if (r.ok) {
      await loadConfigFromServer();
      playChime("DONE");
      showToast("S.W.Ni-Oh", "Đã lưu cấu hình", "Multi-Provider & S3Mini Media!");
      logSerial("I", "NiOhAgent", "Đã lưu sim_config.json (ESP-Claw Multi-Provider + Media).");
    }
  }

  // =========================================================================
  // 7. BLE Chronos Auto-Sync + AVRCP Remote (Tách bạch khỏi Media S3Mini)
  // =========================================================================
  function renderChronosStatus() {
    const c = state.chronos;
    if (byId("pillBle")) byId("pillBle").textContent = `BLE: ${c.connected ? "Connected" : 'S.W.Ni-Oh (Advertising)'}`;
    if (byId("chronosStatusBox")) {
      byId("chronosStatusBox").textContent =
        `Trạng thái BLE GATT : ${c.modeText}\n` +
        `Pin điện thoại (0x91): ${c.phoneBattery}%\n` +
        `Giờ đồng bộ (0x93)   : ${c.lastTimeSync}\n` +
        `Thời tiết (0x88)     : ${state.weather.temp_c}°C - ${state.weather.condition} (${state.weather.humidity}%)\n` +
        `Thông báo đẩy (0x72) : ${c.lastNotif}\n` +
        `Chỉ đường Maps (0xEF): ${state.nav ? `${state.nav.direction} (${state.nav.distance}) - ${state.nav.street}` : "Không hoạt động"}\n` +
        `Tổng gói đã nhận     : ${c.rxPackets}${c.lastHex ? `\nGói mới nhất (HEX)   : ${c.lastHex}` : ""}`;
    }
  }

  async function runChronosDemoStep() {
    try {
      const ev = await (await fetch(`/api/chronos_demo_tick?step=${state.chronos.autoDemoStep++}`)).json();
      state.chronos.connected = true;
      state.chronos.modeText = "Auto-Sync Connected (Virtual Chronos Phone)";
      state.chronos.rxPackets += 1;
      state.chronos.lastHex = ev.hex;
      if (ev.kind === "time_battery") {
        state.chronos.phoneBattery = ev.battery;
        state.chronos.lastTimeSync = ev.time;
        logSerial("BLE", "ChronosBLE", `Auto-Sync 0x93 Time=${ev.time} | 0x91 Bat=${ev.battery}%`);
      } else if (ev.kind === "weather") {
        state.weather = { city: "Chronos Phone", temp_c: ev.temp_c, humidity: ev.humidity, condition: ev.condition };
        logSerial("BLE", "ChronosBLE", `Auto-Sync 0x88 Weather=${ev.temp_c}°C (${ev.condition})`);
      } else if (ev.kind === "notification") {
        state.chronos.lastNotif = `[${ev.app}] ${ev.title}: ${ev.body}`;
        touchActivity(); playChime("ALERT");
        showToast(ev.app, ev.title, ev.body, 4000);
        logSerial("BLE", "ChronosBLE", `Auto-Sync 0x72 [${ev.app}] ${ev.title}`);
      } else if (ev.kind === "navigation") {
        state.nav = { icon: ev.icon, direction: ev.direction, distance: ev.distance, street: ev.street };
        switchScreen("clock");
        logSerial("BLE", "ChronosBLE", `Auto-Sync 0xEF Maps: ${ev.direction} (${ev.distance}) ${ev.street}`);
      }
      renderChronosStatus();
    } catch (e) {
      logSerial("W", "ChronosBLE", `Auto-Sync lỗi: ${e.message}`);
    }
  }

  async function sendAvrcp(cmd) {
    try {
      const r = await (await fetch("/api/chronos_avrcp", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ cmd }),
      })).json();
      if (byId("avrcpOut")) byId("avrcpOut").textContent = `Đã gửi AVRCP "${r.cmd}" qua BLE NUS (${r.ble_hex})`;
      logSerial("BLE", "ChronosBLE", `TX AVRCP ${r.cmd} (${r.ble_hex})`);
      playChime("CLICK");
    } catch (e) {
      if (byId("avrcpOut")) byId("avrcpOut").textContent = `Lỗi AVRCP: ${e.message}`;
    }
  }

  async function loadFirmwareInfo() {
    const box = byId("fwInspectorBox");
    if (!box) return;
    try {
      const info = await (await fetch("/api/firmware_info")).json();
      const bin = info.bin || {};
      const statusHtml = bin.exists
        ? `<span style="color:var(--ok);font-weight:700;">ĐÃ TÌM THẤY (${bin.size_kb} KB / ${bin.factory_limit_kb} KB = ${bin.usage_pct}% | Magic=${bin.esp_magic || "?"} | Entry=${bin.entry_point || "?"})</span>`
        : `<span style="color:var(--warn);font-weight:700;">CHƯA BIÊN DỊCH</span>`;
      const rows = (info.partitions || []).map((p) => `<tr><td>${p.name}</td><td>${p.type}</td><td>${p.subtype}</td><td>${p.offset}</td><td>${p.size}</td></tr>`).join("");
      box.innerHTML = `
        <div style="font-size:12px;line-height:1.6;">
          <div><b>Project:</b> <code>${info.project_name}</code> (${info.brand})</div>
          <div><b>Firmware Binary:</b> <code>${bin.path || "build/sw_nioh.bin"}</code> &mdash; ${statusHtml}</div>
          <div><b>Components (${(info.components || []).length}):</b> <code>${(info.components || []).join(", ")}</code></div>
        </div>
        <table class="fw-table"><thead><tr><th>Phân vùng</th><th>Loại</th><th>Subtype</th><th>Offset</th><th>Kích thước</th></tr></thead><tbody>${rows}</tbody></table>`;
    } catch (e) {
      box.textContent = `Không đọc được thông tin firmware: ${e.message}`;
    }
  }

  // =========================================================================
  // 8. Bind UI Events Safely
  // =========================================================================
  function bindEvents() {
    const btnMain = byId("btnMain");
    if (btnMain) {
      btnMain.addEventListener("mousedown", handleBtnMainDown);
      btnMain.addEventListener("touchstart", (e) => { e.preventDefault(); handleBtnMainDown(); });
    }
    window.addEventListener("mouseup", handleBtnMainUp);
    window.addEventListener("touchend", handleBtnMainUp);
    if (byId("btnVolUp")) byId("btnVolUp").addEventListener("click", () => adjustVolume(10));
    if (byId("btnVolDown")) byId("btnVolDown").addEventListener("click", () => adjustVolume(-10));
    window.addEventListener("keydown", (e) => {
      if (document.activeElement && ["INPUT", "TEXTAREA", "SELECT"].includes(document.activeElement.tagName)) return;
      if (e.code === "Space") { e.preventDefault(); if (!e.repeat) handleBtnMainDown(); }
      else if (e.code === "ArrowUp" || e.key === "+") { e.preventDefault(); adjustVolume(10); }
      else if (e.code === "ArrowDown" || e.key === "-") { e.preventDefault(); adjustVolume(-10); }
    });
    window.addEventListener("keyup", (e) => {
      if (document.activeElement && ["INPUT", "TEXTAREA", "SELECT"].includes(document.activeElement.tagName)) return;
      if (e.code === "Space") { e.preventDefault(); handleBtnMainUp(); }
    });

    document.querySelectorAll(".tab-btn").forEach((btn) => {
      btn.addEventListener("click", () => {
        document.querySelectorAll(".tab-btn").forEach((b) => b.classList.remove("active"));
        document.querySelectorAll(".tab-pane").forEach((p) => p.classList.remove("active"));
        btn.classList.add("active");
        const target = byId(btn.dataset.tab);
        if (target) target.classList.add("active");
      });
    });

    document.querySelectorAll(".emo-btn").forEach((btn) => {
      btn.addEventListener("click", () => { touchActivity(); setEmotion(btn.dataset.emo); });
    });

    if (byId("btnToggleMic")) byId("btnToggleMic").addEventListener("click", enablePcMic);
    if (byId("btnStopMedia")) byId("btnStopMedia").addEventListener("click", () => { stopMediaPlayback(false); playChime("CLICK"); });
    if (byId("btnTestTts")) byId("btnTestTts").addEventListener("click", () => speakTts("Xin chào! Tôi là S.W.Ni-Oh, trợ lý AI tiếng Việt của bạn."));
    if (byId("btnSwitchScreen")) {
      byId("btnSwitchScreen").addEventListener("click", () => {
        if (state.screen === "clock") { touchActivity(); switchScreen("eyes"); }
        else switchScreen("clock");
      });
    }

    if (byId("chatForm")) {
      byId("chatForm").addEventListener("submit", async (e) => {
        e.preventDefault();
        const txt = chatInput ? chatInput.value.trim() : "";
        if (!txt) return;
        chatInput.value = "";
        await processUserMessage(txt);
      });
    }

    if (byId("profileSelect")) {
      byId("profileSelect").addEventListener("change", (e) => {
        readUIToCurProfile();
        state.curProfIdx = Number(e.target.value);
        syncCurProfileToUI();
        renderProfileSelect();
        renderPresetButtons();
      });
    }
    if (byId("profProvider")) {
      byId("profProvider").addEventListener("change", (e) => {
        const pr = PROVIDER_PRESETS[e.target.value];
        if (!pr) return;
        if (byId("profBaseUrl") && pr.base_url !== undefined) byId("profBaseUrl").value = pr.base_url;
        if (byId("profModel") && pr.model) byId("profModel").value = pr.model;
        if (byId("profAuthScheme")) byId("profAuthScheme").value = pr.auth_scheme;
        if (byId("profRawEndpoint")) byId("profRawEndpoint").checked = false;
        renderPresetButtons();
      });
    }
    let keyDetectTimer = null;
    if (byId("profApiKey")) {
      const triggerDetect = () => {
        const v = byId("profApiKey").value.trim();
        if (v.length >= 16) autoDetectFromKey(true);
      };
      byId("profApiKey").addEventListener("input", () => {
        if (keyDetectTimer) clearTimeout(keyDetectTimer);
        keyDetectTimer = setTimeout(triggerDetect, 550);
      });
      byId("profApiKey").addEventListener("change", triggerDetect);
    }
    if (byId("profSetPrimary")) {
      byId("profSetPrimary").addEventListener("change", (e) => {
        if (e.target.checked) {
          state.cfg.active_profile_idx = state.curProfIdx;
          renderProfileSelect();
          renderPresetButtons();
        }
      });
    }
    if (byId("btnAddProfile")) {
      byId("btnAddProfile").addEventListener("click", () => {
        if (!state.cfg || state.cfg.llm_profiles.length >= 16) return;
        readUIToCurProfile();
        state.cfg.llm_profiles.push({
          name: `Fallback #${state.cfg.llm_profiles.length + 1}`,
          provider: "openrouter",
          base_url: "https://openrouter.ai/api/v1",
          model: "openai/gpt-4o-mini",
          api_key: "",
          has_key: false,
          auth_scheme: "bearer",
          custom_auth_name: "",
          raw_endpoint: false,
          enabled: true,
        });
        state.curProfIdx = state.cfg.llm_profiles.length - 1;
        renderProfileSelect();
        syncCurProfileToUI();
        renderPresetButtons();
      });
    }
    if (byId("btnDelProfile")) {
      byId("btnDelProfile").addEventListener("click", () => {
        if (!state.cfg || state.cfg.llm_profiles.length <= 1) return;
        state.cfg.llm_profiles.splice(state.curProfIdx, 1);
        state.curProfIdx = 0;
        state.cfg.active_profile_idx = 0;
        renderProfileSelect();
        syncCurProfileToUI();
        renderPresetButtons();
      });
    }
    if (byId("btnFetchModels")) {
      byId("btnFetchModels").addEventListener("click", () => autoDetectFromKey(true));
    }
    if (byId("btnTestLlm")) {
      byId("btnTestLlm").addEventListener("click", async () => {
        readUIToCurProfile();
        const out = byId("llmTestResult");
        if (out) out.textContent = "Đang Ping LLM kiểm tra kết nối...";
        const res = await (await fetch("/api/llm_test", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ profile_idx: state.curProfIdx, profile: state.cfg.llm_profiles[state.curProfIdx] }),
        })).json();
        if (out) {
          out.textContent = res.ok
            ? `KẾT NỐI THÀNH CÔNG (${res.latency_ms} ms):\n${res.reply}`
            : `KẾT NỐI THẤT BẠI (${res.latency_ms || 0} ms):\n${res.error}`;
        }
      });
    }
    if (byId("btnSaveAllConfig")) byId("btnSaveAllConfig").addEventListener("click", saveAllConfig);
    if (byId("btnSaveServices")) byId("btnSaveServices").addEventListener("click", saveAllConfig);

    if (byId("btnChronosAutoSim")) {
      byId("btnChronosAutoSim").addEventListener("click", () => {
        const btn = byId("btnChronosAutoSim");
        if (!state.chronos.autoDemoTimer) {
          runChronosDemoStep();
          state.chronos.autoDemoTimer = setInterval(runChronosDemoStep, 4500);
          btn.textContent = "Dừng Mô phỏng Điện thoại Chronos (Auto-Sync đang chạy)";
          logSerial("BLE", "ChronosBLE", "Bật Auto-Sync Chronos (0x93/0x88/0x72/0xEF/0x91 mỗi 4.5s).");
        } else {
          clearInterval(state.chronos.autoDemoTimer);
          state.chronos.autoDemoTimer = null;
          state.chronos.connected = false;
          state.chronos.modeText = 'Advertising "S.W.Ni-Oh"';
          btn.textContent = "Mô phỏng Điện thoại Chronos đang kết nối (Auto-Sync)";
          renderChronosStatus();
          logSerial("BLE", "ChronosBLE", "Đã dừng Mô phỏng Chronos Auto-Sync.");
        }
      });
    }

    if (byId("btnWebBleConnect")) {
      byId("btnWebBleConnect").addEventListener("click", async () => {
        if (!navigator.bluetooth) {
          logSerial("W", "ChronosBLE", "Trình duyệt chưa hỗ trợ Web Bluetooth (dùng Chrome/Edge localhost).");
          return;
        }
        try {
          const dev = await navigator.bluetooth.requestDevice({
            filters: [{ namePrefix: "S.W.Ni-Oh" }],
            optionalServices: ["6e400001-b5a3-f393-e0a9-e50e24dcca9e"],
          });
          await dev.gatt.connect();
          state.chronos.connected = true;
          state.chronos.modeText = `Connected (${dev.name || "S.W.Ni-Oh"})`;
          logSerial("BLE", "ChronosBLE", `Đã kết nối Web Bluetooth tới ${dev.name}`);
          renderChronosStatus();
        } catch (e) {
          logSerial("W", "ChronosBLE", `Web Bluetooth: ${e.message}`);
        }
      });
    }

    if (byId("btnAvrcpPrev")) byId("btnAvrcpPrev").addEventListener("click", () => sendAvrcp("prev"));
    if (byId("btnAvrcpPlay")) byId("btnAvrcpPlay").addEventListener("click", () => sendAvrcp("play_pause"));
    if (byId("btnAvrcpNext")) byId("btnAvrcpNext").addEventListener("click", () => sendAvrcp("next"));

    if (byId("btnEspNowRx")) {
      byId("btnEspNowRx").addEventListener("click", async () => {
        const payload = (byId("espnowPayload") && byId("espnowPayload").value) || "ALERT";
        state.espnowSeq = (state.espnowSeq + 1) & 0xff;
        const res = await (await fetch("/api/espnow_packet", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ cmd: "ALERT", seq: state.espnowSeq, payload }),
        })).json();
        if (byId("mcpJsonOut")) byId("mcpJsonOut").textContent = JSON.stringify(res, null, 2);
        logSerial("NOW", "EspNowMesh", `RX ALERT seq=${res.seq}: ${payload}`);
        touchActivity(); playChime("ALERT"); showToast("ESP-NOW ALERT", "Node Mesh", payload);
      });
    }

    if (byId("btnRefreshFw")) byId("btnRefreshFw").addEventListener("click", loadFirmwareInfo);
    if (byId("btnClearSerial")) byId("btnClearSerial").addEventListener("click", () => { if (serialConsole) serialConsole.innerHTML = ""; });
  }

  window.runMcpTest = async (tool, args) => {
    const out = byId("mcpJsonOut");
    if (out) out.textContent = `S3Mini đang chạy ${tool}...`;
    const data = await (await fetch("/api/mcp", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ tool, args }) })).json();
    if (out) out.textContent = JSON.stringify(data.result, null, 2);
    applySideEffectEvents(data.events);
  };

  bindEvents();

  (async () => {
    logSerial("I", "boot", "=== S.W.Ni-Oh | ESP32-S3 SuperMini 4MB Simulator (ESP-Claw) ===");
    logSerial("BLE", "ChronosBLE", "GATT Server đang quảng bá: S.W.Ni-Oh (0x93/0x88/0x72/0xEF/0x91)");
    try { await loadConfigFromServer(); } catch (e) { logSerial("W", "config", `Load config note: ${e.message}`); }
    renderChronosStatus();
    try {
      const ln = (await (await fetch("/api/lunar_now")).json()).lunar;
      state.lunar.text = `Âm lịch: ${String(ln.lunar_day).padStart(2, "0")}/${String(ln.lunar_month).padStart(2, "0")} (${ln.can_chi_year})`;
    } catch (_) {}
    await loadFirmwareInfo();
    showToast("S.W.Ni-Oh", "Simulator Sẵn Sàng", "Nhấn Space hoặc nút Thử Loa TTS");
  })();
})();
