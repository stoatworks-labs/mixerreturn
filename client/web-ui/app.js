/* MixerReturn control surface.

   The Phase 2 virtual audio device does not exist yet, so this runs against a simulated
   device: channel names come from a fixed descriptor and levels from a wandering signal
   generator. Everything below the `Device` seam is what a real backend will replace, and
   the SIMULATED chip in the menu bar stays lit until one does. A control surface that
   looks live when it isn't is worse than one that plainly says so.

   ---------------------------------------------------------------------------
   The routing model, which is the whole point of the device
   ---------------------------------------------------------------------------

   The device wraps a physical interface and presents to the host:

     inputs   physical inputs, passed straight through
     outputs  physical outputs, passed straight through
              + one "Sum n" port per input channel, same format

   The Sum ports are OUTPUTS as far as the host is concerned. In SuperRack a rack takes a
   passed-through physical input, and its output goes either to a passed-through physical
   output — behaving as an ordinary insert, straight back to the desk — or to a Sum port,
   which feeds the summing buses.

   One Sum port per rack is what makes this work at all: SuperRack allows only one rack to
   patch to any given output I/O, so twenty-four racks cannot share one summing output.
   Giving each its own port and summing inside the device sidesteps that completely.

   Sum ports are assigned to up to eight stereo buses through a crosspoint matrix, with
   console-style assign switches on each strip for the common case. Each bus lands on a
   physical output pair, which is what returns to the desk as a group's External Input. */

const MIN_DB = -60;   // bottom of every meter and the foot of the fader travel ("-inf")
const MAX_DB = 10;    // top of the fader travel, matching the plugin's trim range
const METER_TOP_DB = 6;
const NUM_BUSES = 8;  // stereo

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

const dbToGain = db => (db <= MIN_DB ? 0 : Math.pow(10, db / 20));
const gainToDb = g => (g <= 0.000001 ? -Infinity : 20 * Math.log10(g));

/** Meter geometry: linear in dB, which is what evenly spaced ticks imply. */
const meterFraction = db => clamp((db - MIN_DB) / (METER_TOP_DB - MIN_DB), 0, 1);
const faderFraction = db => clamp((db - MIN_DB) / (MAX_DB - MIN_DB), 0, 1);
const fractionToFaderDb = f => MIN_DB + clamp(f, 0, 1) * (MAX_DB - MIN_DB);

const formatFader = db => (db <= MIN_DB ? '-inf' : `${db >= 0 ? '+' : ''}${db.toFixed(0)}dB`);
const formatMeter = db =>
  (!isFinite(db) || db <= MIN_DB ? '-∞ dBFS' : `${db.toFixed(1)} dBFS`);

/* ------------------------------------------------------------------------- */
/* The device seam. A real driver replaces everything in here.                */
/* ------------------------------------------------------------------------- */

const Device = {
  simulated: true,
  name: 'Simulated 8x8 interface',

  describe() {
    // Pans are a simulation nicety so the stereo bus meters are two different pictures.
    // In the real device this becomes a per-crosspoint value.
    const pans = [-0.7, -0.3, 0.3, 0.7, -0.9, -0.2, 0.4, 0.85];

    const mk = (kind, n, offset) =>
      Array.from({ length: n }, (_, i) => ({
        name: `${kind} ${i + 1}`,
        number: offset + i + 1,
        pan: pans[(offset + i) % pans.length],
      }));

    const inputs = [...mk('Mic', 4, 0), ...mk('Line', 4, 4)];

    return {
      inputs,
      outputs: [...mk('Mic', 4, 0), ...mk('Line', 4, 4)],
      // One Sum port per input, matching its format.
      sums: inputs.map((ch, i) => ({ name: `Sum ${i + 1}`, number: i + 1, pan: ch.pan })),
    };
  },
};

/** Smoothly wandering level in dB, so the meters read like programme rather than noise. */
class SignalSim {
  constructor(seed) {
    this.phase = seed * 1.7;
    this.rate = 0.35 + (seed % 5) * 0.11;
    this.centre = -22 + (seed % 7) * 2.4;
  }

  levelDb(t) {
    const slow = Math.sin(t * this.rate + this.phase);
    const fast = Math.sin(t * (this.rate * 5.3) + this.phase * 2.1);
    return this.centre + slow * 9 + fast * 3.5;
  }
}

/* ------------------------------------------------------------------------- */

class Strip {
  constructor(spec, index, opts = {}) {
    this.spec = spec;
    this.sim = new SignalSim(index + (opts.seedOffset || 0));
    this.faderDb = opts.faderDb !== undefined ? opts.faderDb : 0;
    this.levelDb = MIN_DB;
    this.compact = !!opts.compact;

    this.el = document.createElement('div');
    this.el.className = 'strip';
    this.el.innerHTML = `
      <div class="name"></div>
      ${this.compact ? '' : '<div class="num"></div>'}
      <div class="fadergroup">
        <div class="fader" tabindex="0" role="slider"
             aria-valuemin="${MIN_DB}" aria-valuemax="${MAX_DB}">
          <div class="bar meter"><div class="mask"></div></div>
          <div class="bar track"></div>
          <div class="ticks"></div>
          <div class="cap"></div>
        </div>
      </div>
      <div class="value"></div>`;

    this.el.querySelector('.name').textContent = spec.name;
    if (!this.compact) this.el.querySelector('.num').textContent = String(spec.number);

    const ticks = this.el.querySelector('.ticks');
    for (const pct of [0, 25, 50, 75, 100]) {
      const i = document.createElement('i');
      i.style.top = `${pct}%`;
      ticks.appendChild(i);
    }

    this.faderEl = this.el.querySelector('.fader');
    this.maskEl = this.el.querySelector('.mask');
    this.capEl = this.el.querySelector('.cap');
    this.valueEl = this.el.querySelector('.value');

    this.faderEl.setAttribute('aria-label', `${spec.name} fader`);
    this.attachDrag();
    this.renderFader();
  }

  attachDrag() {
    const set = clientY => {
      const r = this.faderEl.getBoundingClientRect();
      this.faderDb = fractionToFaderDb(1 - (clientY - r.top) / r.height);
      this.renderFader();
    };

    this.faderEl.addEventListener('pointerdown', e => {
      this.faderEl.setPointerCapture(e.pointerId);
      set(e.clientY);
      e.preventDefault();
    });

    this.faderEl.addEventListener('pointermove', e => {
      if (this.faderEl.hasPointerCapture(e.pointerId)) set(e.clientY);
    });

    this.faderEl.addEventListener('dblclick', () => {
      this.faderDb = 0;
      this.renderFader();
    });

    this.faderEl.addEventListener('keydown', e => {
      const step = e.shiftKey ? 0.5 : 2;
      if (e.key === 'ArrowUp')        this.faderDb = clamp(this.faderDb + step, MIN_DB, MAX_DB);
      else if (e.key === 'ArrowDown') this.faderDb = clamp(this.faderDb - step, MIN_DB, MAX_DB);
      else return;
      this.renderFader();
      e.preventDefault();
    });
  }

  renderFader() {
    this.capEl.style.bottom = `${faderFraction(this.faderDb) * 100}%`;
    this.valueEl.textContent = formatFader(this.faderDb);
    this.faderEl.setAttribute('aria-valuenow', this.faderDb.toFixed(1));
    this.faderEl.setAttribute('aria-valuetext', formatFader(this.faderDb));
  }

  showLevel(db) {
    this.levelDb = db;
    this.maskEl.style.height = `${(1 - meterFraction(db)) * 100}%`;
    this.el.classList.toggle('clipping', db > 0);
    return db;
  }

  /** Post-fader level, which is what a meter beside a fader should be showing. */
  update(t) {
    return this.showLevel(this.sim.levelDb(t) + Math.min(this.faderDb, MAX_DB));
  }
}

/** A Sum port: what SuperRack writes a rack's output into, plus its bus assignments.

    Its level is not invented — it is the corresponding input's post-fader signal, because
    that is literally what would be coming back out of a rack patched this way. */
class SumStrip extends Strip {
  constructor(spec, index, source) {
    super(spec, index, { faderDb: 0 });
    this.source = source;
    this.buses = new Set();

    const grid = document.createElement('div');
    grid.className = 'busassign';

    this.busButtons = [];
    for (let b = 0; b < NUM_BUSES; b++) {
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'busbtn';
      btn.textContent = String(b + 1);
      btn.setAttribute('aria-pressed', 'false');
      btn.setAttribute('aria-label', `${spec.name} to bus ${b + 1}`);
      btn.addEventListener('click', () => this.toggleBus(b));
      grid.appendChild(btn);
      this.busButtons.push(btn);
    }

    // Beside the fader rather than below it, as a numbered column down the strip — the
    // Midas arrangement, and it keeps the fader its full height.
    this.el.querySelector('.fadergroup').appendChild(grid);
  }

  toggleBus(b, force) {
    const on = force !== undefined ? force : !this.buses.has(b);
    if (on) this.buses.add(b); else this.buses.delete(b);
    this.busButtons[b].setAttribute('aria-pressed', String(on));
    onRoutingChanged();
  }

  update(t) {
    // The rack's own output, then this port's send trim.
    return this.showLevel(this.source.levelDb + Math.min(this.faderDb, MAX_DB));
  }
}

/** Bus return strips are handed a level rather than inventing one. */
class ReturnStrip extends Strip {
  update(_t, levelDb) {
    return this.showLevel(levelDb);
  }
}

/* ------------------------------------------------------------------------- */

const DEFAULT_FADERS = [-13, -5, -26, -17, -4, -29, -11, -21];

const desc = Device.describe();

const inputStrips = desc.inputs.map((s, i) =>
  new Strip(s, i, { faderDb: DEFAULT_FADERS[i % DEFAULT_FADERS.length] }));

const outputStrips = desc.outputs.map((s, i) =>
  new Strip(s, i, { seedOffset: 11, faderDb: DEFAULT_FADERS[i % DEFAULT_FADERS.length] }));

const sumStrips = desc.sums.map((s, i) => new SumStrip(s, i, inputStrips[i]));

const returnStrips = [
  new ReturnStrip({ name: 'L', number: 1 }, 0, { compact: true, faderDb: 0 }),
  new ReturnStrip({ name: 'R', number: 2 }, 1, { compact: true, faderDb: 0 }),
];

const buses = Array.from({ length: NUM_BUSES }, (_, i) => ({
  name: `Bus ${i + 1}`,
  destination: null,
  levelL: MIN_DB,
  levelR: MIN_DB,
}));

let selectedBus = 0;

document.getElementById('strips-inputs').append(...inputStrips.map(s => s.el));
document.getElementById('strips-outputs').append(...outputStrips.map(s => s.el));
document.getElementById('strips-sum').append(...sumStrips.map(s => s.el));
document.getElementById('return-strips').append(...returnStrips.map(s => s.el));

document.getElementById('device-name').textContent = Device.name;
document.getElementById('sim-chip').hidden = !Device.simulated;
document.getElementById('status-text').textContent = Device.simulated
  ? 'Simulated device — no driver attached. Levels and channel names are generated.'
  : `Connected to ${Device.name}`;

/* ---- bus picker ---------------------------------------------------------- */

const pickerEl = document.getElementById('buspicker');
const busPicks = buses.map((bus, i) => {
  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'buspick';
  btn.innerHTML = `<i class="lvl"></i><span>${i + 1}</span>`;
  btn.setAttribute('aria-pressed', String(i === selectedBus));
  btn.setAttribute('aria-label', `Show bus ${i + 1}`);
  btn.addEventListener('click', () => {
    selectedBus = i;
    for (const [j, b] of busPicks.entries()) b.setAttribute('aria-pressed', String(j === i));
    renderBusDestination();
  });
  pickerEl.appendChild(btn);
  return btn;
});

const destEl = document.getElementById('return-dest');
const readout = document.getElementById('return-readout');

function renderBusDestination() {
  const bus = buses[selectedBus];
  destEl.textContent = bus.destination
    ? `${bus.name} → ${bus.destination}`
    : `${bus.name} → unassigned`;
}

/** Single place that repaints everything showing routing state.

    The strip switches and the matrix are two views of one set of assignments, so whichever
    one is touched, both have to end up agreeing — an open matrix showing a crosspoint that
    was just switched off elsewhere is exactly the sort of thing that gets patched wrong. */
function onRoutingChanged() {
  for (const [i, btn] of busPicks.entries()) {
    const used = sumStrips.some(s => s.buses.has(i));
    btn.classList.toggle('unassigned', !used);
  }

  for (const cell of document.querySelectorAll('.matrixgrid .xpt')) {
    const strip = sumStrips[Number(cell.dataset.sum)];
    if (strip) cell.setAttribute('aria-pressed', String(strip.buses.has(Number(cell.dataset.bus))));
  }
}

// A sensible starting point rather than a blank matrix: everything into bus 1, which is
// the Dugan case the device exists for. Has to come after busPicks exists, because
// assigning a bus repaints the picker.
for (const s of sumStrips) s.toggleBus(0, true);

renderBusDestination();
onRoutingChanged();

/* ---- rack disclosure ----------------------------------------------------- */

for (const btn of document.querySelectorAll('.disclose')) {
  btn.addEventListener('click', () => {
    const open = btn.getAttribute('aria-expanded') === 'true';
    btn.setAttribute('aria-expanded', String(!open));
    btn.closest('.rack').classList.toggle('collapsed', open);
  });
}

/* ---- menus --------------------------------------------------------------- */

function buildMatrix() {
  const wrap = document.createElement('div');
  wrap.innerHTML = `<p>Which Sum ports feed which bus. The assign switches on each Sum
    strip are the same thing seen one channel at a time.</p>`;

  const grid = document.createElement('div');
  grid.className = 'matrixgrid';
  grid.style.gridTemplateColumns = `auto repeat(${NUM_BUSES}, 26px)`;

  grid.appendChild(document.createElement('div'));
  for (let b = 0; b < NUM_BUSES; b++) {
    const c = document.createElement('div');
    c.className = 'collabel';
    c.textContent = String(b + 1);
    grid.appendChild(c);
  }

  for (const [si, strip] of sumStrips.entries()) {
    const label = document.createElement('div');
    label.className = 'rowlabel';
    label.textContent = strip.spec.name;
    grid.appendChild(label);

    for (let b = 0; b < NUM_BUSES; b++) {
      const cell = document.createElement('button');
      cell.type = 'button';
      cell.className = 'xpt';
      cell.textContent = '●';
      cell.dataset.sum = String(si);
      cell.dataset.bus = String(b);
      cell.setAttribute('aria-pressed', String(strip.buses.has(b)));
      cell.setAttribute('aria-label', `${strip.spec.name} to bus ${b + 1}`);
      // No local repaint: toggleBus fans out through onRoutingChanged, so this cell and
      // the strip switch cannot drift apart.
      cell.addEventListener('click', () => strip.toggleBus(b));
      grid.appendChild(cell);
    }
  }

  wrap.appendChild(grid);

  const note = document.createElement('p');
  note.className = 'dim';
  note.textContent =
    'Each bus is stereo and lands on a physical output pair, chosen in Setup. That pair is '
    + 'what returns to the desk as a group’s External Input.';
  wrap.appendChild(note);

  return wrap;
}

const SHEETS = {
  save: {
    title: 'Save',
    body: () => `<p>Writes the wrapped device, the channel names, fader positions and the
           full Sum-to-bus crosspoint matrix to a file the driver reads on start.</p>
           <p class="dim">Not wired up: there is no driver to write a configuration for yet.</p>`,
  },
  load: {
    title: 'Load',
    body: () => `<p>Loads a saved configuration and applies it to the attached device.</p>
           <p class="dim">Not wired up: there is no driver to apply a configuration to yet.</p>`,
  },
  setup: {
    title: 'Setup',
    body: () => `<p>Picks the physical device to wrap, and where each bus lands.</p>
           <ul>
             <li>Wrapped device &mdash; <span class="dim">none; a simulated 8&times;8 interface</span></li>
             <li>Sample rate &mdash; <span class="dim">96 kHz, to match an SQ core</span></li>
             <li>Sum ports &mdash; <span class="dim">${desc.sums.length}, one per input</span></li>
             <li>Bus destinations &mdash; <span class="dim">unassigned</span></li>
           </ul>
           <p class="dim">Not wired up: enumerating real devices needs the driver.</p>`,
  },
  matrix: { title: 'Sum → bus matrix', node: buildMatrix },
  diagnostics: {
    title: 'Diagnostics',
    body: () => `<p>Log level, the rotating log file and a one-file diagnostics bundle,
           matching the vendored <code>diag</code> module the rest of the fleet uses.</p>
           <p class="dim">Not wired up yet.</p>`,
  },
  about: {
    title: 'About MixerReturn',
    body: () => `<p>Control surface for the MixerReturn virtual audio device. It wraps a
           physical interface, passes its I/O straight through, and adds one
           <strong>Sum</strong> port per input &mdash; outputs, as far as the host is
           concerned.</p>
           <p>In SuperRack a rack's output goes either to a passed-through physical output,
           behaving as an ordinary insert, or to a Sum port, which feeds one of eight stereo
           buses. Each bus lands on a physical output pair and returns to the desk as a
           group's External Input.</p>
           <p>One Sum port per rack is what makes it work: SuperRack allows only one rack
           per output I/O, so racks cannot share a summing output. Giving each its own port
           and summing in the device sidesteps that.</p>
           <p class="dim">Stoatworks Labs &middot; MIT &middot; AI-assisted, human-reviewed.</p>`,
  },
};

function openSheet(key) {
  const spec = SHEETS[key];
  if (!spec) return;

  const sheet = document.createElement('div');
  sheet.className = 'sheet';
  sheet.innerHTML = `<div class="card" role="dialog" aria-modal="true">
      <h3></h3><div class="body"></div>
      <button type="button" class="close">Close</button>
    </div>`;
  sheet.querySelector('h3').textContent = spec.title;

  const body = sheet.querySelector('.body');
  if (spec.node) body.appendChild(spec.node());
  else body.innerHTML = spec.body();

  const close = () => sheet.remove();
  sheet.querySelector('.close').addEventListener('click', close);
  sheet.addEventListener('click', e => { if (e.target === sheet) close(); });
  document.addEventListener('keydown', function esc(e) {
    if (e.key === 'Escape') { close(); document.removeEventListener('keydown', esc); }
  });

  document.body.appendChild(sheet);
  sheet.querySelector('.close').focus();
}

for (const btn of document.querySelectorAll('.menu button')) {
  btn.addEventListener('click', () => openSheet(btn.dataset.menu));
}

/* ---- the loop ------------------------------------------------------------ */

let peakHoldDb = MIN_DB;
let peakHoldUntil = 0;

function frame(now) {
  const t = now / 1000;

  for (const strip of inputStrips) strip.update(t);
  for (const strip of outputStrips) strip.update(t);
  for (const strip of sumStrips) strip.update(t);

  // Each bus is the sum of the Sum ports assigned to it, in linear gain, panned into the
  // stereo pair — the same arithmetic the device will do.
  for (const bus of buses) { bus.gL = 0; bus.gR = 0; }

  for (const strip of sumStrips) {
    if (strip.buses.size === 0) continue;
    const g = dbToGain(strip.levelDb);
    const pan = strip.spec.pan || 0;
    const gl = g * Math.cos((pan + 1) * Math.PI / 4);
    const gr = g * Math.sin((pan + 1) * Math.PI / 4);
    for (const b of strip.buses) { buses[b].gL += gl; buses[b].gR += gr; }
  }

  for (const bus of buses) {
    bus.levelL = gainToDb(bus.gL);
    bus.levelR = gainToDb(bus.gR);
  }

  for (const [i, btn] of busPicks.entries()) {
    const peak = Math.max(buses[i].levelL, buses[i].levelR);
    btn.querySelector('.lvl').style.height = `${meterFraction(peak) * 100}%`;
  }

  const shown = buses[selectedBus];
  returnStrips[0].update(t, shown.levelL);
  returnStrips[1].update(t, shown.levelR);

  const peak = Math.max(shown.levelL, shown.levelR);
  if (peak > peakHoldDb || now > peakHoldUntil) {
    peakHoldDb = peak;
    peakHoldUntil = now + 900;
  }

  readout.textContent = formatMeter(peakHoldDb);
  readout.classList.toggle('clipping', peakHoldDb > 0);

  requestAnimationFrame(frame);
}

requestAnimationFrame(frame);
