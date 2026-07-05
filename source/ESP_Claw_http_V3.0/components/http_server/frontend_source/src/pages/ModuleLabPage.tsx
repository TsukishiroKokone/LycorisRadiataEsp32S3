import { createEffect, createMemo, createSignal, For, onCleanup, onMount, Show, type Component } from 'solid-js';
import { CheckCircle2, Eraser, Play, Save, Square, RotateCcw } from 'lucide-solid';

import {
  deleteModuleConfig,
  fetchModuleConfig,
  fetchPins,
  moduleAction,
  saveModuleConfig,
  type ModuleActionResult,
  type PinCapability,
} from '../api/client';
import { Button } from '../components/ui/Button';
import { PageHeader } from '../components/ui/PageHeader';
import { Section } from '../components/ui/Section';
import { currentLocale } from '../i18n';
import { pushToast } from '../state/toast';
import { useDirtyTracker } from '../state/dirty';
import {
  MODULES,
  modulesForCategory,
  type LocalizedText,
  type ModuleCategory,
  type ModuleDefinition,
} from './moduleRegistry';

type PinField = {
  key: string;
  label: LocalizedText;
  capability: PinCapability;
  optional?: boolean;
};

type Props = {
  category: ModuleCategory;
};

type Reading = {
  name?: string;
  pin?: number;
  level?: number;
  raw?: number;
  percent?: number;
  voltage_mv?: number;
  celsius?: number;
  distance_cm?: number;
  duration_us?: number;
  red?: number;
  green?: number;
  blue?: number;
  count?: number;
  delta?: number;
  clk?: number;
  dt?: number;
  button?: number;
  button_pressed?: boolean;
  button_events?: number;
  button_pin?: number;
  sda?: number;
  scl?: number;
  address?: number;
  who_am_i?: number;
  swapped?: boolean;
  accel_x_g?: number;
  accel_y_g?: number;
  accel_z_g?: number;
  gyro_x_dps?: number;
  gyro_y_dps?: number;
  gyro_z_dps?: number;
  roll_deg?: number;
  pitch_deg?: number;
  melody?: string;
  title?: string;
  state?: string;
  error?: string;
  message?: string;
};

type DisplayReading = {
  label: string;
  value: string;
  metricLabel?: string;
  metric?: string;
  detail?: string;
  tone?: 'ok' | 'warn' | 'error' | 'neutral';
};

const categoryTitle: Record<ModuleCategory, LocalizedText> = {
  sensors: { zh: '传感器', en: 'Sensors' },
  actuators: { zh: '执行器', en: 'Actuators' },
  media: { zh: '音视频', en: 'Audio & Video' },
};

const categoryDesc: Record<ModuleCategory, LocalizedText> = {
  sensors: {
    zh: '选择一个模块，先临时选择引脚测试，确认接线后再手动保存配置。',
    en: 'Select a module, choose temporary pins for testing, then save only after wiring is confirmed.',
  },
  actuators: {
    zh: '舵机和电机统一放在执行器列表，后续可以继续扩展更多驱动模块。',
    en: 'Servo and motor modules live here, with room for future actuator drivers.',
  },
  media: {
    zh: '音频输入、音频输出、视频输入和复合设备分开验收，USB 三合一设备按功能拆分测试。',
    en: 'Audio input, audio output, video input, and composite devices are tested separately.',
  },
};

const emptyPins = {};

function lang() {
  return currentLocale() === 'zh-cn' ? 'zh' : 'en';
}

function pick(text: LocalizedText) {
  return text[lang()];
}

function pinFields(module: ModuleDefinition): PinField[] {
  switch (module.id) {
    case 'sensor_gpio_traffic_led':
      return [
        { key: 'red', label: { zh: '红灯', en: 'Red' }, capability: 'gpio' },
        { key: 'blue', label: { zh: '蓝灯', en: 'Blue' }, capability: 'gpio' },
        { key: 'green', label: { zh: '绿灯', en: 'Green' }, capability: 'gpio' },
      ];
    case 'sensor_joystick':
      return [
        { key: 'x', label: { zh: 'X 轴 ADC', en: 'X ADC' }, capability: 'adc' },
        { key: 'y', label: { zh: 'Y 轴 ADC', en: 'Y ADC' }, capability: 'adc' },
        { key: 'button', label: { zh: '按键 GPIO', en: 'Button GPIO' }, capability: 'gpio' },
      ];
    case 'sensor_soil_moisture':
    case 'sensor_sound_level':
      return [
        { key: 'ao', label: { zh: '模拟 AO', en: 'Analog AO' }, capability: 'adc' },
        { key: 'do', label: { zh: '数字 DO', en: 'Digital DO' }, capability: 'gpio' },
      ];
    case 'sensor_rotary_encoder':
      return [
        { key: 'clk', label: { zh: 'CLK', en: 'CLK' }, capability: 'gpio' },
        { key: 'dt', label: { zh: 'DT', en: 'DT' }, capability: 'gpio' },
        { key: 'button', label: { zh: '按键', en: 'Button' }, capability: 'gpio', optional: true },
      ];
    case 'sensor_hcsr04_ultrasonic':
      return [
        { key: 'trig', label: { zh: 'Trig', en: 'Trig' }, capability: 'gpio' },
        { key: 'echo', label: { zh: 'Echo', en: 'Echo' }, capability: 'gpio' },
      ];
    case 'sensor_i2c_mpu6050':
      return [
        { key: 'sda', label: { zh: 'SDA', en: 'SDA' }, capability: 'i2c' },
        { key: 'scl', label: { zh: 'SCL', en: 'SCL' }, capability: 'i2c' },
      ];
    case 'actuator_multi_servo':
      return [0, 1, 2, 3].map((index) => ({
        key: `servo_${index + 1}`,
        label: { zh: `舵机 ${index + 1}`, en: `Servo ${index + 1}` },
        capability: 'pwm' as const,
        optional: index > 0,
      }));
    case 'actuator_stepper_28byj48':
      return [1, 2, 3, 4].map((index) => ({
        key: `in${index}`,
        label: { zh: `IN${index}`, en: `IN${index}` },
        capability: 'gpio' as const,
      }));
    default:
      if (module.capability === 'gpio' || module.capability === 'adc' || module.capability === 'pwm' || module.capability === 'onewire') {
        return [{ key: 'signal', label: { zh: '信号引脚', en: 'Signal pin' }, capability: module.capability }];
      }
      return [];
  }
}

function resultData(result: ModuleActionResult | null): Reading[] {
  if (!result || !Array.isArray(result.data)) return [];
  return result.data.filter((item): item is Reading => item && typeof item === 'object');
}

function percentValue(value: unknown) {
  return typeof value === 'number' && Number.isFinite(value) ? Math.max(0, Math.min(100, Math.round(value))) : null;
}

function pinDetail(reading: Reading) {
  return typeof reading.pin === 'number' ? `GPIO ${reading.pin}` : undefined;
}

function gpioLevelDetail(reading: Reading) {
  return pinDetail(reading);
}

function gpioLevelMetric(reading: Reading) {
  if (typeof reading.level !== 'number') return undefined;
  const levelText = reading.level === 0
    ? lang() === 'zh' ? '低电平' : 'Low'
    : lang() === 'zh' ? '高电平' : 'High';
  return `${levelText} (${reading.level})`;
}

function isGpioOutputModule(module: ModuleDefinition) {
  return module.controls?.includes('onoff') || module.controls?.includes('level') || module.controls?.includes('color');
}

function isLiveReadModule(module: ModuleDefinition) {
  return Boolean(module.implemented && module.controls?.includes('read') && module.controls?.includes('watch') && !isGpioOutputModule(module));
}

function gpioOutputLabel(reading: Reading) {
  if (reading.name === 'red') return lang() === 'zh' ? '红灯输出' : 'Red output';
  if (reading.name === 'blue') return lang() === 'zh' ? '蓝灯输出' : 'Blue output';
  if (reading.name === 'green') return lang() === 'zh' ? '绿灯输出' : 'Green output';
  return lang() === 'zh' ? '当前输出电平' : 'Current output';
}

function adcDetail(reading: Reading) {
  const parts: string[] = [];
  if (typeof reading.voltage_mv === 'number') {
    const voltage = (reading.voltage_mv / 1000).toFixed(2);
    parts.push(lang() === 'zh' ? `模拟电压：${voltage} V` : `Analog voltage: ${voltage} V`);
  }
  if (typeof reading.raw === 'number') {
    parts.push(`ADC ${reading.raw}`);
  }
  const pin = pinDetail(reading);
  if (pin) parts.push(pin);
  return parts.join(' · ') || undefined;
}

function adcDisplay(moduleId: string, reading: Reading): DisplayReading {
  const raw = typeof reading.raw === 'number' ? reading.raw : null;
  const percent = percentValue(reading.percent);
  const normalPercent = percent ?? 0;
  const invertedPercent = 100 - normalPercent;

  switch (moduleId) {
    case 'sensor_adc_photoresistor':
      return {
        label: lang() === 'zh' ? '亮度' : 'Brightness',
        value: `${invertedPercent}%`,
        metricLabel: lang() === 'zh' ? '模拟电压' : 'Voltage',
        metric: typeof reading.voltage_mv === 'number' ? `${(reading.voltage_mv / 1000).toFixed(2)} V` : undefined,
        detail: adcDetail(reading),
        tone: 'ok',
      };
    case 'sensor_adc_water_level':
      return {
        label: lang() === 'zh' ? '水位' : 'Water level',
        value: `${normalPercent}%`,
        metricLabel: lang() === 'zh' ? '模拟电压' : 'Voltage',
        metric: typeof reading.voltage_mv === 'number' ? `${(reading.voltage_mv / 1000).toFixed(2)} V` : undefined,
        detail: adcDetail(reading),
        tone: normalPercent > 0 ? 'ok' : 'neutral',
      };
    case 'sensor_soil_moisture':
      return {
        label: lang() === 'zh' ? '湿度' : 'Moisture',
        value: `${invertedPercent}%`,
        metricLabel: lang() === 'zh' ? '模拟电压' : 'Voltage',
        metric: typeof reading.voltage_mv === 'number' ? `${(reading.voltage_mv / 1000).toFixed(2)} V` : undefined,
        detail: adcDetail(reading),
        tone: 'ok',
      };
    case 'sensor_sound_level':
      return {
        label: lang() === 'zh' ? '声音强度' : 'Sound level',
        value: `${normalPercent}%`,
        metricLabel: lang() === 'zh' ? '模拟电压' : 'Voltage',
        metric: typeof reading.voltage_mv === 'number' ? `${(reading.voltage_mv / 1000).toFixed(2)} V` : undefined,
        detail: adcDetail(reading),
        tone: normalPercent > 70 ? 'warn' : 'ok',
      };
    default:
      return {
        label: lang() === 'zh' ? '数值' : 'Value',
        value: percent === null ? (raw === null ? '--' : String(raw)) : `${normalPercent}%`,
        metricLabel: lang() === 'zh' ? '模拟电压' : 'Voltage',
        metric: typeof reading.voltage_mv === 'number' ? `${(reading.voltage_mv / 1000).toFixed(2)} V` : undefined,
        detail: adcDetail(reading),
        tone: 'neutral',
      };
  }
}

function displayReadings(module: ModuleDefinition, result: ModuleActionResult | null): DisplayReading[] {
  if (!result) return [];
  if (result.ok === false) {
    return [
      {
        label: lang() === 'zh' ? '测试结果' : 'Test result',
        value: lang() === 'zh' ? '失败' : 'Failed',
        detail: result.message,
        tone: 'error',
      },
    ];
  }
  if (!isGpioOutputModule(module) && (result.status === 'stopped' || result.action === 'stop')) {
    return [
      {
        label: lang() === 'zh' ? '测试状态' : 'Test status',
        value: lang() === 'zh' ? '已停止' : 'Stopped',
        tone: 'neutral',
      },
    ];
  }

  const readings = resultData(result);
  if (readings.length === 0) {
    return [
      {
        label: lang() === 'zh' ? '测试状态' : 'Test status',
        value: result.ok ? (lang() === 'zh' ? '已完成' : 'Complete') : (lang() === 'zh' ? '暂无数据' : 'No data'),
        detail: result.message,
        tone: result.ok ? 'ok' : 'neutral',
      },
    ];
  }

  return readings.map((reading) => {
    if (reading.error) {
      return {
        label: reading.name ?? (lang() === 'zh' ? '数据' : 'Reading'),
        value: lang() === 'zh' ? '读取失败' : 'Read failed',
        detail: reading.message ?? (reading.error === 'ESP_ERR_INVALID_ARG' ? (lang() === 'zh' ? '所选 GPIO 不能作为 ADC 输入，请重新选择 ADC 引脚。' : 'The selected GPIO cannot be used as an ADC input. Choose an ADC pin.') : reading.error),
        tone: 'error' as const,
      };
    }
    if (reading.name === 'temperature' && typeof reading.celsius === 'number') {
      return {
        label: lang() === 'zh' ? '温度' : 'Temperature',
        value: `${reading.celsius.toFixed(1)} °C`,
        detail: gpioLevelDetail(reading),
        tone: 'ok' as const,
      };
    }
    if (reading.name === 'humidity' && typeof reading.percent === 'number') {
      return {
        label: lang() === 'zh' ? '湿度' : 'Humidity',
        value: `${Math.round(reading.percent)}%`,
        detail: gpioLevelDetail(reading),
        tone: 'ok' as const,
      };
    }
    if (reading.name === 'distance' && typeof reading.distance_cm === 'number') {
      return {
        label: lang() === 'zh' ? '距离' : 'Distance',
        value: `${reading.distance_cm.toFixed(1)} cm`,
        metricLabel: lang() === 'zh' ? 'Echo 脉宽' : 'Echo pulse',
        metric: typeof reading.duration_us === 'number' ? `${reading.duration_us} us` : undefined,
        detail: reading.pin !== undefined ? `Echo GPIO ${reading.pin}` : undefined,
        tone: 'ok' as const,
      };
    }
    if (reading.name === 'rgb') {
      const red = reading.red ?? 0;
      const green = reading.green ?? 0;
      const blue = reading.blue ?? 0;
      const off = red === 0 && green === 0 && blue === 0;
      return {
        label: lang() === 'zh' ? '灯光状态' : 'LED state',
        value: off ? (lang() === 'zh' ? '已关闭' : 'Off') : `R ${red} · G ${green} · B ${blue}`,
        metricLabel: lang() === 'zh' ? '灯珠数量' : 'LED count',
        metric: typeof reading.count === 'number' ? String(reading.count) : undefined,
        detail: pinDetail(reading),
        tone: off ? 'neutral' as const : 'ok' as const,
      };
    }
    if (reading.name === 'melody') {
      return {
        label: lang() === 'zh' ? '蜂鸣器状态' : 'Buzzer state',
        value: reading.state === 'stopped' ? (lang() === 'zh' ? '已停止' : 'Stopped') : (lang() === 'zh' ? '播放中' : 'Playing'),
        metricLabel: lang() === 'zh' ? '旋律' : 'Melody',
        metric: reading.title,
        detail: gpioLevelDetail(reading),
        tone: reading.state === 'stopped' ? 'neutral' as const : 'ok' as const,
      };
    }
    if (module.id === 'sensor_rotary_encoder' && reading.name === 'rotary') {
      const delta = reading.delta ?? 0;
      const direction = delta > 0
        ? (lang() === 'zh' ? '正向' : 'Forward')
        : delta < 0
          ? (lang() === 'zh' ? '反向' : 'Reverse')
          : (lang() === 'zh' ? '未变化' : 'No change');
      const details = [
        typeof reading.clk === 'number' ? `CLK ${gpioLevelMetric({ level: reading.clk })}` : undefined,
        typeof reading.dt === 'number' ? `DT ${gpioLevelMetric({ level: reading.dt })}` : undefined,
        typeof reading.button === 'number'
          ? `${lang() === 'zh' ? '按键' : 'Button'} ${reading.button_pressed ? (lang() === 'zh' ? '按下' : 'Pressed') : (lang() === 'zh' ? '未按下' : 'Released')} · ${gpioLevelMetric({ level: reading.button })}`
          : undefined,
        typeof reading.button_events === 'number' ? `${lang() === 'zh' ? '按下次数' : 'Presses'} ${reading.button_events}` : undefined,
        typeof reading.pin === 'number' ? `CLK GPIO ${reading.pin}` : undefined,
        typeof reading.button_pin === 'number' ? `SW GPIO ${reading.button_pin}` : undefined,
      ].filter(Boolean).join(' · ');
      return {
        label: lang() === 'zh' ? '编码器计数' : 'Encoder count',
        value: String(reading.count ?? 0),
        metricLabel: lang() === 'zh' ? '方向' : 'Direction',
        metric: direction,
        detail: details || undefined,
        tone: delta === 0 ? 'neutral' as const : 'ok' as const,
      };
    }
    if (module.id === 'sensor_i2c_mpu6050' && reading.name === 'mpu6050') {
      const details = [
        typeof reading.accel_x_g === 'number' && typeof reading.accel_y_g === 'number' && typeof reading.accel_z_g === 'number'
          ? `ACC X ${reading.accel_x_g.toFixed(2)}g · Y ${reading.accel_y_g.toFixed(2)}g · Z ${reading.accel_z_g.toFixed(2)}g`
          : undefined,
        typeof reading.gyro_x_dps === 'number' && typeof reading.gyro_y_dps === 'number' && typeof reading.gyro_z_dps === 'number'
          ? `GYRO X ${reading.gyro_x_dps.toFixed(1)}°/s · Y ${reading.gyro_y_dps.toFixed(1)}°/s · Z ${reading.gyro_z_dps.toFixed(1)}°/s`
          : undefined,
        typeof reading.celsius === 'number'
          ? `${lang() === 'zh' ? '温度' : 'Temperature'} ${reading.celsius.toFixed(1)} °C`
          : undefined,
        typeof reading.sda === 'number' && typeof reading.scl === 'number'
          ? `SDA GPIO ${reading.sda} · SCL GPIO ${reading.scl}`
          : undefined,
        typeof reading.address === 'number'
          ? `I2C 0x${reading.address.toString(16).toUpperCase().padStart(2, '0')}`
          : undefined,
        reading.swapped
          ? (lang() === 'zh' ? '检测到 SDA/SCL 线序对调' : 'Detected with SDA/SCL swapped')
          : undefined,
      ].filter(Boolean).join(' · ');
      return {
        label: lang() === 'zh' ? '姿态角' : 'Attitude',
        value: typeof reading.pitch_deg === 'number' ? `${reading.pitch_deg.toFixed(1)}°` : '--',
        metricLabel: lang() === 'zh' ? '横滚角' : 'Roll',
        metric: typeof reading.roll_deg === 'number' ? `${reading.roll_deg.toFixed(1)}°` : undefined,
        detail: details || undefined,
        tone: 'ok' as const,
      };
    }
    if (module.id === 'sensor_joystick') {
      if (reading.name === 'x' || reading.name === 'y') {
        const percent = percentValue(reading.percent);
        return {
          label: reading.name === 'x' ? 'X' : 'Y',
          value: percent === null ? '--' : `${percent}%`,
          metricLabel: lang() === 'zh' ? '模拟电压' : 'Voltage',
          metric: typeof reading.voltage_mv === 'number' ? `${(reading.voltage_mv / 1000).toFixed(2)} V` : undefined,
          detail: adcDetail(reading),
          tone: 'ok' as const,
        };
      }
      if (reading.name === 'button' && typeof reading.level === 'number') {
        return {
          label: lang() === 'zh' ? '按键数字电平' : 'Button digital level',
          value: gpioLevelMetric(reading) ?? '--',
          detail: gpioLevelDetail(reading),
          tone: 'neutral' as const,
        };
      }
    }
    if ((module.id === 'sensor_soil_moisture' || module.id === 'sensor_sound_level') && reading.name === 'do' && typeof reading.level === 'number') {
      return {
        label: lang() === 'zh' ? '数字 DO 电平' : 'Digital DO level',
        value: gpioLevelMetric(reading) ?? '--',
        detail: gpioLevelDetail(reading),
        tone: 'neutral' as const,
      };
    }
    if (typeof reading.level === 'number') {
      return {
        label: isGpioOutputModule(module) ? gpioOutputLabel(reading) : lang() === 'zh' ? '数字电平' : 'Digital level',
        value: gpioLevelMetric(reading) ?? '--',
        detail: gpioLevelDetail(reading),
        tone: 'neutral' as const,
      };
    }
    return adcDisplay(module.id, reading);
  });
}

function toneClass(tone: DisplayReading['tone']) {
  switch (tone) {
    case 'ok':
      return 'border-emerald-400/30 bg-emerald-400/10 text-emerald-200';
    case 'warn':
      return 'border-amber-400/35 bg-amber-400/10 text-amber-200';
    case 'error':
      return 'border-red-400/35 bg-red-400/10 text-red-200';
    default:
      return 'border-[var(--color-border-subtle)] bg-white/[0.03] text-[var(--color-text-primary)]';
  }
}

export const ModuleLabPage: Component<Props> = (props) => {
  const modules = createMemo(() => modulesForCategory(props.category));
  const groups = createMemo(() => Array.from(new Set(modules().map((item) => pick(item.group)))));
  const [group, setGroup] = createSignal<string>('all');
  const [selectedId, setSelectedId] = createSignal(modules()[0]?.id ?? MODULES[0]?.id ?? '');
  const selected = createMemo(() => modules().find((item) => item.id === selectedId()) ?? modules()[0]);
  const [pinOptions, setPinOptions] = createSignal<Record<PinCapability, number[]>>({
    gpio: [],
    adc: [],
    pwm: [],
    i2c: [],
    onewire: [],
  });
  const [pins, setPins] = createSignal<Record<string, number | null>>(emptyPins);
  const [saved, setSaved] = createSignal(false);
  const [dirty, setDirty] = createSignal(false);
  const [busy, setBusy] = createSignal(false);
  const [result, setResult] = createSignal<ModuleActionResult | null>(null);
  const [livePaused, setLivePaused] = createSignal(false);
  const [melody, setMelody] = createSignal('twinkle');
  let liveReadBusy = false;

  useDirtyTracker(props.category, dirty);

  const visibleModules = createMemo(() => {
    const currentGroup = group();
    if (currentGroup === 'all') return modules();
    return modules().filter((item) => pick(item.group) === currentGroup);
  });

  const fields = createMemo(() => {
    const module = selected();
    return module ? pinFields(module) : [];
  });

  const ready = createMemo(() => {
    const module = selected();
    if (!module || !module.implemented) return false;
    return fields().every((field) => field.optional || pins()[field.key] !== null && pins()[field.key] !== undefined);
  });
  const liveReadEnabled = createMemo(() => {
    const module = selected();
    return Boolean(module && ready() && isLiveReadModule(module) && !livePaused());
  });

  const loadPins = async () => {
    const caps: PinCapability[] = ['gpio', 'adc', 'pwm', 'i2c', 'onewire'];
    const entries = await Promise.all(
      caps.map(async (capability) => [capability, (await fetchPins(capability)).pins] as const),
    );
    setPinOptions(Object.fromEntries(entries) as Record<PinCapability, number[]>);
  };

  const loadConfig = async (moduleId: string) => {
    setBusy(true);
    setResult(null);
    setLivePaused(false);
    try {
      const config = await fetchModuleConfig(moduleId);
      setPins(config.saved ? (config.pins ?? {}) : {});
      setSaved(Boolean(config.saved));
      setDirty(false);
    } catch (err) {
      setPins({});
      setSaved(false);
      setDirty(false);
      pushToast((err as Error).message, 'error', 4500);
    } finally {
      setBusy(false);
    }
  };

  onMount(() => {
    void loadPins();
  });

  createEffect(() => {
    const moduleId = selectedId();
    if (moduleId) void loadConfig(moduleId);
  });

  const setPin = (key: string, value: string) => {
    const next = value === '' ? null : Number(value);
    setPins({ ...pins(), [key]: next });
    setDirty(true);
    setSaved(false);
    setResult(null);
    setLivePaused(false);
  };

  const save = async () => {
    const module = selected();
    if (!module) return;
    setBusy(true);
    try {
      const config = await saveModuleConfig(module.id, { pins: pins(), params: {} });
      setPins(config.pins ?? pins());
      setSaved(true);
      setDirty(false);
      pushToast(lang() === 'zh' ? '配置已保存' : 'Configuration saved', 'success', 2500);
    } catch (err) {
      pushToast((err as Error).message, 'error', 4500);
    } finally {
      setBusy(false);
    }
  };

  const clearConfig = async () => {
    const module = selected();
    if (!module) return;
    setBusy(true);
    try {
      await deleteModuleConfig(module.id);
      setPins({});
      setSaved(false);
      setDirty(false);
      setResult(null);
    } catch (err) {
      pushToast((err as Error).message, 'error', 4500);
    } finally {
      setBusy(false);
    }
  };

  const runAction = async (action: 'read' | 'start' | 'stop' | 'control', params: Record<string, unknown> = {}) => {
    const module = selected();
    if (!module) return;
    if (action === 'read') setLivePaused(false);
    setBusy(true);
    try {
      setResult(await moduleAction(module.id, action, { pins: pins(), params }));
    } catch (err) {
      setResult({ ok: false, message: (err as Error).message });
    } finally {
      setBusy(false);
    }
  };

  createEffect(() => {
    const module = selected();
    const currentPins = pins();
    const enabled = liveReadEnabled();
    if (!module || !enabled) return;

    const readLive = async () => {
      if (liveReadBusy) return;
      liveReadBusy = true;
      try {
        setResult(await moduleAction(module.id, 'read', { pins: currentPins, params: {} }));
      } catch (err) {
        setResult({ ok: false, message: (err as Error).message });
      } finally {
        liveReadBusy = false;
      }
    };

    void readLive();
    const timer = window.setInterval(() => void readLive(), 700);
    onCleanup(() => window.clearInterval(timer));
  });

  return (
    <div>
      <PageHeader title={pick(categoryTitle[props.category])} description={pick(categoryDesc[props.category])} />
      <div class="p-5 grid gap-5 xl:grid-cols-[minmax(280px,360px),1fr]">
        <Section title={lang() === 'zh' ? '模块列表' : 'Modules'} defaultOpen>
          <div class="flex flex-wrap gap-2 mb-4">
            <Button size="xs" active={group() === 'all'} onClick={() => setGroup('all')}>
              {lang() === 'zh' ? '全部' : 'All'}
            </Button>
            <For each={groups()}>
              {(item) => (
                <Button size="xs" active={group() === item} onClick={() => setGroup(item)}>
                  {item}
                </Button>
              )}
            </For>
          </div>
          <div class="grid gap-2">
            <For each={visibleModules()}>
              {(module) => {
                const Icon = module.icon;
                return (
                  <button
                    type="button"
                    class={[
                      'w-full text-left rounded-[var(--radius-md)] border p-3 transition flex items-center gap-3',
                      selectedId() === module.id
                        ? 'border-[var(--color-accent-soft)] bg-[var(--color-accent)]/10'
                        : 'border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)] hover:border-[var(--color-border-strong)]',
                    ].join(' ')}
                    onClick={() => setSelectedId(module.id)}
                  >
                    <span class="w-10 h-10 rounded-full bg-white/[0.04] flex items-center justify-center shrink-0">
                      <Icon class="w-5 h-5 text-[var(--color-accent-soft)]" />
                    </span>
                    <span class="min-w-0 flex-1">
                      <span class="block text-sm text-[var(--color-text-primary)] truncate">{pick(module.name)}</span>
                      <span class="block text-[0.75rem] text-[var(--color-text-muted)] truncate">{pick(module.group)}</span>
                    </span>
                    <span
                      class={[
                        'text-[0.68rem] px-2 py-0.5 rounded-full border',
                        module.implemented
                          ? 'text-emerald-300 border-emerald-400/30 bg-emerald-400/10'
                          : 'text-amber-300 border-amber-400/30 bg-amber-400/10',
                      ].join(' ')}
                    >
                      {module.implemented ? (lang() === 'zh' ? '可测试' : 'Ready') : lang() === 'zh' ? '预留' : 'Reserved'}
                    </span>
                  </button>
                );
              }}
            </For>
          </div>
        </Section>

        <Show when={selected()}>
          {(module) => {
            const Icon = module().icon;
            return (
              <div class="rounded-[var(--radius-lg)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-panel)] overflow-hidden">
                <div class="p-5 border-b border-[var(--color-border-subtle)] flex flex-wrap items-start justify-between gap-4">
                  <div class="flex items-center gap-3 min-w-0">
                    <span class="w-12 h-12 rounded-full bg-[var(--color-accent)]/12 flex items-center justify-center shrink-0">
                      <Icon class="w-6 h-6 text-[var(--color-accent-soft)]" />
                    </span>
                    <div class="min-w-0">
                      <h3 class="m-0 text-base font-semibold text-[var(--color-text-primary)]">{pick(module().name)}</h3>
                      <p class="m-0 mt-1 text-[0.82rem] text-[var(--color-text-muted)]">{pick(module().description)}</p>
                    </div>
                  </div>
                  <div class="flex items-center gap-2">
                    <Show when={saved()}>
                      <span class="inline-flex items-center gap-1 text-[0.75rem] text-emerald-300">
                        <CheckCircle2 class="w-3.5 h-3.5" />
                        {lang() === 'zh' ? '已保存' : 'Saved'}
                      </span>
                    </Show>
                    <Show when={dirty()}>
                      <span class="text-[0.75rem] text-amber-300">{lang() === 'zh' ? '未保存' : 'Unsaved'}</span>
                    </Show>
                  </div>
                </div>

                <div class="p-5 grid gap-5 lg:grid-cols-[minmax(260px,0.9fr),1.1fr]">
                  <div class="space-y-4">
                    <div class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-white/[0.02] p-4">
                      <h4 class="m-0 mb-3 text-sm font-semibold text-[var(--color-text-primary)]">
                        {lang() === 'zh' ? '引脚配置' : 'Pin configuration'}
                      </h4>
                      <Show
                        when={fields().length > 0}
                        fallback={
                          <p class="m-0 text-[0.82rem] text-[var(--color-text-muted)]">
                            {lang() === 'zh' ? '此模块不需要选择 ESP32-S3 引脚。' : 'This module does not require ESP32-S3 pin selection.'}
                          </p>
                        }
                      >
                        <div class="grid gap-3">
                          <For each={fields()}>
                            {(field) => (
                              <label class="grid gap-1.5">
                                <span class="text-[0.75rem] text-[var(--color-text-muted)]">
                                  {pick(field.label)}
                                  <Show when={field.optional}>
                                    <span class="ml-1 opacity-70">({lang() === 'zh' ? '可选' : 'optional'})</span>
                                  </Show>
                                </span>
                                <select
                                  class="w-full rounded-[var(--radius-sm)] border border-[var(--color-border-strong)] bg-[var(--color-bg-surface)] px-3 py-2 text-sm text-[var(--color-text-primary)]"
                                  value={pins()[field.key] ?? ''}
                                  onInput={(event) => setPin(field.key, event.currentTarget.value)}
                                >
                                  <option value="">{lang() === 'zh' ? '请选择' : 'Select pin'}</option>
                                  <For each={pinOptions()[field.capability]}>
                                    {(pin) => <option value={pin}>GPIO {pin}</option>}
                                  </For>
                                </select>
                              </label>
                            )}
                          </For>
                        </div>
                      </Show>
                      <div class="mt-4 flex flex-wrap gap-2">
                        <Button variant="primary" disabled={busy()} onClick={() => void save()}>
                          <Save class="w-4 h-4" />
                          {lang() === 'zh' ? '保存配置' : 'Save config'}
                        </Button>
                        <Button disabled={busy()} onClick={() => void clearConfig()}>
                          <Eraser class="w-4 h-4" />
                          {lang() === 'zh' ? '清除配置' : 'Clear'}
                        </Button>
                        <Button disabled={busy()} onClick={() => void loadConfig(module().id)}>
                          <RotateCcw class="w-4 h-4" />
                          {lang() === 'zh' ? '重新载入' : 'Reload'}
                        </Button>
                      </div>
                    </div>

                    <div class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-white/[0.02] p-4">
                      <h4 class="m-0 mb-2 text-sm font-semibold text-[var(--color-text-primary)]">
                        {lang() === 'zh' ? '实时状态' : 'Live status'}
                      </h4>
                      <p class="m-0 text-[0.82rem] text-[var(--color-text-muted)]">
                        <Show
                          when={module().implemented}
                          fallback={lang() === 'zh' ? '预留页面，驱动暂未接入。' : 'Reserved page. Driver is not connected yet.'}
                        >
                          {ready()
                            ? isLiveReadModule(module())
                              ? lang() === 'zh'
                                ? livePaused()
                                  ? '实时读取已暂停。'
                                  : '正在实时读取传感器数据。'
                                : livePaused()
                                  ? 'Live reading is paused.'
                                  : 'Reading sensor data live.'
                              : lang() === 'zh'
                                ? '引脚已选择，可以开始测试。'
                                : 'Pins are selected and the module can be tested.'
                            : lang() === 'zh'
                              ? '请先完成必选引脚。'
                              : 'Complete required pin selection first.'}
                        </Show>
                      </p>
                      <Show when={result()}>
                        <div class="mt-3 grid gap-3 sm:grid-cols-2">
                          <For each={displayReadings(module(), result())}>
                            {(item) => (
                                <div class={['rounded-[var(--radius-md)] border p-4', toneClass(item.tone)].join(' ')}>
                                  <div class="text-[0.75rem] opacity-75">{item.label}</div>
                                  <div class="mt-1 text-2xl font-semibold tracking-normal">{item.value}</div>
                                  <Show when={item.metric}>
                                    <div class="mt-3 inline-flex items-baseline gap-2 rounded-[var(--radius-sm)] bg-black/20 px-3 py-2">
                                      <span class="text-[0.72rem] opacity-75">
                                        {item.metricLabel ?? (lang() === 'zh' ? '测量值' : 'Measured')}
                                      </span>
                                      <span class="text-xl font-semibold tracking-normal">{item.metric}</span>
                                    </div>
                                  </Show>
                                  <Show when={item.detail}>
                                    <div class="mt-3 text-[0.78rem] leading-relaxed opacity-80">{item.detail}</div>
                                  </Show>
                                </div>
                            )}
                          </For>
                        </div>
                      </Show>
                    </div>
                  </div>

                  <div class="space-y-4">
                    <div class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-white/[0.02] p-4">
                      <h4 class="m-0 mb-2 text-sm font-semibold text-[var(--color-text-primary)]">
                        {lang() === 'zh' ? '操作提示' : 'Operation guide'}
                      </h4>
                      <p class="text-[0.82rem] text-[var(--color-text-muted)]">{pick(module().wiring)}</p>
                      <p class="text-[0.82rem] text-[var(--color-text-muted)]">{pick(module().operation)}</p>
                    </div>

                    <div class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-white/[0.02] p-4">
                      <h4 class="m-0 mb-3 text-sm font-semibold text-[var(--color-text-primary)]">
                        {lang() === 'zh' ? '操作按钮' : 'Controls'}
                      </h4>
                      <div class="flex flex-wrap gap-2">
                        <Show when={module().controls?.includes('onoff')}>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('start', { level: 1 })}>
                            <Play class="w-4 h-4" />
                            {lang() === 'zh' ? '开始测试' : 'Start test'}
                          </Button>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('stop', { level: 0 })}>
                            <Square class="w-4 h-4" />
                            {lang() === 'zh' ? '停止' : 'Stop'}
                          </Button>
                        </Show>
                        <Show when={module().controls?.includes('level')}>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('control', { level: 1 })}>
                            <Play class="w-4 h-4" />
                            {lang() === 'zh' ? '输出高电平' : 'Output high'}
                          </Button>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('control', { level: 0 })}>
                            <Square class="w-4 h-4" />
                            {lang() === 'zh' ? '输出低电平' : 'Output low'}
                          </Button>
                        </Show>
                        <Show when={module().controls?.includes('color')}>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('control', { red: 1, blue: 0, green: 0 })}>
                            <Play class="w-4 h-4" />
                            {lang() === 'zh' ? '红灯' : 'Red'}
                          </Button>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('control', { red: 0, blue: 1, green: 0 })}>
                            <Play class="w-4 h-4" />
                            {lang() === 'zh' ? '蓝灯' : 'Blue'}
                          </Button>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('control', { red: 0, blue: 0, green: 1 })}>
                            <Play class="w-4 h-4" />
                            {lang() === 'zh' ? '绿灯' : 'Green'}
                          </Button>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('stop', { red: 0, blue: 0, green: 0 })}>
                            <Square class="w-4 h-4" />
                            {lang() === 'zh' ? '全部关闭' : 'All off'}
                          </Button>
                        </Show>
                        <Show when={module().id === 'sensor_pwm_passive_buzzer'}>
                          <select
                            class="rounded-[var(--radius-sm)] border border-[var(--color-border-strong)] bg-[var(--color-bg-surface)] px-3 py-2 text-sm text-[var(--color-text-primary)]"
                            value={melody()}
                            onInput={(event) => setMelody(event.currentTarget.value)}
                          >
                            <option value="two_tigers">{lang() === 'zh' ? '两只老虎' : 'Two Tigers'}</option>
                            <option value="twinkle">{lang() === 'zh' ? '一闪一闪亮晶晶' : 'Twinkle Twinkle Little Star'}</option>
                            <option value="ruyuan">{lang() === 'zh' ? '如愿（简版）' : 'Ru Yuan short pattern'}</option>
                          </select>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('control', { melody: melody() })}>
                            <Play class="w-4 h-4" />
                            {lang() === 'zh' ? '播放旋律' : 'Play melody'}
                          </Button>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('stop', { melody: melody() })}>
                            <Square class="w-4 h-4" />
                            {lang() === 'zh' ? '停止' : 'Stop'}
                          </Button>
                        </Show>
                        <Show when={isLiveReadModule(module())}>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('read')}>
                            <Play class="w-4 h-4" />
                            {lang() === 'zh' ? '立即读取' : 'Read now'}
                          </Button>
                          <Button disabled={!ready() || busy()} onClick={() => setLivePaused(!livePaused())}>
                            <Show when={livePaused()} fallback={<Square class="w-4 h-4" />}>
                              <Play class="w-4 h-4" />
                            </Show>
                            {livePaused()
                              ? lang() === 'zh'
                                ? '恢复实时读取'
                                : 'Resume live read'
                              : lang() === 'zh'
                                ? '暂停实时读取'
                                : 'Pause live read'}
                          </Button>
                        </Show>
                        <Show when={!isGpioOutputModule(module()) && !isLiveReadModule(module()) && module().id !== 'sensor_pwm_passive_buzzer'}>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('read')}>
                            <Play class="w-4 h-4" />
                            {lang() === 'zh' ? '读取一次' : 'Read once'}
                          </Button>
                          <Button disabled={!ready() || busy()} onClick={() => void runAction('start')}>
                            <Play class="w-4 h-4" />
                            {lang() === 'zh' ? '开始测试' : 'Start test'}
                          </Button>
                          <Button disabled={!module().implemented || busy()} onClick={() => void runAction('stop')}>
                            <Square class="w-4 h-4" />
                            {lang() === 'zh' ? '停止' : 'Stop'}
                          </Button>
                        </Show>
                      </div>
                    </div>

                    <div class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-white/[0.02] p-4">
                      <h4 class="m-0 mb-3 text-sm font-semibold text-[var(--color-text-primary)]">
                        {lang() === 'zh' ? '验收步骤' : 'Acceptance steps'}
                      </h4>
                      <ol class="m-0 pl-5 space-y-2 text-[0.82rem] text-[var(--color-text-muted)]">
                        <For each={module().acceptance}>{(item) => <li>{pick(item)}</li>}</For>
                      </ol>
                    </div>
                  </div>
                </div>
              </div>
            );
          }}
        </Show>
      </div>
    </div>
  );
};





