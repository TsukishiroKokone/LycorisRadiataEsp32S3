import { createSignal, onCleanup, onMount, type Component, Show } from 'solid-js';
import { t } from '../i18n';
import { PageHeader } from '../components/ui/PageHeader';
import { Section } from '../components/ui/Section';

type DeviceTab = 'servo' | 'stepper' | 'led' | 'dht11';

type DevicesGpios = {
  servo?: number[];
  stepper?: number[];
  dht11?: number[];
};

type ServoStatus = {
  channels?: Array<{
    channel: number;
    gpio: number;
    angle: number;
    enabled: boolean;
  }>;
};

type Dht11Status = {
  gpio?: number;
  default_gpio?: number;
  gpio_options?: number[];
};

type StepperStatus = {
  in1?: number;
  in2?: number;
  in3?: number;
  in4?: number;
  steps_per_rev?: number;
  rpm?: number;
  release_after_move?: boolean;
  gpio_options?: number[];
};

const FALLBACK_SERVO_GPIOS = [1, 8, 10, 11, 12, 13, 14, 17, 18, 21, 33, 34, 39, 41, 42, 47, 48];
const FALLBACK_STEPPER_GPIOS = [1, 8, 10, 11, 12, 13, 14, 17, 18, 21, 33, 34, 39, 40, 41, 42, 46, 47, 48];
const FALLBACK_DHT11_GPIOS = [1, 8, 10, 11, 12, 13, 14, 17, 18, 21, 33, 34, 39, 40, 41, 42, 47, 48];

const postJson = async (url: string, body?: Record<string, unknown>) => {
  const r = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined,
  });
  return r.ok;
};

const DeviceTabBar: Component<{ tab: DeviceTab; onSelect: (t: DeviceTab) => void }> = (props) => (
  <div class="flex gap-1 mb-4 border-b border-[var(--color-border-subtle)]">
    <button
      class={[
        'px-4 py-2 text-sm rounded-t transition',
        props.tab === 'servo'
          ? 'bg-[var(--color-accent)]/15 text-[var(--color-text-primary)]'
          : 'text-[var(--color-text-muted)] hover:text-[var(--color-text-primary)]',
      ].join(' ')}
      onClick={() => props.onSelect('servo')}
    >
      {t('devicesServo')}
    </button>
    <button
      class={[
        'px-4 py-2 text-sm rounded-t transition',
        props.tab === 'stepper'
          ? 'bg-[var(--color-accent)]/15 text-[var(--color-text-primary)]'
          : 'text-[var(--color-text-muted)] hover:text-[var(--color-text-primary)]',
      ].join(' ')}
      onClick={() => props.onSelect('stepper')}
    >
      {t('devicesStepper')}
    </button>
    <button
      class={[
        'px-4 py-2 text-sm rounded-t transition',
        props.tab === 'led'
          ? 'bg-[var(--color-accent)]/15 text-[var(--color-text-primary)]'
          : 'text-[var(--color-text-muted)] hover:text-[var(--color-text-primary)]',
      ].join(' ')}
      onClick={() => props.onSelect('led')}
    >
      {t('devicesLed')}
    </button>
    <button
      class={[
        'px-4 py-2 text-sm rounded-t transition',
        props.tab === 'dht11'
          ? 'bg-[var(--color-accent)]/15 text-[var(--color-text-primary)]'
          : 'text-[var(--color-text-muted)] hover:text-[var(--color-text-primary)]',
      ].join(' ')}
      onClick={() => props.onSelect('dht11')}
    >
      {t('devicesDht11')}
    </button>
  </div>
);

// ── Servo Panel ─────────────────────────────────────────────────────

const ServoPanel: Component = () => {
  const [angles, setAngles] = createSignal<number[]>([90, 90, 90, 90]);
  const [gpios, setGpios] = createSignal<number[]>([10, 11, 12, 13]);
  const [gpioOptions, setGpioOptions] = createSignal<number[]>(FALLBACK_SERVO_GPIOS);
  const [enabled, setEnabled] = createSignal<boolean[]>([false, false, false, false]);
  const [msg, setMsg] = createSignal('');

  onMount(() => {
    fetch('/api/devices/gpios')
      .then((r) => r.json())
      .then((j: DevicesGpios) => {
        if (Array.isArray(j.servo) && j.servo.length > 0) {
          setGpioOptions(j.servo);
        }
      })
      .catch(() => {});

    fetch('/api/servo/status')
      .then((r) => r.json())
      .then((j: ServoStatus) => {
        if (!Array.isArray(j.channels)) return;
        const nextGpios = [...gpios()];
        const nextAngles = [...angles()];
        const nextEnabled = [...enabled()];
        for (const ch of j.channels) {
          if (ch.channel < 0 || ch.channel >= nextGpios.length) continue;
          if (ch.gpio > 0) nextGpios[ch.channel] = ch.gpio;
          nextAngles[ch.channel] = ch.angle;
          nextEnabled[ch.channel] = ch.enabled;
        }
        setGpios(nextGpios);
        setAngles(nextAngles);
        setEnabled(nextEnabled);
      })
      .catch(() => {});
  });

  const post = async (url: string, body: Record<string, unknown>) => {
    try {
      const ok = await postJson(url, body);
      if (!ok) { setMsg('Error'); return false; }
      setMsg('OK');
      return true;
    } catch { setMsg('Network error'); return false; }
  };

  const handleGpio = async (ch: number, gpio: number) => {
    const g = [...gpios()];
    g[ch] = gpio;
    setGpios(g);
    await post('/api/servo/config', { channel: ch, gpio });
  };

  const handleAngle = async (ch: number, angle: number) => {
    const a = [...angles()];
    a[ch] = angle;
    setAngles(a);
    await post('/api/servo/angle', { channel: ch, angle });
  };

  const handleEnable = async (ch: number, en: boolean) => {
    const e = [...enabled()];
    e[ch] = en;
    setEnabled(e);
    await post('/api/servo/enable', { channel: ch, enabled: en });
  };

  return (
    <div>
      <div class="text-xs text-[var(--color-text-muted)] mb-4">{t('devicesServoDesc')}</div>
      {msg() && (
        <div class="text-xs text-[var(--color-accent)] mb-2">{msg()}</div>
      )}
      {[0, 1, 2, 3].map((ch) => (
        <div class="mb-6 p-4 rounded border border-[var(--color-border-subtle)]">
          <div class="flex items-center gap-3 mb-3">
            <span class="text-sm font-medium">{t('devicesChannel')} {ch + 1}</span>
            <select
              class="bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded px-2 py-1 text-sm"
              value={gpios()[ch]}
              onChange={(e) => void handleGpio(ch, parseInt(e.target.value))}
            >
              {gpioOptions().map((g) => (
                <option value={g}>GPIO {g}</option>
              ))}
            </select>
            <label class="flex items-center gap-1 text-sm">
              <input
                type="checkbox"
                checked={enabled()[ch]}
                onChange={(e) => void handleEnable(ch, e.target.checked)}
              />
              {t('devicesEnable')}
            </label>
          </div>
          <div class="flex items-center gap-3">
            <span class="text-xs w-8">{angles()[ch]}°</span>
            <input
              type="range"
              min="0"
              max="180"
              value={angles()[ch]}
              class="flex-1"
              onInput={(e) => {
                const a = [...angles()];
                a[ch] = parseInt(e.target.value);
                setAngles(a);
              }}
              onChange={(e) => void handleAngle(ch, parseInt(e.target.value))}
            />
          </div>
        </div>
      ))}
    </div>
  );
};

// ── LED Panel ───────────────────────────────────────────────────────

const StepperPanel: Component = () => {
  const [pins, setPins] = createSignal<number[]>([46, 17, 18, 21]);
  const [gpioOptions, setGpioOptions] = createSignal<number[]>(FALLBACK_STEPPER_GPIOS);
  const [direction, setDirection] = createSignal<'cw' | 'ccw'>('cw');
  const [mode, setMode] = createSignal<'steps' | 'degrees' | 'revolutions'>('steps');
  const [amount, setAmount] = createSignal(128);
  const [rpm, setRpm] = createSignal(8);
  const [releaseAfterMove, setReleaseAfterMove] = createSignal(true);
  const [busy, setBusy] = createSignal(false);
  const [msg, setMsg] = createSignal('');

  const loadStatus = () => {
    fetch('/api/devices/gpios')
      .then((r) => r.json())
      .then((j: DevicesGpios) => {
        if (Array.isArray(j.stepper) && j.stepper.length > 0) {
          setGpioOptions(j.stepper);
        }
      })
      .catch(() => {});

    fetch('/api/stepper/status')
      .then((r) => r.json())
      .then((j: StepperStatus) => {
        setPins([
          typeof j.in1 === 'number' ? j.in1 : 46,
          typeof j.in2 === 'number' ? j.in2 : 17,
          typeof j.in3 === 'number' ? j.in3 : 18,
          typeof j.in4 === 'number' ? j.in4 : 21,
        ]);
        if (typeof j.rpm === 'number') setRpm(j.rpm);
        if (typeof j.release_after_move === 'boolean') setReleaseAfterMove(j.release_after_move);
        if (Array.isArray(j.gpio_options) && j.gpio_options.length > 0) {
          setGpioOptions(j.gpio_options);
        }
      })
      .catch(() => {});
  };

  onMount(loadStatus);

  const updatePin = async (index: number, gpio: number) => {
    const next = [...pins()];
    next[index] = gpio;
    setPins(next);
    try {
      const ok = await postJson('/api/stepper/config', {
        in1: next[0],
        in2: next[1],
        in3: next[2],
        in4: next[3],
        rpm: rpm(),
        release_after_move: releaseAfterMove(),
      });
      setMsg(ok ? 'OK' : 'Error');
    } catch {
      setMsg('Network error');
    }
  };

  const move = async (preset?: Partial<Record<'steps' | 'degrees' | 'revolutions', number>>) => {
    const body: Record<string, unknown> = {
      in1: pins()[0],
      in2: pins()[1],
      in3: pins()[2],
      in4: pins()[3],
      direction: direction(),
      rpm: rpm(),
      release_after_move: releaseAfterMove(),
    };
    if (preset) {
      Object.assign(body, preset);
    } else {
      body[mode()] = amount();
    }

    setBusy(true);
    setMsg('');
    try {
      const ok = await postJson('/api/stepper/move', body);
      setMsg(ok ? 'OK' : 'Error');
    } catch {
      setMsg('Network error');
    } finally {
      setBusy(false);
    }
  };

  const release = async () => {
    setBusy(true);
    try {
      const ok = await postJson('/api/stepper/release');
      setMsg(ok ? 'OK' : 'Error');
    } catch {
      setMsg('Network error');
    } finally {
      setBusy(false);
    }
  };

  return (
    <div>
      <div class="text-xs text-[var(--color-text-muted)] mb-4">{t('devicesStepperDesc')}</div>
      {msg() && <div class="text-xs text-[var(--color-accent)] mb-2">{msg()}</div>}

      <div class="grid grid-cols-2 md:grid-cols-4 gap-3 mb-4">
        {['IN1', 'IN2', 'IN3', 'IN4'].map((label, index) => (
          <label class="flex flex-col gap-1 text-xs text-[var(--color-text-muted)]">
            <span>{label}</span>
            <select
              class="bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded px-2 py-1 text-sm text-[var(--color-text-primary)]"
              value={pins()[index]}
              onChange={(e) => void updatePin(index, parseInt(e.target.value))}
            >
              {gpioOptions().map((g) => (
                <option value={g} selected={pins()[index] === g}>GPIO {g}</option>
              ))}
            </select>
          </label>
        ))}
      </div>

      <div class="grid grid-cols-1 md:grid-cols-3 gap-3 mb-4">
        <label class="flex flex-col gap-1 text-xs text-[var(--color-text-muted)]">
          <span>{t('devicesStepperMode')}</span>
          <select
            class="bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded px-2 py-1 text-sm text-[var(--color-text-primary)]"
            value={mode()}
            onChange={(e) => setMode(e.target.value as 'steps' | 'degrees' | 'revolutions')}
          >
            <option value="steps">{t('devicesStepperSteps')}</option>
            <option value="degrees">{t('devicesStepperDegrees')}</option>
            <option value="revolutions">{t('devicesStepperRevolutions')}</option>
          </select>
        </label>
        <label class="flex flex-col gap-1 text-xs text-[var(--color-text-muted)]">
          <span>{t('devicesStepperAmount')}</span>
          <input
            type="number"
            class="bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded px-2 py-1 text-sm text-[var(--color-text-primary)]"
            value={amount()}
            min="0"
            onInput={(e) => setAmount(parseFloat(e.target.value || '0'))}
          />
        </label>
        <label class="flex flex-col gap-1 text-xs text-[var(--color-text-muted)]">
          <span>{t('devicesStepperRpm')}</span>
          <input
            type="number"
            class="bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded px-2 py-1 text-sm text-[var(--color-text-primary)]"
            value={rpm()}
            min="1"
            max="15"
            step="1"
            onInput={(e) => setRpm(parseFloat(e.target.value || '8'))}
          />
        </label>
      </div>

      <div class="flex flex-wrap items-center gap-3 mb-4">
        <label class="flex items-center gap-2 text-sm">
          <input type="radio" checked={direction() === 'cw'} onChange={() => setDirection('cw')} />
          {t('devicesStepperCw')}
        </label>
        <label class="flex items-center gap-2 text-sm">
          <input type="radio" checked={direction() === 'ccw'} onChange={() => setDirection('ccw')} />
          {t('devicesStepperCcw')}
        </label>
        <label class="flex items-center gap-2 text-sm">
          <input
            type="checkbox"
            checked={releaseAfterMove()}
            onChange={(e) => setReleaseAfterMove(e.target.checked)}
          />
          {t('devicesStepperReleaseAfter')}
        </label>
      </div>

      <div class="flex flex-wrap gap-2">
        <button
          class="px-4 py-2 bg-blue-600 text-white rounded hover:bg-blue-700 transition text-sm disabled:opacity-50"
          disabled={busy()}
          onClick={() => void move()}
        >
          {busy() ? t('devicesStepperRunning') : t('devicesStepperMove')}
        </button>
        <button
          class="px-3 py-2 bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded hover:bg-white/5 transition text-sm"
          disabled={busy()}
          onClick={() => void move({ steps: 32 })}
        >
          32 {t('devicesStepperSteps')}
        </button>
        <button
          class="px-3 py-2 bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded hover:bg-white/5 transition text-sm"
          disabled={busy()}
          onClick={() => void move({ steps: 128 })}
        >
          128 {t('devicesStepperSteps')}
        </button>
        <button
          class="px-3 py-2 bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded hover:bg-white/5 transition text-sm"
          disabled={busy()}
          onClick={() => void move({ degrees: 90 })}
        >
          90{t('devicesStepperDegreesSymbol')}
        </button>
        <button
          class="px-3 py-2 bg-red-600 text-white rounded hover:bg-red-700 transition text-sm disabled:opacity-50"
          disabled={busy()}
          onClick={() => void release()}
        >
          {t('devicesStepperRelease')}
        </button>
      </div>
    </div>
  );
};

const LEDPanel: Component = () => {
  const [color, setColor] = createSignal('#FF4444');
  const [brightness, setBrightness] = createSignal(80);
  const [ledCount, setLedCount] = createSignal(8);
  const [msg, setMsg] = createSignal('');

  const post = async (url: string, body?: Record<string, unknown>) => {
    try {
      const ok = await postJson(url, body);
      if (!ok) { setMsg('Error'); return; }
      setMsg('OK');
    } catch { setMsg('Network error'); }
  };

  const applySolid = () => post('/api/led/solid', { color: color(), brightness: brightness() });
  const turnOff = () => post('/api/led/off');
  const setCount = (n: number) => {
    setLedCount(n);
    post('/api/led/config', { led_count: n });
  };

  return (
    <div>
      <div class="text-xs text-[var(--color-text-muted)] mb-4">{t('devicesLedDesc')}</div>
      {msg() && (
        <div class="text-xs text-[var(--color-accent)] mb-2">{msg()}</div>
      )}

      {/* LED Count */}
      <div class="mb-4">
        <span class="text-sm mr-2">{t('devicesLedCount')}:</span>
        {[8, 12, 16, 24].map((n) => (
          <button
            class={[
              'px-3 py-1 text-sm rounded mr-1 transition',
              ledCount() === n
                ? 'bg-[var(--color-accent)] text-white'
                : 'bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] text-[var(--color-text-secondary)]',
            ].join(' ')}
            onClick={() => setCount(n)}
          >
            {n}
          </button>
        ))}
      </div>

      {/* Color */}
      <div class="mb-4 flex items-center gap-3">
        <span class="text-sm">{t('devicesLedColor')}:</span>
        <input
          type="color"
          value={color()}
          class="w-10 h-8 rounded border border-[var(--color-border-subtle)]"
          onChange={(e) => setColor(e.target.value)}
        />
        <span class="text-xs text-[var(--color-text-muted)]">{color()}</span>
      </div>

      {/* Brightness */}
      <div class="mb-4 flex items-center gap-3">
        <span class="text-xs w-12">{t('devicesLedBrightness')}: {brightness()}%</span>
        <input
          type="range"
          min="1"
          max="100"
          value={brightness()}
          class="flex-1"
          onInput={(e) => setBrightness(parseInt(e.target.value))}
          onChange={(e) => { setBrightness(parseInt(e.target.value)); applySolid(); }}
        />
      </div>

      {/* Buttons */}
      <div class="flex gap-2">
        <button
          class="px-4 py-2 bg-blue-600 text-white rounded hover:bg-blue-700 transition text-sm"
          onClick={applySolid}
        >
          {t('devicesLedApply')}
        </button>
        <button
          class="px-4 py-2 bg-red-600 text-white rounded hover:bg-red-700 transition text-sm"
          onClick={turnOff}
        >
          {t('devicesLedOff')}
        </button>
      </div>
    </div>
  );
};

// ── DHT11 Panel ─────────────────────────────────────────────────────

const DHT11Panel: Component = () => {
  const [temp, setTemp] = createSignal('--');
  const [humi, setHumi] = createSignal('--');
  const [gpio, setGpio] = createSignal(40);
  const [gpioOptions, setGpioOptions] = createSignal<number[]>(FALLBACK_DHT11_GPIOS);
  const [msg, setMsg] = createSignal('');

  const loadStatus = () => {
    fetch('/api/dht11/status')
      .then((r) => r.json())
      .then((j: Dht11Status) => {
        const nextGpio = typeof j.gpio === 'number'
          ? j.gpio
          : typeof j.default_gpio === 'number'
            ? j.default_gpio
            : 40;
        if (FALLBACK_DHT11_GPIOS.includes(nextGpio)) {
          setGpio(nextGpio);
        }
        if (Array.isArray(j.gpio_options) && j.gpio_options.length > 0) {
          setGpioOptions(j.gpio_options);
        }
      })
      .catch(() => {});
  };

  const handleGpio = async (nextGpio: number) => {
    setGpio(nextGpio);
    try {
      const ok = await postJson('/api/dht11/config', { gpio: nextGpio });
      setMsg(ok ? 'OK' : 'Error');
    } catch {
      setMsg('Network error');
    }
  };

  function readSensor() {
    fetch('/api/dht11/read')
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (typeof j.gpio === 'number') {
          setGpio(j.gpio);
        }
        if (j.ok) {
          setTemp(j.temperature.toFixed(1) + '°C');
          setHumi(j.humidity.toFixed(1) + '%');
        } else {
          setTemp('Err');
          setHumi('Err');
        }
      })
      .catch(function () {
        setTemp('Err');
        setHumi('Err');
      });
  }

  onMount(() => {
    loadStatus();
    readSensor();
    const timer = window.setInterval(readSensor, 2000);
    onCleanup(() => window.clearInterval(timer));
  });

  return (
    <div class="flex flex-col items-center gap-3">
      <div class="text-xs text-[var(--color-text-muted)]">{t('devicesDht11Desc')}</div>
      <div class="flex items-center gap-2">
        <span class="text-sm">GPIO</span>
        <select
          class="bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded px-2 py-1 text-sm"
          value={gpio()}
          onChange={(e) => void handleGpio(parseInt(e.target.value))}
        >
          {gpioOptions().map((g) => (
            <option value={g} selected={gpio() === g}>GPIO {g}</option>
          ))}
        </select>
        {msg() && <span class="text-xs text-[var(--color-accent)]">{msg()}</span>}
      </div>
      <div class="flex gap-6 items-center">
        <div class="text-center">
          <div class="text-4xl font-bold text-[var(--color-text-primary)]">{temp()}</div>
          <div class="text-xs text-[var(--color-text-muted)] mt-1">{t('devicesDht11Temp')}</div>
        </div>
        <div class="text-center">
          <div class="text-4xl font-bold text-[var(--color-text-primary)]">{humi()}</div>
          <div class="text-xs text-[var(--color-text-muted)] mt-1">{t('devicesDht11Hum')}</div>
        </div>
      </div>
      <button
        class="px-3 py-1 text-xs bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded hover:bg-white/5 transition"
        onClick={readSensor}
      >
        {t('devicesDht11Refresh')}
      </button>
    </div>
  );
};

// ── Devices Page ─────────────────────────────────────────────────────

export const DevicesPage: Component = () => {
  const [tab, setTab] = createSignal<DeviceTab>('servo');

  return (
    <div>
      <PageHeader
        title={t('navDevices') as string}
        description={t('devicesDescription') as string}
      />
      <Section title={t('navDevices') as string}>
        <DeviceTabBar tab={tab()} onSelect={setTab} />
        <Show when={tab() === 'servo'}>
          <ServoPanel />
        </Show>
        <Show when={tab() === 'stepper'}>
          <StepperPanel />
        </Show>
        <Show when={tab() === 'led'}>
          <LEDPanel />
        </Show>
        <Show when={tab() === 'dht11'}>
          <DHT11Panel />
        </Show>
      </Section>
    </div>
  );
};
