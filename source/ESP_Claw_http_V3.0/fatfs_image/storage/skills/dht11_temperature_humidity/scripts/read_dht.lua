local environmental_sensor = require("environmental_sensor")

local TAG = "[dht11_temperature_humidity]"
local raw_args = type(args) == "table" and args or {}

local function int_arg(name, default, min_value, max_value)
  local value = raw_args[name]
  if value == nil then
    return default
  end
  local n = tonumber(value)
  if n == nil or math.floor(n) ~= n then
    error(TAG .. " args." .. name .. " must be an integer")
  end
  if n < min_value or n > max_value then
    error(TAG .. " args." .. name .. " must be between " .. min_value .. " and " .. max_value)
  end
  return n
end

local function string_arg(name, default)
  local value = raw_args[name]
  if value == nil then
    return default
  end
  if type(value) ~= "string" then
    error(TAG .. " args." .. name .. " must be a string")
  end
  local lower = string.lower(value)
  if lower ~= "dht11" and lower ~= "dht22" and lower ~= "si7021" then
    error(TAG .. " args." .. name .. " must be dht11, dht22, or si7021")
  end
  return lower
end

local pin = int_arg("pin", 40, 0, 48)
local sensor_type = string_arg("sensor_type", "dht11")
local sensor

local function cleanup()
  pcall(function()
    if sensor then
      sensor:close()
    end
  end)
end

local function run()
  sensor = environmental_sensor.new({
    type = "dht",
    pin = pin,
    sensor_type = sensor_type,
  })

  local sample = sensor:read()
  print(string.format(
    "%s sensor=%s pin=%d temperature=%.1f C humidity=%.1f %%",
    TAG,
    sensor_type,
    pin,
    sample.temperature,
    sample.humidity
  ))
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print(TAG .. " ERROR: " .. tostring(err))
  error(err)
end
