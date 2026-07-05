import { createSignal, onCleanup, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import { PageHeader } from '../components/ui/PageHeader';
import { Section } from '../components/ui/Section';

export const CameraPage: Component = () => {
  let audioController: AbortController | null = null;
  let audioTimeout: ReturnType<typeof setTimeout> | null = null;
  const [activeTab, setActiveTab] = createSignal<'video' | 'audio'>('video');
  const [streaming, setStreaming] = createSignal(false);
  const [snapshotUrl, setSnapshotUrl] = createSignal('');
  const [streamKey, setStreamKey] = createSignal(0);
  const [streamUrl, setStreamUrl] = createSignal('');
  const [streamError, setStreamError] = createSignal('');
  const [closing, setClosing] = createSignal(false);
  const [audioRunning, setAudioRunning] = createSignal(false);
  const [audioMessage, setAudioMessage] = createSignal('');
  const [audioError, setAudioError] = createSignal('');

  const mjpegUrl = () =>
    `${window.location.protocol}//${window.location.hostname}:8081/camera/mjpeg`;

  const takeSnapshot = () => {
    setSnapshotUrl(`/api/camera/snapshot?t=${Date.now()}`);
  };

  const startStream = () => {
    if (closing()) {
      return;
    }
    setStreamError('');
    const nextKey = streamKey() + 1;
    setStreamKey(nextKey); // force img reload
    setStreamUrl(`${mjpegUrl()}?k=${nextKey}&t=${Date.now()}`);
    setStreaming(true);
  };

  const stopStream = async () => {
    setStreamUrl('');
    setStreamError('');
    setStreaming(false);
    setClosing(true);
    try {
      await fetch('/api/camera/close', { method: 'POST' });
    } catch {
      // The preview is already disconnected; the next close/start will retry the device state.
    } finally {
      setClosing(false);
    }
  };

  const runAudioLoopback = async () => {
    if (streaming() || closing() || audioRunning()) {
      return;
    }
    audioController?.abort();
    if (audioTimeout) {
      clearTimeout(audioTimeout);
    }
    audioController = new AbortController();
    setAudioRunning(true);
    setAudioMessage('');
    setAudioError('');
    audioTimeout = setTimeout(() => {
      audioController?.abort();
      setAudioRunning(false);
      setAudioError(t('cameraAudioTimeout') as string);
    }, 45_000);
    try {
      const response = await fetch('/api/camera/audio/loopback', {
        method: 'POST',
        cache: 'no-store',
        signal: audioController.signal,
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || (t('cameraAudioFailed') as string));
      }
      const data = await response.json();
      setAudioMessage(
        `${t('cameraAudioDone') as string} ${data.recorded_bytes ?? 0} bytes`,
      );
    } catch (error) {
      if (error instanceof DOMException && error.name === 'AbortError') {
        setAudioError(t('cameraAudioTimeout') as string);
      } else {
        setAudioError(error instanceof Error ? error.message : (t('cameraAudioFailed') as string));
      }
    } finally {
      if (audioTimeout) {
        clearTimeout(audioTimeout);
        audioTimeout = null;
      }
      audioController = null;
      setAudioRunning(false);
    }
  };

  onCleanup(() => {
    audioController?.abort();
    if (audioTimeout) {
      clearTimeout(audioTimeout);
    }
    setStreamUrl('');
    fetch('/api/camera/close', { method: 'POST', keepalive: true }).catch(() => {});
  });

  return (
    <div>
      <PageHeader
        title={t('navCamera') as string}
        description={t('cameraDescription') as string}
      />
      <Section title={t('navCamera') as string}>
        <div class="flex flex-col gap-6">
          <div class="flex border-b border-[var(--color-border-subtle)]">
            <button
              class={[
                'px-5 py-3 text-sm transition',
                activeTab() === 'video'
                  ? 'bg-red-950/50 text-[var(--color-accent)]'
                  : 'text-[var(--color-text-muted)] hover:text-[var(--color-text)]',
              ].join(' ')}
              onClick={() => setActiveTab('video')}
            >
              {t('cameraVideoTab')}
            </button>
            <button
              class={[
                'px-5 py-3 text-sm transition',
                activeTab() === 'audio'
                  ? 'bg-red-950/50 text-[var(--color-accent)]'
                  : 'text-[var(--color-text-muted)] hover:text-[var(--color-text)]',
              ].join(' ')}
              onClick={() => setActiveTab('audio')}
            >
              {t('cameraAudioTab')}
            </button>
          </div>

          <Show when={activeTab() === 'video'}>
            <div class="flex flex-col items-center gap-4">
              <div class="flex gap-2">
                <button
                  onClick={startStream}
                  disabled={closing() || streaming()}
                  class={[
                    'px-4 py-2 text-sm rounded transition',
                    closing()
                      ? 'bg-gray-600 text-gray-400 cursor-wait'
                      : streaming()
                      ? 'bg-green-700 text-white cursor-default'
                      : 'bg-blue-600 text-white hover:bg-blue-700',
                  ].join(' ')}
                >
                  {t('cameraStreamOn')}
                </button>
                <button
                  onClick={() => void stopStream()}
                  disabled={closing() || !streaming()}
                  class={[
                    'px-4 py-2 text-sm rounded transition',
                    closing()
                      ? 'bg-gray-600 text-gray-400 cursor-wait'
                      : streaming()
                      ? 'bg-red-600 text-white hover:bg-red-700'
                      : 'bg-gray-600 text-gray-400 cursor-default',
                  ].join(' ')}
                >
                  {t('cameraStreamOff')}
                </button>
              </div>

              <Show when={streaming() && streamUrl()}>
                <div class="relative flex min-h-[180px] w-full max-w-[640px] items-center justify-center overflow-hidden rounded border border-[var(--color-border-subtle)] bg-black">
                  <img
                    src={streamUrl()}
                    alt="Camera stream"
                    class="max-h-[480px] max-w-full object-contain"
                    onError={() => setStreamError(t('cameraLoadError') as string)}
                  />
                </div>
                <Show
                  when={streamError()}
                  fallback={
                    <p class="text-xs text-[var(--color-text-muted)]">
                      {t('cameraStreamHint')}
                    </p>
                  }
                >
                  <p class="text-xs text-red-400">{streamError()}</p>
                </Show>
              </Show>

              <button
                onClick={takeSnapshot}
                class="px-4 py-2 bg-blue-600 text-white rounded hover:bg-blue-700 transition"
              >
                {t('cameraSnapshot')}
              </button>

              <Show when={snapshotUrl()}>
                <div class="mt-4">
                  <p class="text-sm text-[var(--color-text-muted)] mb-2">
                    {t('cameraSnapshotResult')}
                  </p>
                  <img
                    src={snapshotUrl()}
                    alt="Snapshot"
                    class="max-w-xs rounded border border-[var(--color-border-subtle)]"
                  />
                </div>
              </Show>
            </div>
          </Show>

          <Show when={activeTab() === 'audio'}>
            <div class="flex flex-col items-center gap-3">
              <button
                onClick={() => void runAudioLoopback()}
                disabled={audioRunning() || streaming() || closing()}
                class={[
                  'px-4 py-2 text-sm rounded transition',
                  audioRunning() || streaming() || closing()
                    ? 'bg-gray-600 text-gray-400 cursor-wait'
                    : 'bg-blue-600 text-white hover:bg-blue-700',
                ].join(' ')}
              >
                {audioRunning() ? t('cameraAudioRunning') : t('cameraAudioLoopback')}
              </button>
              <p class="max-w-[520px] text-center text-xs leading-5 text-[var(--color-text-muted)]">
                {t('cameraAudioHint')}
              </p>
              <Show when={audioMessage()}>
                <p class="text-xs text-green-400">{audioMessage()}</p>
              </Show>
              <Show when={audioError()}>
                <p class="max-w-[520px] text-center text-xs text-red-400">{audioError()}</p>
              </Show>
            </div>
          </Show>
        </div>
      </Section>
    </div>
  );
};
