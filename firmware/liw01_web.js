/*
 * LIW-01 Custom Web UI V0.5.2 RC1
 * Lightweight vanilla-JS frontend for ESPHome web_server v3.
 * No framework, no external assets, no charts, no animation.
 * Backend/API semantics intentionally follow the stock ESPHome v3 frontend:
 * EventSource /events, state updates and same-origin REST actions.
 */
(() => {
  "use strict";
  const BUILD = "LIW01_WEB_V052_RC3";
  const entities = new Map();
  let config = {title:"ZAMEL LIW-01", comment:"", uptime:0};
  let connected = false;
  let everConnected = false;
  let lastEventAt = 0;
  let advanced = false;
  let healthDetails = false;
  let renderQueued = false;

  // UI-only lifecycle. It never controls firmware actions.
  // The page stays visible during a reboot if it was already open, and will
  // reconnect to the same firmware-embedded /0.js + /0.css after boot.
  let lifecycle = "initializing"; // initializing | ready | reconnecting | restarting | factory_reset

  // ESPHome v3 sends compact and detail payloads using the SAME SSE event
  // name: "state". If a compact state arrives before its detail payload,
  // resolve it through the stock-compatible ?detail=all REST endpoint.
  const detailQueued = new Set();
  const detailResolved = new Set();
  const detailQueue = [];
  let detailWorkerRunning = false;

  const alias = {
    "Total consumption":"Celková spotřeba",
    "Since V0.2":"Od instalace V0.2",
    "Scaled total consumption":"Přepočtená spotřeba",
    "System health":"Stav systému",
    "Status":"Zařízení online",
    "Counter synchronized":"Čítač synchronizován",
    "FRAM V2 ready":"FRAM připravena",
    "FRAM V2 last persist OK":"Poslední zápis FRAM",
    "FRAM V2 storage state":"FRAM diagnostika",
    "WiFi RSSI":"Wi-Fi signál",
    "Uptime":"Doba běhu",
    "Local button last hold":"Poslední délka stisku",
    "Local button last band":"Poslední typ stisku",
    "Local button last action":"Poslední akce",
    "Local button factory confirmation pending":"Potvrzení factory resetu",
    "Web safety state":"Bezpečnost servisu",
    "Service operation state":"Servisní stav",
    "Impulses per liter":"Impulsů na litr",
    "Service target total liters":"Cílový stav počítadla [L]",
    "Arm RESET COUNTER":"Povolit RESET počítadla",
    "Arm LOCAL CONFIG RESET":"Povolit RESET lokální konfigurace",
    "APPLY Service SET TOTAL":"NASTAVIT cílový stav",
    "RESET COUNTER TO 0 - SERVICE":"RESET počítadla na 0",
    "RESET LOCAL CONFIG - KEEP TOTAL":"RESET lokální konfigurace",
    "Restart LIW-01":"Restart LIW-01"
  };

  const sectionDefs = [
    {id:"consumption", title:"Spotřeba", icon:"💧", tone:"water",
      names:["Total consumption","Since V0.2","Scaled total consumption"]},
    {id:"health", title:"Stav zařízení", icon:"✓", tone:"health",
      names:["System health","Status","Counter synchronized","FRAM V2 ready",
             "FRAM V2 last persist OK","WiFi RSSI","Uptime"]},
    {id:"button", title:"Lokální tlačítko", icon:"◉", tone:"button",
      names:["Local button last hold","Local button last band","Local button last action",
             "Local button factory confirmation pending"]},
    {id:"service", title:"Servis", icon:"🛡", tone:"service",
      names:["Web safety state","Service operation state","Impulses per liter",
             "Service target total liters","Arm RESET COUNTER","Arm LOCAL CONFIG RESET",
             "Restart LIW-01"]}
  ];
  const advancedService = [
    "APPLY Service SET TOTAL",
    "RESET COUNTER TO 0 - SERVICE",
    "RESET LOCAL CONFIG - KEEP TOTAL"
  ];

  const byName = name => {
    for (const e of entities.values()) if (e.name === name) return e;
    return null;
  };
  const entityId = d => d.name_id || d.id || d.unique_id || "";
  const escSeg = s => encodeURIComponent(String(s));
  const basePath = () => location.pathname.endsWith("/") ? location.pathname.slice(0,-1) : location.pathname;

  function actionUrl(e, action) {
    const id = entityId(e);

    // Match stock ESPHome v3 behavior exactly.
    // New format unique_id: domain/name or domain/device/name.
    // Action URL is built from domain + optional device + ENTITY NAME.
    if (id.includes("/")) {
      const devicePart = e.device ? `${escSeg(e.device)}/` : "";
      return `${basePath()}/${e.domain}/${devicePart}${escSeg(e.name)}/${action}`;
    }

    // Legacy format: domain-object_id.
    const domain = e.domain || id.split("-")[0];
    const objectId = id.split("-").slice(1).join("-");
    return `${basePath()}/${domain}/${escSeg(objectId)}/${action}`;
  }

  function detailUrl(id) {
    let path;
    if (id.includes("/")) {
      path = id.split("/").map(escSeg).join("/");
    } else {
      const parts = id.split("-");
      const domain = parts.shift();
      const objectId = parts.join("-");
      path = `${domain}/${escSeg(objectId)}`;
    }
    return `${basePath()}/${path}?detail=all`;
  }

  function queueDetail(id) {
    if (!id || detailResolved.has(id) || detailQueued.has(id)) return;
    detailQueued.add(id);
    detailQueue.push(id);
    runDetailWorker();
  }

  async function runDetailWorker() {
    if (detailWorkerRunning) return;
    detailWorkerRunning = true;
    try {
      // Deliberately one request at a time: gentle on ESP8266 RAM/socket budget.
      while (detailQueue.length) {
        const id = detailQueue.shift();
        try {
          const response = await fetch(detailUrl(id), {
            method: "GET",
            credentials: "same-origin",
            cache: "no-store"
          });
          if (response.ok) {
            const detail = await response.json();
            ingestDetail(detail);
            detailResolved.add(id);
          } else {
            console.warn("LIW-01 detail fetch HTTP", response.status, id);
          }
        } catch (err) {
          console.warn("LIW-01 detail fetch failed", id, err);
        } finally {
          detailQueued.delete(id);
        }

        // Small yield between REST reads so UI discovery cannot monopolize ESP.
        await new Promise(resolve => setTimeout(resolve, 60));
      }
    } finally {
      detailWorkerRunning = false;
    }
  }

  function post(e, action, confirmText, uiLifecycle = "") {
    if (!e) return;
    if (confirmText && !confirm(confirmText)) return;

    if (uiLifecycle) {
      lifecycle = uiLifecycle;
      requestRender();
    }

    fetch(actionUrl(e, action), {
      method:"POST",
      credentials:"same-origin",
      headers:{"Content-Type":"application/x-www-form-urlencoded"}
    }).catch(err => {
      console.error("LIW-01 action failed", err);
      if (uiLifecycle) {
        lifecycle = connected ? "ready" : "reconnecting";
        requestRender();
      }
    });
  }

  function requestRender() {
    if (renderQueued) return;
    renderQueued = true;
    requestAnimationFrame(() => { renderQueued = false; render(); });
  }

  function ingestDetail(data) {
    const id = entityId(data);
    if (!id) return;

    const domain = data.domain || (id.includes("/") ? id.split("/")[0] : id.split("-")[0]);
    entities.set(id, {
      ...(entities.get(id) || {}),
      ...data,
      domain,
      unique_id: id
    });
    detailResolved.add(id);
    detailQueued.delete(id);
    requestRender();
  }

  function ingestState(data) {
    const id = entityId(data);
    if (!id) return;

    // Stock ESPHome v3 uses "state" for detail_all too.
    // A detail payload is recognizable by name + domain.
    if (data.name && data.domain) {
      ingestDetail(data);
      return;
    }

    const old = entities.get(id);
    if (old) {
      const cleaned = {...data};
      delete cleaned.id;
      delete cleaned.name_id;
      delete cleaned.domain;
      delete cleaned.unique_id;
      entities.set(id, {...old, ...cleaned});
      requestRender();
      return;
    }

    // Compact state for an entity not described yet: resolve gently via REST.
    queueDetail(id);
  }

  function formatUptime(sec) {
    sec = Number(sec) || 0;
    const d = Math.floor(sec/86400); sec %= 86400;
    const h = Math.floor(sec/3600); sec %= 3600;
    const m = Math.floor(sec/60);
    return [d?`${d} d`:"",h?`${h} h`:"",`${m} min`].filter(Boolean).join(" ");
  }

  function isOn(e) {
    const s = String(e?.state ?? e?.value ?? "").toUpperCase();
    return s === "ON" || s === "TRUE" || s === "1";
  }

  function valueClass(name, e) {
    const raw = String(e?.state ?? e?.value ?? "");
    const s = raw.toUpperCase();

    if (name === "Local button factory confirmation pending" ||
        name === "Arm RESET COUNTER" || name === "Arm LOCAL CONFIG RESET") {
      return isOn(e) ? "warn" : "ok";
    }

    if (name === "Web safety state") {
      if (s.includes("SAFE") || s.includes("DISARMED")) return "ok";
      if ((s.includes("ARMED") && !s.includes("DISARMED")) || s.includes("PENDING")) return "warn";
    }

    if (name === "FRAM V2 storage state") {
      const clean = /FAULTS=0/i.test(raw) && /UNSAVED=0/i.test(raw);
      const valid = /VALID/i.test(raw);
      return clean && valid ? "ok" : "bad";
    }

    if (name === "WiFi RSSI") {
      const dbm = parseFloat(raw);
      if (!Number.isNaN(dbm)) {
        if (dbm >= -67) return "ok";
        if (dbm >= -85) return "warn";
        return "bad";
      }
    }

    if (s.includes("FAULT") || s.includes("ERROR") || s.includes("FAIL")) return "bad";
    if (s.includes("WARNING") || (s.includes("ARMED") && !s.includes("DISARMED")) || s.includes("PENDING")) return "warn";
    if (s === "OK" || s === "ON" || s.includes("VALID") || s.includes("SYNC")) return "ok";
    if (s === "OFF" && (name === "Status" || name.includes("ready") || name.includes("persist"))) return "bad";
    return "";
  }

  function glyph(name, e) {
    if (name.includes("Total") || name.includes("consumption") || name.includes("Since")) return "💧";
    if (name.includes("WiFi")) return "◔";
    if (name === "Uptime") return "◷";
    if (name.includes("FRAM")) return "▣";
    if (name.includes("Counter")) return "↻";
    if (name.includes("button") || name.includes("Local button")) return "◉";
    if (name.includes("Arm")) return "⚿";
    if (name.includes("RESET")) return "!";
    if (name.includes("Restart")) return "↻";
    if (name.includes("health") || name === "Status") return "✓";
    return "•";
  }

  function mk(tag, cls, text) {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    if (text !== undefined) el.textContent = text;
    return el;
  }

  function valueText(e, name = "") {
    if (!e) return "čekám…";

    if (e.domain === "binary_sensor") {
      if (name === "Local button factory confirmation pending") {
        return isOn(e) ? "ČEKÁ NA POTVRZENÍ" : "NE";
      }
      if (name === "Status" || name === "Counter synchronized" ||
          name === "FRAM V2 ready" || name === "FRAM V2 last persist OK") {
        return isOn(e) ? "OK" : "CHYBA";
      }
      return isOn(e) ? "ON" : "OFF";
    }

    const raw = String(e.state ?? e.value ?? "—");
    if (name === "Uptime") {
      const seconds = parseFloat(raw);
      if (!Number.isNaN(seconds)) return formatUptime(seconds);
    }
    return raw;
  }

  function makeSwitch(e) {
    const wrap = mk("label","switch");
    const input = document.createElement("input");
    input.type = "checkbox"; input.checked = isOn(e);
    input.addEventListener("change", () => post(e, input.checked ? "turn_on" : "turn_off"));
    const span = mk("span","slider");
    wrap.append(input, span);
    return wrap;
  }

  function makeNumber(e) {
    const wrap = mk("div","number-control");
    const input = document.createElement("input");
    input.type = "number";
    if (e.min_value !== undefined) input.min = e.min_value;
    if (e.max_value !== undefined) input.max = e.max_value;
    input.step = e.step ?? 1;
    input.value = e.value ?? parseFloat(e.state) ?? "";
    const btn = mk("button","mini primary","ULOŽIT");
    btn.addEventListener("click", () => {
      const val = input.value;
      post(e, `set?value=${encodeURIComponent(val)}`);
    });
    wrap.append(input, btn);
    if (e.uom) wrap.append(mk("span","uom",e.uom));
    return wrap;
  }

  function makeButton(name, e) {
    const b = mk("button","action-btn", name === "Restart LIW-01" ? "RESTART" : "PROVÉST");
    let confirmText = "";
    if (name === "Restart LIW-01") {
      confirmText = "Opravdu restartovat LIW-01?";
      b.classList.add("secondary");
    }
    if (name === "APPLY Service SET TOTAL") {
      confirmText = "Opravdu nastavit nový absolutní stav počítadla?";
      b.classList.add("warning");
    }
    if (name === "RESET COUNTER TO 0 - SERVICE") {
      confirmText = "POZOR: Opravdu resetovat počítadlo na 0? Musí být předem ARM RESET COUNTER.";
      b.classList.add("danger");
      const arm = byName("Arm RESET COUNTER");
      b.disabled = !isOn(arm);
    }
    if (name === "RESET LOCAL CONFIG - KEEP TOTAL") {
      confirmText = "POZOR: Opravdu resetovat lokální konfiguraci ESPHome? TOTAL ve FRAM zůstane zachován.";
      b.classList.add("danger");
      const arm = byName("Arm LOCAL CONFIG RESET");
      b.disabled = !isOn(arm);
    }
    let uiLifecycle = "";
    if (name === "Restart LIW-01") uiLifecycle = "restarting";
    if (name === "RESET LOCAL CONFIG - KEEP TOTAL") uiLifecycle = "factory_reset";

    b.addEventListener("click", () => post(e, "press", confirmText, uiLifecycle));
    return b;
  }

  function row(name) {
    const e = byName(name);
    if (!e) return null;
    const r = mk("div","entity-row");
    r.dataset.name = name;
    const ico = mk("div","entity-icon",glyph(name,e));
    const label = mk("div","entity-label",alias[name] || name);
    const val = mk("div","entity-value "+valueClass(name,e));

    if (e.domain === "switch") val.append(makeSwitch(e));
    else if (e.domain === "number") val.append(makeNumber(e));
    else if (e.domain === "button") val.append(makeButton(name,e));
    else {
      val.textContent = valueText(e, name);
      if (name === "FRAM V2 storage state") val.classList.add("small","mono");
    }
    r.append(ico,label,val);
    return r;
  }

  function section(def) {
    const card = mk("section",`card tone-${def.tone}`);
    const head = mk("div","card-head");
    head.append(mk("span","card-icon",def.icon), mk("h2","",def.title));
    card.append(head);
    const body = mk("div","card-body");
    for (const n of def.names) {
      // With 1 pulse/liter the scaled total is exactly the same as Total
      // consumption, so don't waste visual space. It appears automatically
      // when the installation uses a different scale.
      if (def.id === "consumption" && n === "Scaled total consumption") {
        const scale = byName("Impulses per liter");
        const scaleValue = parseFloat(scale?.state ?? scale?.value ?? "1");
        if (!Number.isNaN(scaleValue) && Math.abs(scaleValue - 1.0) < 0.0005) continue;
      }

      const rr = row(n); if (rr) body.append(rr);
    }

    if (def.id === "health") {
      const sep = mk("div","advanced-sep health-sep");
      const toggle = mk("button","advanced-toggle health-toggle",
        healthDetails ? "Skrýt podrobnosti diagnostiky" : "Podrobnosti diagnostiky");
      toggle.addEventListener("click", () => {
        healthDetails = !healthDetails;
        requestRender();
      });
      sep.append(toggle);
      body.append(sep);

      if (healthDetails) {
        const details = mk("div","advanced-box health-box");
        const rr = row("FRAM V2 storage state");
        if (rr) details.append(rr);
        body.append(details);
      }
    }

    if (def.id === "service") {
      const sep = mk("div","advanced-sep");
      const toggle = mk("button","advanced-toggle", advanced ? "Skrýt rozšířený servis" : "Rozšířený servis");
      toggle.addEventListener("click", () => { advanced = !advanced; requestRender(); });
      sep.append(toggle);
      body.append(sep);
      if (advanced) {
        const adv = mk("div","advanced-box");
        for (const n of advancedService) {
          const rr = row(n); if (rr) adv.append(rr);
        }
        body.append(adv);
      }
    }
    card.append(body);
    return card;
  }

  function render() {
    const app = document.getElementById("liw-app");
    if (!app) return;

    const total = byName("Total consumption");
    const health = byName("System health");
    const uptimeEntity = byName("Uptime");
    const dataReady = !!(total && health);

    if (connected && dataReady && lifecycle !== "factory_reset" && lifecycle !== "restarting") {
      lifecycle = "ready";
    } else if (connected && !dataReady && lifecycle === "reconnecting") {
      lifecycle = "initializing";
    }

    const hState = String(health?.state || "");
    const healthOk = hState === "OK";
    const healthWarn = hState.includes("WARNING");

    let pillText = hState || "INITIALIZACE";
    let pillClass = healthOk ? "ok" : healthWarn ? "warn" : "bad";
    let heroInfo = "";
    let connectionText = connected ? "● ONLINE" : "● OFFLINE";

    if (lifecycle === "initializing") {
      pillText = "INITIALIZACE";
      pillClass = "info";
      heroInfo = "Načítám stav čítače a FRAM. Externí TOTAL zůstává v chráněné FRAM.";
    } else if (lifecycle === "reconnecting") {
      pillText = "OBNOVA SPOJENÍ";
      pillClass = "warn";
      heroInfo = "Zařízení se znovu připojuje. Barevný web je součást firmware a po naběhnutí se obnoví.";
    } else if (lifecycle === "restarting") {
      pillText = "RESTART";
      pillClass = "warn";
      heroInfo = "LIW-01 se restartuje. Čekám na opětovné připojení.";
    } else if (lifecycle === "factory_reset") {
      pillText = "FACTORY RESET";
      pillClass = "warn";
      heroInfo = "Resetuji pouze lokální ESPHome nastavení. Web UI zůstává ve firmware a TOTAL v externí FRAM se nemaže.";
    }

    app.replaceChildren();

    const header = mk("header","hero");
    const brand = mk("div","brand");
    const drop = mk("div","drop","💧");
    const titleBox = mk("div","");
    titleBox.append(
      mk("div","eyebrow","OPEN LIW-01"),
      mk("h1","",config.title || "ZAMEL LIW-01"),
      mk("div","subtitle","Lokální vodoměr • FRAM persistence • Home Assistant")
    );
    brand.append(drop,titleBox);

    const heroValue = mk("div","hero-value");
    heroValue.append(
      mk("div","hero-label","CELKOVÁ SPOTŘEBA"),
      mk("div","hero-number", total ? valueText(total, "Total consumption") : "NAČÍTÁM…")
    );

    const conn = mk("div","hero-status");
    const pill = mk("div",`health-pill ${pillClass}`, pillText);
    const live = mk("div",`live ${connected?"online":"offline"}`, connectionText);
    const up = mk("div","uptime-small",
      uptimeEntity ? `běh ${valueText(uptimeEntity, "Uptime")}` :
      config.uptime ? `běh ${formatUptime(config.uptime)}` : "");
    conn.append(pill,live,up);
    header.append(brand,heroValue,conn);
    app.append(header);

    if (heroInfo) {
      const notice = mk("div",`lifecycle-notice lifecycle-${lifecycle}`);
      notice.append(
        mk("span","notice-icon", lifecycle === "factory_reset" ? "↻" : lifecycle === "initializing" ? "…" : "⟳"),
        mk("span","",heroInfo)
      );
      app.append(notice);
    }

    const grid = mk("main","dashboard");
    for (const def of sectionDefs) grid.append(section(def));
    app.append(grid);

    const footer = mk("footer","footer");
    footer.append(
      mk("span","",`UI ${BUILD}`),
      mk("span","",connected ? "SSE připojeno" : "čekám na zařízení…")
    );
    app.append(footer);
  }


  function setup() {
    document.title = "ZAMEL LIW-01";
    document.documentElement.lang = "cs";
    let old = document.querySelector("esp-app");
    if (old) old.remove();
    let app = document.getElementById("liw-app");
    if (!app) {
      app = document.createElement("div");
      app.id = "liw-app";
      document.body.appendChild(app);
    }
    render();

    const src = new EventSource(basePath()+"/events");
    window.liw01Source = src;

    src.addEventListener("ping", ev => {
      lastEventAt = Date.now();
      connected = true;
      everConnected = true;
      if (ev.data) {
        try {
          const d = JSON.parse(ev.data);
          config = {...config, ...d};
          if (d.title) document.title = d.title;
        } catch {}
      }
      requestRender();
    });
    // ESPHome v3 emits both initial/detail and compact updates as "state".
    src.addEventListener("state", ev => {
      lastEventAt = Date.now();
      connected = true;
      everConnected = true;
      try {
        ingestState(JSON.parse(ev.data));
      } catch (err) {
        console.warn("LIW-01 state parse failed", err);
      }
    });
    src.addEventListener("error", () => {
      connected = false;
      if (lifecycle !== "factory_reset" && lifecycle !== "restarting") {
        lifecycle = everConnected ? "reconnecting" : "initializing";
      }
      requestRender();
    });

    setInterval(() => {
      if (connected && lastEventAt && Date.now()-lastEventAt > 16000) {
        connected = false;
        if (lifecycle !== "factory_reset" && lifecycle !== "restarting") {
          lifecycle = everConnected ? "reconnecting" : "initializing";
        }
        requestRender();
      }
    }, 5000);
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", setup, {once:true});
  else setup();
})();
