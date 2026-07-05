---
{
  "name": "usb_camera_3in1",
  "description": "Use the USB 3-in-1 camera: UVC photo capture plus USB Audio Class microphone/speaker loopback through the same HTTP API used by the Camera web page.",
  "metadata": {
    "cap_groups": [
      "cap_http_request",
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# USB 3-in-1 Camera

Use this skill for requests about the USB 3-in-1 camera, UVC camera, camera
microphone, camera speaker, recording audio, or playing recorded audio.

Strong trigger phrases include:

- USB camera
- UVC camera
- USB 3-in-1 camera
- USB 摄像头
- UVC 摄像头
- 三合一摄像头
- 摄像头拍照
- USB 摄像头拍照
- 摄像头麦克风
- USB 麦克风
- 摄像头喇叭
- USB 喇叭
- 录音
- 收音
- 播放
- 录音播放
- 录一段再播放
- 测试麦克风和喇叭

## Critical Routing Rules

This board uses a USB 3-in-1 camera. Its audio is USB Audio Class (UAC), not
analog ADC/DAC.

For microphone, speaker, recording, playback, or "record then play" requests:

- DO use the Camera web page backend API: `POST /api/camera/audio/loopback`.
- DO use `http_request` when available.
- DO NOT use the Lua `audio` module directly.
- DO NOT call `board_manager.get_audio_codec_input_params("audio_adc")`.
- DO NOT call `board_manager.get_audio_codec_output_params("audio_dac")`.
- DO NOT write a new script that initializes `audio_adc` or `audio_dac`.

The endpoint already performs the required USB flow:

1. Rejects the request if MJPEG video is still streaming.
2. Closes/deinitializes the UVC camera path if needed.
3. Initializes the board UAC microphone and UAC speaker.
4. Plays prompt tones.
5. Records from the USB camera microphone.
6. Plays the recording through the USB camera speaker.

If the user asks to take a photo, capture a snapshot, or save a picture, activate
and use the `take_picture` skill. Do not answer with text only.

If the user asks about video preview or camera stream, answer that the web page
Camera tab uses MJPEG preview and the stream should be stopped before audio
testing.

## Preferred Tool Call

For microphone/speaker/audio requests, call `http_request` directly:

```json
{
  "url": "http://127.0.0.1/api/camera/audio/loopback",
  "method": "POST",
  "timeout_ms": 50000,
  "max_body_bytes": 4096
}
```

If `127.0.0.1` is unavailable but the device IP is known, use the device's LAN
address instead:

```json
{
  "url": "http://192.168.0.126/api/camera/audio/loopback",
  "method": "POST",
  "timeout_ms": 50000,
  "max_body_bytes": 4096
}
```

The expected success response body contains:

```json
{
  "ok": true,
  "recorded_bytes": 320000,
  "sample_rate": 16000,
  "channels": 1,
  "bits_per_sample": 16,
  "duration_seconds": 10
}
```

## Lua Bridge Fallback

If direct `http_request` routing is not selected, run exactly this bridge script
with `lua_run_script`. The script still calls the same HTTP/UAC endpoint and does
not use Lua audio or board ADC/DAC initialization.

```json
{"path":"{CUR_SKILL_DIR}/scripts/usb_audio_loopback.lua","args":{"timeout_ms":50000},"timeout_ms":60000}
```

Optional bridge args:

```json
{
  "host": "192.168.0.126",
  "url": "http://192.168.0.126/api/camera/audio/loopback",
  "timeout_ms": 50000
}
```

## Failure Handling

- If the endpoint returns `409 Stop video before running audio test.`, tell the
  user to stop the camera video preview/stream and retry.
- If the endpoint returns `503`, tell the user the USB Audio Class microphone or
  speaker was not found or failed to initialize. Ask them to replug the USB
  3-in-1 camera and retry after stopping video.
- Do not claim the problem is analog ADC/DAC. This hardware path is USB UAC.
