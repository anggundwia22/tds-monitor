const pages = document.querySelectorAll(".page");
const navs = document.querySelectorAll(".nav");

function showPage(name) {
  pages.forEach((page) => page.classList.toggle("visible", page.id === name));
  navs.forEach((btn) => btn.classList.toggle("active", btn.dataset.page === name));
  if (name === "settings") loadSettings();
  if (name === "data") loadDataInfo();
}

navs.forEach((btn) => {
  btn.addEventListener("click", () => showPage(btn.dataset.page));
});

const msgTimers = {};

function setMsg(id, text, ok) {
  const el = document.getElementById(id);
  el.textContent = text;
  el.className = "msg " + (ok ? "ok" : "bad");
  clearTimeout(msgTimers[id]);
  msgTimers[id] = setTimeout(() => {
    el.textContent = "";
    el.className = "msg";
  }, 2500);
}

function mark(id, text) {
  const el = document.getElementById(id);
  el.textContent = text;
  const dot = el.parentElement.querySelector(".dot");
  const bad = text === "NOT FOUND" || text === "error";
  const good = !bad && text === "connected";
  dot.classList.toggle("ok", good);
  dot.classList.toggle("bad", bad);
}

function formatBytes(bytes) {
  if (bytes < 1024) return bytes + " B";
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(bytes < 10 * 1024 ? 1 : 0) + " KB";
  return (bytes / (1024 * 1024)).toFixed(2) + " MB";
}

function formatCount(n) {
  return Number(n).toLocaleString("en-US");
}

async function refreshDashboard() {
  try {
    const [sensor, status] = await Promise.all([
      fetch("/api/sensor").then((r) => r.json()),
      fetch("/api/status").then((r) => r.json()),
    ]);
    document.getElementById("tds").textContent = sensor.tds == null ? "--" : sensor.tds;
    document.getElementById("temp").textContent = sensor.temperature == null ? "--" : sensor.temperature;
    document.getElementById("ph").textContent = sensor.ph == null ? "--" : sensor.ph;
    document.getElementById("datetime").textContent = sensor.datetime || "--";
    mark("stRtc", status.rtcLostPower && status.rtc === "connected" ? "connected, time not set" : status.rtc);
    mark("stDs", status.ds18b20);
    mark("stTds", status.tds);
    mark("stPh", status.ph);
  } catch (err) {
    document.getElementById("datetime").textContent = "offline";
  }
}

function isoDate(ddmmyyyy) {
  const parts = (ddmmyyyy || "").split("-");
  if (parts.length !== 3) return "";
  return parts[2] + "-" + parts[1] + "-" + parts[0];
}

async function loadSettings() {
  try {
    const [config, rtc] = await Promise.all([
      fetch("/api/config").then((r) => r.json()),
      fetch("/api/rtc").then((r) => r.json()),
    ]);
    document.getElementById("measure").value = String(Math.round(config.measurementInterval / 1000));
    document.getElementById("log").value = String(Math.round(config.logInterval / 1000));
    if (rtc.date) document.getElementById("date").value = isoDate(rtc.date);
    if (rtc.time) document.getElementById("time").value = rtc.time;
  } catch (err) {
    setMsg("intervalMsg", "Could not load settings", false);
  }
}

document.getElementById("intervalForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const measureSec = Number(document.getElementById("measure").value);
  const logSec = Number(document.getElementById("log").value);
  if (!Number.isInteger(measureSec) || !Number.isInteger(logSec) || measureSec < 1 || logSec < 1 || measureSec > 86400 || logSec > 86400) {
    setMsg("intervalMsg", "Enter 1 to 86400 seconds", false);
    return;
  }
  const res = await fetch("/api/config", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      measurementInterval: measureSec * 1000,
      logInterval: logSec * 1000,
    }),
  });
  setMsg("intervalMsg", res.ok ? "Settings saved" : "Save failed", res.ok);
});

document.getElementById("rtcForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const body = {
    date: document.getElementById("date").value,
    time: document.getElementById("time").value,
  };
  const res = await fetch("/api/rtc", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await res.json().catch(() => ({}));
  setMsg("rtcMsg", res.ok ? "RTC set to " + data.datetime : (data.error || "Set RTC failed"), res.ok);
});

document.getElementById("restartBtn").addEventListener("click", async () => {
  if (!confirm("Are you sure you want to restart the device?")) return;
  setMsg("restartMsg", "Restarting...", true);
  try {
    await fetch("/api/restart", { method: "POST" });
  } catch (err) {
    setMsg("restartMsg", "Restart sent", true);
  }
});

async function loadDataInfo() {
  try {
    const info = await fetch("/api/data/info").then((r) => r.json());
    document.getElementById("dataCount").textContent = formatCount(info.count);
    document.getElementById("dataSize").textContent = formatBytes(info.bytes);
    const used = formatBytes(info.usedBytes);
    const total = formatBytes(info.totalBytes);
    document.getElementById("dataFs").textContent = info.littlefs + " · " + used + " / " + total;
  } catch (err) {
    setMsg("dataMsg", "Could not read storage", false);
  }
}

document.getElementById("deleteBtn").addEventListener("click", async () => {
  if (!confirm("Are you sure you want to delete all logged data?")) return;
  const res = await fetch("/api/data", { method: "DELETE" });
  setMsg("dataMsg", res.ok ? "Logged data deleted" : "Delete failed", res.ok);
  loadDataInfo();
});

refreshDashboard();
setInterval(refreshDashboard, 1000);
setInterval(() => {
  if (document.getElementById("data").classList.contains("visible")) loadDataInfo();
}, 5000);
