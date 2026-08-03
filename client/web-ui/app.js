/* MixerReturn control surface.

   The Phase 2 virtual audio device does not exist yet, so this runs against a simulated
   device: channel names come from a fixed descriptor and levels from a wandering signal
   generator. Everything below the `Device` seam is what a real backend will replace, and
   the SIMULATED chip in the menu bar stays lit until one does. A control surface that
   looks live when it isn't is worse than one that plainly says so.

   The one behaviour worth keeping honest even in simulation: the summed return really is
   the sum of the sending inputs, not its own independent wobble. That is the whole product,
   and a mock that faked it separately would hide the only thing worth looking at. */

const MIN_DB = -60;   // bottom of every meter and the foot of the fader travel ("-inf")
const MAX_DB = 10;    // top of the fader travel, matching the plugin's trim range
const METER_TOP_DB = 6;

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

const dbToGain = db => (db <= MIN_DB ? 0 : Math.pow(10, db / 20));
const gainToDb = g => (g <= 0.000001 ? -Infinity : 20 * Math.log10(g));

/** Meter geometry: linear in dB from MIN_DB to METER_TOP_DB, which is what the evenly
    spaced ticks in the design imply. */
const meterFraction = db => clamp((db - MIN_DB) / (METER_TOP_DB - MIN_DB), 0, 1);
const faderFraction = db => clamp((db - MIN_DB) / (MAX_DB - MIN_DB), 0, 1);
const fractionToFaderDb = f => MIN_DB + clamp(f, 0, 1) * (MAX_DB - MIN_DB);

function formatFader(db) {
  if (db <= MIN_DB) return '-inf';
  return `${db >= 0 ? '+' : ''}${db.toFixed(0)}dB`;
}

function formatMeter(db) {
  if (!isFinite(db) || db <= MIN_DB) return '-∞ dBFS';
  return `${db.toFixed(1)} dBFS`;
}

/* ------------------------------------------------------------------------- */
/* The device seam. A real driver replaces everything in here.                */
/* ------------------------------------------------------------------------- */

const Device = {
  simulated: true,
  name: 'Simulated 8x8 interface',

  describe() {
    // Pans are what make the summed return a stereo picture rather than the same
    // number drawn twice: -1 hard left, +1 hard right.
    const pans = [-0.7, -0.3, 0.3, 0.7, -0.9, -0.2, 0.4, 0.85];

    const mk = (kind, n, offset) =>
      Array.from({ length: n }, (_, i) => ({
        name: `${kind} ${i + 1}`,
        number: offset + i + 1,
        pan: pans[(offset + i) % pans.length],
      }));

    return {
      inputs:  [...mk('Mic', 4, 0), ...mk('Line', 4, 4)],
      outputs: [...mk('Mic', 4, 0), ...mk('Line', 4, 4)],
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
    this.faderDb = opts.faderDb !== undefined ? opts.faderDb : -12 + (index % 5) * 3;
    this.levelDb = MIN_DB;
    this.compact = !!opts.compact;

    this.el = document.createElement('div');
    this.el.className = 'strip';
    this.el.innerHTML = `
      <div class="name"></div>
      ${this.compact ? '' : '<div class="num"></div>'}
      <div class="fader" tabindex="0" role="slider"
           aria-valuemin="${MIN_DB}" aria-valuemax="${MAX_DB}">
        <div class="bar meter"><div class="mask"></div></div>
        <div class="bar track"></div>
        <div class="ticks"></div>
        <div class="cap"></div>
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

    this.attachDrag();
    this.renderFader();
  }

  attachDrag() {
    const set = clientY => {
      const r = this.faderEl.getBoundingClientRect();
      const f = 1 - (clientY - r.top) / r.height;
      this.faderDb = fractionToFaderDb(f);
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

  /** Post-fader level, which is what a meter beside a fader should be showing. */
  update(t) {
    const raw = this.sim.levelDb(t);
    this.levelDb = raw + Math.min(this.faderDb, MAX_DB);
    this.maskEl.style.height = `${(1 - meterFraction(this.levelDb)) * 100}%`;
    this.el.classList.toggle('clipping', this.levelDb > 0);
    return this.levelDb;
  }
}

/** The return strips show a level that is handed to them, not one they invent. */
class ReturnStrip extends Strip {
  update(_t, levelDb) {
    this.levelDb = levelDb;
    this.maskEl.style.height = `${(1 - meterFraction(this.levelDb)) * 100}%`;
    this.el.classList.toggle('clipping', this.levelDb > 0);
    return this.levelDb;
  }
}

/* ------------------------------------------------------------------------- */

// Deliberately uneven, because a tidy ramp of fader positions reads as a mock-up at a
// glance and a real surface never looks like that.
const DEFAULT_FADERS = [-13, -5, -26, -17, -4, -29, -11, -21];

const desc = Device.describe();
const inputStrips = desc.inputs.map((s, i) =>
  new Strip(s, i, { faderDb: DEFAULT_FADERS[i % DEFAULT_FADERS.length] }));
const outputStrips = desc.outputs.map((s, i) =>
  new Strip(s, i, { seedOffset: 11, faderDb: DEFAULT_FADERS[i % DEFAULT_FADERS.length] }));
const returnStrips = [
  new ReturnStrip({ name: 'L', number: 1 }, 0, { compact: true, faderDb: 0 }),
  new ReturnStrip({ name: 'R', number: 2 }, 1, { compact: true, faderDb: 0 }),
];

document.getElementById('strips-inputs').append(...inputStrips.map(s => s.el));
document.getElementById('strips-outputs').append(...outputStrips.map(s => s.el));
document.getElementById('return-strips').append(...returnStrips.map(s => s.el));

document.getElementById('device-name').textContent = Device.name;
document.getElementById('sim-chip').hidden = !Device.simulated;
document.getElementById('status-text').textContent = Device.simulated
  ? 'Simulated device — no driver attached. Levels and channel names are generated.'
  : `Connected to ${Device.name}`;

const readout = document.getElementById('return-readout');

for (const btn of document.querySelectorAll('.disclose')) {
  btn.addEventListener('click', () => {
    const open = btn.getAttribute('aria-expanded') === 'true';
    btn.setAttribute('aria-expanded', String(!open));
    btn.closest('.rack').classList.toggle('collapsed', open);
  });
}

/* ---- menus --------------------------------------------------------------- */

const SHEETS = {
  save: {
    title: 'Save',
    body: `<p>Saving a configuration writes the channel names, fader positions and the
           summed-return routing to a file the driver reads on start.</p>
           <p class="dim">Not wired up: there is no driver to write a configuration for yet.</p>`,
  },
  load: {
    title: 'Load',
    body: `<p>Loads a previously saved configuration and applies it to the attached device.</p>
           <p class="dim">Not wired up: there is no driver to apply a configuration to yet.</p>`,
  },
  setup: {
    title: 'Setup',
    body: `<p>Picks the physical device to wrap, and chooses which of its outputs carry the
           summed return.</p>
           <ul>
             <li>Wrapped device &mdash; <span class="dim">none; this is a simulated 8&times;8 interface</span></li>
             <li>Sample rate &mdash; <span class="dim">96 kHz, to match an SQ core</span></li>
             <li>Summed return destination &mdash; <span class="dim">unassigned</span></li>
           </ul>
           <p class="dim">Not wired up: enumerating real devices needs the driver.</p>`,
  },
  diagnostics: {
    title: 'Diagnostics',
    body: `<p>Log level, the rotating log file and a one-file diagnostics bundle, matching
           the vendored <code>diag</code> module the rest of the fleet uses.</p>
           <p class="dim">Not wired up yet.</p>`,
  },
  about: {
    title: 'About MixerReturn',
    body: `<p>Control surface for the MixerReturn virtual audio device: it passes a physical
           interface's I/O straight through and adds virtual ports that can be summed and
           sent to chosen outputs.</p>
           <p>The shipping half of the project today is the <strong>plugin</strong>, which
           does the same summing job inside Waves SuperRack Performer. This device is
           Phase&nbsp;2, and covers what a plugin structurally cannot &mdash; SuperRack
           SoundGrid, and routing between separate applications.</p>
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
  sheet.querySelector('.body').innerHTML = spec.body;

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

  // The return is the sum of the inputs, in linear gain, exactly as the bus does it —
  // panned, so L and R are two different pictures rather than one drawn twice.
  let sumL = 0;
  let sumR = 0;

  for (const strip of inputStrips) {
    const g = dbToGain(strip.update(t));
    const pan = strip.spec.pan || 0;
    sumL += g * Math.cos((pan + 1) * Math.PI / 4);
    sumR += g * Math.sin((pan + 1) * Math.PI / 4);
  }

  for (const strip of outputStrips) strip.update(t);

  const dbL = gainToDb(sumL);
  const dbR = gainToDb(sumR);
  returnStrips[0].update(t, dbL);
  returnStrips[1].update(t, dbR);

  const summedDb = Math.max(dbL, dbR);

  if (summedDb > peakHoldDb || now > peakHoldUntil) {
    peakHoldDb = summedDb;
    peakHoldUntil = now + 900;
  }

  readout.textContent = formatMeter(peakHoldDb);
  readout.classList.toggle('clipping', peakHoldDb > 0);

  requestAnimationFrame(frame);
}

requestAnimationFrame(frame);
