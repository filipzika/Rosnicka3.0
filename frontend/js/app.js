// ============================================================
//  SENSOR_CONFIG – upravte dle umisteni vasich senzoru
//
//  Pro kazdy senzor zadejte:
//    label : zobrazovane jmeno
//    x, y  : souradnice v SVG (viewBox 0 0 500 340)
//    color : barva markeru
//
//  Chcete-li pridat dalsi senzor, zkopirujte blok a zmente
//  klic (device_id musi odpovidat #define DEVICE_ID ve firmware).
// ============================================================
const SENSOR_CONFIG = {
  rosnicka_01: {
    label: 'Obyvak',
    x: 125,
    y: 100,
    color: '#16a34a',
  },
  // rosnicka_02: { label: 'Kuchyne', x: 375, y: 65, color: '#2563eb' },
  // rosnicka_03: { label: 'Loznice', x: 250, y: 272, color: '#9333ea' },
};

// ── Konfigurace MQTT ──────────────────────────────────────────
const MQTT_BROKER    = 'wss://broker.emqx.io:8084/mqtt';
const MQTT_TOPIC_SUB = 'rosnicka/+/sensors';
const STALE_MS       = 60_000;   // po 60 s bez dat = stale
const SVG_NS         = 'http://www.w3.org/2000/svg';

// ── Stav ─────────────────────────────────────────────────────
const sensorData = {};  // { device_id: { id, t, h, ts } }

// Ulozene polohy markeru (pretezeno pres SENSOR_CONFIG)
const savedPositions = (() => {
  try { return JSON.parse(localStorage.getItem('rosnicka-positions') || '{}'); }
  catch (_) { return {}; }
})();

// Ulozene nazvy zarizeni (senzory)
const savedLabels = (() => {
  try { return JSON.parse(localStorage.getItem('rosnicka-labels') || '{}'); }
  catch (_) { return {}; }
})();

// Ulozene nazvy mistnosti na pudorysu
const savedRoomLabels = (() => {
  try { return JSON.parse(localStorage.getItem('rosnicka-room-labels') || '{}'); }
  catch (_) { return {}; }
})();

// Edit mode
let editMode = false;
let drag = { active: false, target: null, offsetX: 0, offsetY: 0 };

// ── Helpers ──────────────────────────────────────────────────
function svgEl(tag, attrs) {
  const el = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, v);
  return el;
}

function svgText(attrs, text) {
  const el = svgEl('text', attrs);
  el.textContent = text;
  return el;
}

function tempColor(t) {
  if (t < 18) return '#2563eb';
  if (t < 22) return '#16a34a';
  if (t < 26) return '#d97706';
  return '#dc2626';
}

function freshness(ts) {
  const age = Date.now() - ts;
  if (age < 60_000)   return `pred ${Math.round(age / 1000)} s`;
  if (age < 3_600_000) return `pred ${Math.round(age / 60_000)} min`;
  return `pred ${Math.round(age / 3_600_000)} h`;
}

// ── Nazvy zarizeni ────────────────────────────────────────────
function getLabel(deviceId) {
  return savedLabels[deviceId] ?? SENSOR_CONFIG[deviceId]?.label ?? deviceId;
}

function saveLabel(deviceId, label) {
  savedLabels[deviceId] = label;
  localStorage.setItem('rosnicka-labels', JSON.stringify(savedLabels));
  const g = document.getElementById(`sensor-${deviceId}`);
  if (g) g.querySelector('.sensor-name').textContent = label;
}

function startRename(deviceId, nameEl) {
  const current = getLabel(deviceId);
  const input   = document.createElement('input');
  input.type      = 'text';
  input.className = 'name-input';
  input.value     = current;
  input.maxLength = 32;

  const commit = () => {
    const val = input.value.trim() || current;
    saveLabel(deviceId, val);
    const span = document.createElement('span');
    span.className   = 'card-name editable';
    span.textContent = val;
    span.addEventListener('click', () => startRename(deviceId, span));
    input.replaceWith(span);
  };

  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter')  { e.preventDefault(); input.blur(); }
    if (e.key === 'Escape') { input.value = current; input.blur(); }
  });
  input.addEventListener('blur', commit);
  nameEl.replaceWith(input);
  input.focus();
  input.select();
}

// ── Mistnosti na pudorysu ─────────────────────────────────────
function initRoomLabels() {
  document.querySelectorAll('[data-room-id]').forEach(el => {
    const id = el.getAttribute('data-room-id');
    if (savedRoomLabels[id]) el.textContent = savedRoomLabels[id];

    el.addEventListener('click', () => {
      if (!editMode) return;
      startRoomRename(el);
    });
  });
}

function startRoomRename(labelEl) {
  const roomId  = labelEl.getAttribute('data-room-id');
  const current = labelEl.textContent;
  const rect    = labelEl.getBoundingClientRect();
  const input   = document.getElementById('room-rename-input');

  input.value = current;
  input.style.left    = Math.round((rect.left + rect.right) / 2 - 60) + 'px';
  input.style.top     = Math.round(rect.top - 6) + 'px';
  input.style.display = 'block';
  input.focus();
  input.select();

  let done = false;
  const commit = () => {
    if (done) return;
    done = true;
    const val = input.value.trim() || current;
    labelEl.textContent = val;
    savedRoomLabels[roomId] = val;
    localStorage.setItem('rosnicka-room-labels', JSON.stringify(savedRoomLabels));
    input.style.display = 'none';
  };

  input.onblur    = commit;
  input.onkeydown = (e) => {
    if (e.key === 'Enter')  { e.preventDefault(); commit(); }
    if (e.key === 'Escape') { input.value = current; commit(); }
  };
}

// ── Vlastni obrazek pudorysu ──────────────────────────────────
function setFloorplanImage(url) {
  const svg = document.getElementById('floorplan');
  let img = svg.querySelector('.floorplan-bg');
  if (!img) {
    img = svgEl('image', {
      class: 'floorplan-bg',
      x: '0', y: '0', width: '500', height: '340',
      preserveAspectRatio: 'xMidYMid meet',
    });
    svg.insertBefore(img, svg.firstChild);
  }
  img.setAttribute('href', url);
  svg.classList.add('custom-bg');
  const clearBtn = document.getElementById('fp-clear-btn');
  if (clearBtn) clearBtn.style.display = '';
}

function clearFloorplanImage() {
  const svg = document.getElementById('floorplan');
  svg.querySelector('.floorplan-bg')?.remove();
  svg.classList.remove('custom-bg');
  localStorage.removeItem('rosnicka-floorplan-img');
  const clearBtn = document.getElementById('fp-clear-btn');
  if (clearBtn) clearBtn.style.display = 'none';
}

function initFloorplanTools() {
  document.getElementById('fp-upload-btn').addEventListener('click', () => {
    const picker = document.createElement('input');
    picker.type   = 'file';
    picker.accept = 'image/*';
    picker.onchange = (e) => {
      const file = e.target.files[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = (ev) => {
        const url = ev.target.result;
        localStorage.setItem('rosnicka-floorplan-img', url);
        setFloorplanImage(url);
      };
      reader.readAsDataURL(file);
    };
    picker.click();
  });

  document.getElementById('fp-clear-btn').addEventListener('click', clearFloorplanImage);

  // Obnov ulozeny obrazek
  const saved = localStorage.getItem('rosnicka-floorplan-img');
  if (saved) setFloorplanImage(saved);
}

// ── Drag helpers ─────────────────────────────────────────────
function getSvgPoint(svg, clientX, clientY) {
  const pt = svg.createSVGPoint();
  pt.x = clientX;
  pt.y = clientY;
  return pt.matrixTransform(svg.getScreenCTM().inverse());
}

function getMarkerPos(g) {
  const m = g.getAttribute('transform').match(/translate\(([^,\s]+)[,\s]+([^)]+)\)/);
  return m ? { x: parseFloat(m[1]), y: parseFloat(m[2]) } : { x: 0, y: 0 };
}

function toggleEditMode() {
  editMode = !editMode;
  const svg  = document.getElementById('floorplan');
  const btn  = document.getElementById('edit-btn');
  const hint = document.getElementById('edit-hint');

  if (editMode) {
    svg.classList.add('edit-mode');
    btn.textContent = 'Hotovo';
    btn.classList.add('active');
    hint.classList.add('visible');
  } else {
    svg.classList.remove('edit-mode');
    btn.textContent = 'Upravit';
    btn.classList.remove('active');
    hint.classList.remove('visible');
    if (drag.target) drag.target.classList.remove('dragging');
    drag = { active: false, target: null, offsetX: 0, offsetY: 0 };
  }

  // Zobraz/skryj nastroje pudorysu
  document.getElementById('floorplan-tools').classList.toggle('visible', editMode);

  // Prerender karet – prida/odebere tridu editable na card-name
  for (const [id, data] of Object.entries(sensorData)) updateCard(id, data);
}

function initDrag(svg) {
  const startDrag = (g, clientX, clientY) => {
    const pos = getMarkerPos(g);
    const pt  = getSvgPoint(svg, clientX, clientY);
    drag = { active: true, target: g, offsetX: pt.x - pos.x, offsetY: pt.y - pos.y };
    g.classList.add('dragging');
  };

  const moveDrag = (clientX, clientY) => {
    if (!drag.active) return;
    const pt = getSvgPoint(svg, clientX, clientY);
    const nx = (pt.x - drag.offsetX).toFixed(1);
    const ny = (pt.y - drag.offsetY).toFixed(1);
    drag.target.setAttribute('transform', `translate(${nx}, ${ny})`);
  };

  const endDrag = () => {
    if (!drag.active) return;
    const g        = drag.target;
    const deviceId = g.id.replace('sensor-', '');
    savedPositions[deviceId] = getMarkerPos(g);
    localStorage.setItem('rosnicka-positions', JSON.stringify(savedPositions));
    g.classList.remove('dragging');
    drag = { active: false, target: null, offsetX: 0, offsetY: 0 };
  };

  svg.addEventListener('mousedown', (e) => {
    if (!editMode) return;
    const g = e.target.closest('.sensor-marker');
    if (!g) return;
    e.preventDefault();
    startDrag(g, e.clientX, e.clientY);
  });

  svg.addEventListener('mousemove', (e) => moveDrag(e.clientX, e.clientY));
  svg.addEventListener('mouseup',    endDrag);
  svg.addEventListener('mouseleave', endDrag);

  svg.addEventListener('touchstart', (e) => {
    if (!editMode) return;
    const g = e.target.closest('.sensor-marker');
    if (!g) return;
    e.preventDefault();
    startDrag(g, e.touches[0].clientX, e.touches[0].clientY);
  }, { passive: false });

  svg.addEventListener('touchmove', (e) => {
    if (!drag.active) return;
    e.preventDefault();
    moveDrag(e.touches[0].clientX, e.touches[0].clientY);
  }, { passive: false });

  svg.addEventListener('touchend', endDrag);
}

// ── Floor plan – inicializace markerů ────────────────────────
function initFloorplan() {
  const svg = document.getElementById('floorplan');

  for (const [deviceId, cfg] of Object.entries(SENSOR_CONFIG)) {
    const pos = savedPositions[deviceId] ?? { x: cfg.x, y: cfg.y };
    const g = svgEl('g', {
      id: `sensor-${deviceId}`,
      transform: `translate(${pos.x}, ${pos.y})`,
    });
    g.classList.add('sensor-marker', 'sensor-offline');

    g.appendChild(svgEl('circle', {
      class: 'pulse-ring',
      r: '14',
      fill: cfg.color,
      'fill-opacity': '0.25',
    }));

    g.appendChild(svgEl('circle', {
      class: 'sensor-dot',
      r: '9',
      fill: '#9ca3af',
    }));

    g.appendChild(svgText({
      class: 'sensor-temp',
      y: '-22',
      'text-anchor': 'middle',
      'font-family': 'system-ui, sans-serif',
      fill: '#1a1d23',
    }, '--'));

    g.appendChild(svgText({
      class: 'sensor-hum',
      y: '-10',
      'text-anchor': 'middle',
      'font-family': 'system-ui, sans-serif',
      fill: '#6b7280',
    }, '-- %'));

    g.appendChild(svgText({
      class: 'sensor-name',
      y: '25',
      'text-anchor': 'middle',
      'font-family': 'system-ui, sans-serif',
      fill: '#374151',
    }, getLabel(deviceId)));

    svg.appendChild(g);
  }
}

// ── Aktualizace markeru na plane ─────────────────────────────
function updateMarker(deviceId, data) {
  const g = document.getElementById(`sensor-${deviceId}`);
  if (!g) return;

  const cfg   = SENSOR_CONFIG[deviceId];
  const color = cfg ? tempColor(data.t) : '#16a34a';
  const stale = Date.now() - data.ts > STALE_MS;

  g.className.baseVal = `sensor-marker ${stale ? 'sensor-stale' : 'sensor-online'}`;

  g.querySelector('.sensor-dot').setAttribute('fill', stale ? '#d97706' : color);
  g.querySelector('.sensor-temp').textContent = `${data.t.toFixed(1)}°C`;
  g.querySelector('.sensor-hum').textContent  = `${Math.round(data.h)} %`;
  g.querySelector('.sensor-name').textContent = getLabel(deviceId);
}

// ── Sensor karta ─────────────────────────────────────────────
function updateCard(deviceId, data) {
  const label = getLabel(deviceId);
  const stale = Date.now() - data.ts > STALE_MS;
  const cls   = stale ? 'card-stale' : 'card-online';

  const placeholder = document.querySelector('.no-data');
  if (placeholder) placeholder.remove();

  let card = document.getElementById(`card-${deviceId}`);
  if (!card) {
    card = document.createElement('div');
    card.id = `card-${deviceId}`;
    card.className = 'sensor-card';
    document.getElementById('sensors-list').appendChild(card);
  }

  // Neprerenderuj kdyz uzivatel prave pise nazev
  if (card.querySelector('.name-input')) {
    card.className = `sensor-card ${cls}`;
    return;
  }

  card.className = `sensor-card ${cls}`;
  card.innerHTML = `
    <div class="card-header">
      <span class="card-name${editMode ? ' editable' : ''}">${label}</span>
      <div class="card-actions">
        <button class="btn-card-edit" title="Prejmenovat zarizeni">&#9998;</button>
        <span class="card-id">${deviceId}</span>
      </div>
    </div>
    <div class="card-values">
      <div class="value-temp">${data.t.toFixed(1)}<span class="unit"> °C</span></div>
      <div class="value-hum">${Math.round(data.h)}<span class="unit"> %</span></div>
    </div>
    <div class="card-time">Aktualizovano ${freshness(data.ts)}</div>
  `;

  card.querySelector('.btn-card-edit').addEventListener('click', () => {
    startRename(deviceId, card.querySelector('.card-name'));
  });

  if (editMode) {
    card.querySelector('.card-name').addEventListener('click', function () {
      startRename(deviceId, this);
    });
  }
}

// ── Stavovy indikator ─────────────────────────────────────────
function setStatus(state, text) {
  const el = document.getElementById('connection-status');
  el.className = `status status-${state}`;
  el.querySelector('.status-text').textContent = text;
}

// ── Obnova ulozenych dat z localStorage ──────────────────────
function loadSaved() {
  for (const deviceId of Object.keys(SENSOR_CONFIG)) {
    try {
      const raw = localStorage.getItem(`rosnicka-${deviceId}`);
      if (raw) sensorData[deviceId] = JSON.parse(raw);
    } catch (_) { /* ignore */ }
  }
}

// ── MQTT pripojeni ────────────────────────────────────────────
function connectMQTT() {
  setStatus('connecting', 'Pripojuji...');

  const clientId = `rosnicka-web-${Math.random().toString(36).slice(2, 9)}`;
  const client   = mqtt.connect(MQTT_BROKER, {
    clientId,
    clean: true,
    reconnectPeriod: 6000,
    connectTimeout: 12000,
  });

  client.on('connect', () => {
    setStatus('online', 'Pripojeno');
    client.subscribe(MQTT_TOPIC_SUB, { qos: 0 });

    // Zobraz ulozena data hned po pripojeni
    for (const [id, data] of Object.entries(sensorData)) {
      updateMarker(id, data);
      updateCard(id, data);
    }
  });

  client.on('message', (topic, message) => {
    try {
      const deviceId = topic.split('/')[1];
      if (!deviceId) return;

      const data = JSON.parse(message.toString());
      // ts z ESP je Unix sekund (UTC) – prevedeme na ms; fallback = nyni
      data.ts = data.ts ? data.ts * 1000 : Date.now();

      sensorData[deviceId] = data;
      localStorage.setItem(`rosnicka-${deviceId}`, JSON.stringify(data));

      updateMarker(deviceId, data);
      updateCard(deviceId, data);

      document.getElementById('last-update').textContent =
        `Posledni upload: ${new Date(data.ts).toLocaleTimeString('cs-CZ')}`;
    } catch (e) {
      console.warn('Parse error:', e);
    }
  });

  client.on('offline',   () => setStatus('offline',    'Odpojeno'));
  client.on('reconnect', () => setStatus('connecting', 'Pripojuji...'));
  client.on('error',     (e) => console.error('MQTT error:', e));
}

// ── Periodicka aktualizace stavu karet (cerstve/stale) ───────
function startFreshnessTimer() {
  setInterval(() => {
    for (const [id, data] of Object.entries(sensorData)) {
      updateMarker(id, data);
      updateCard(id, data);
    }
  }, 15_000);
}

// ── Start ─────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
  loadSaved();
  initFloorplan();
  initRoomLabels();
  initFloorplanTools();

  const svg = document.getElementById('floorplan');
  initDrag(svg);
  document.getElementById('edit-btn').addEventListener('click', toggleEditMode);

  // Zobraz ulozena data pred tim nez se MQTT pripoji
  for (const [id, data] of Object.entries(sensorData)) {
    updateMarker(id, data);
    updateCard(id, data);
  }

  connectMQTT();
  startFreshnessTimer();
});
