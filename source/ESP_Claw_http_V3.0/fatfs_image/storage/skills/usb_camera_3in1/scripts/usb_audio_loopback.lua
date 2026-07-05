local capability = require("capability")
local system = require("system")

local TAG = "[usb_camera_3in1]"
local raw_args = type(args) == "table" and args or {}

local function string_arg(name, default)
  local value = raw_args[name]
  if value == nil or value == "" then
    return default
  end
  if type(value) ~= "string" then
    error(TAG .. " args." .. name .. " must be a string")
  end
  return value
end

local function int_arg(name, default, min_value, max_value)
  local value = raw_args[name]
  if value == nil then
    return default
  end
  local n = tonumber(value)
  if n == nil or math.floor(n) ~= n then
    error(TAG .. " args." .. name .. " must be an integer")
  end
  if min_value ~= nil and n < min_value then
    error(TAG .. " args." .. name .. " is below minimum " .. min_value)
  end
  if max_value ~= nil and n > max_value then
    error(TAG .. " args." .. name .. " is above maximum " .. max_value)
  end
  return n
end

local function parse_http_output(out)
  local first_line, body = tostring(out or ""):match("^(.-)\n(.*)$")
  if not first_line then
    first_line = tostring(out or "")
    body = ""
  end
  local status = tonumber(first_line:match("^HTTP%s+(%d+)"))
  if not status then
    error(TAG .. " unexpected http_request output: " .. tostring(out))
  end
  return status, body
end

local function build_url()
  local explicit_url = string_arg("url", nil)
  if explicit_url then
    return explicit_url
  end

  local host = string_arg("host", nil)
  if not host then
    host = system.ip()
  end
  if not host or host == "" then
    host = "127.0.0.1"
  end

  return "http://" .. host .. "/api/camera/audio/loopback"
end

local function run()
  local url = build_url()
  local timeout_ms = int_arg("timeout_ms", 50000, 1000, 120000)

  print(TAG .. " route=HTTP_UAC_LOOPBACK")
  print(TAG .. " url=" .. url)
  print(TAG .. " note=uses the same USB Audio Class path as the Camera page; it does not use Lua audio/audio_adc/audio_dac")

  local ok, out, err = capability.call("http_request", {
    url = url,
    method = "POST",
    timeout_ms = timeout_ms,
    max_body_bytes = 4096,
  }, {
    source_cap = "usb_camera_3in1",
  })

  if not ok then
    local text = tostring(err or out or "unknown error")
    error(TAG .. " HTTP_UAC_LOOPBACK failed: " .. text)
  end

  local status, body = parse_http_output(out)
  print(TAG .. " http_status=" .. tostring(status))
  if body ~= "" then
    print(TAG .. " response=" .. body)
  end
  if status < 200 or status >= 300 then
    error(TAG .. " USB Audio Class loopback failed with HTTP " .. tostring(status) .. ": " .. body)
  end
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
  error(err)
end
