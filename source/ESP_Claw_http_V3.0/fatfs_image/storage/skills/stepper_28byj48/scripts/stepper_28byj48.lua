-- --------------------------------------------------------------
-- Control a 28BYJ-48 stepper motor through a ULN2003 driver.
-- --------------------------------------------------------------

local gpio = require("gpio")
local delay = require("delay")

local TAG = "[stepper_28byj48]"

local DEFAULT_IN1 = 46
local DEFAULT_IN2 = 17
local DEFAULT_IN3 = 18
local DEFAULT_IN4 = 21
local DEFAULT_RPM = 8
local DEFAULT_STEPS = 128
local DEFAULT_STEPS_PER_REV = 4096
local DEFAULT_RELEASE_AFTER_MOVE = true

local MIN_GPIO = 0
local MAX_GPIO = 48
local MIN_RPM = 1
local MAX_RPM = 15
local MAX_STEPS = 16384
local MAX_DEGREES = 1440
local MAX_REVOLUTIONS = 4

local HALF_STEP_SEQUENCE = {
  { 1, 0, 0, 0 },
  { 1, 1, 0, 0 },
  { 0, 1, 0, 0 },
  { 0, 1, 1, 0 },
  { 0, 0, 1, 0 },
  { 0, 0, 1, 1 },
  { 0, 0, 0, 1 },
  { 1, 0, 0, 1 },
}

local raw_args = type(args) == "table" and args or {}

local function fail(message)
  error(TAG .. " " .. message)
end

local function number_arg(name, default)
  local value = raw_args[name]
  if value == nil then
    return default
  end

  local n = tonumber(value)
  if n == nil then
    fail("args." .. name .. " must be a number")
  end
  return n
end

local function int_arg(name, default)
  local n = number_arg(name, default)
  if n == nil then
    return nil
  end
  if math.floor(n) ~= n then
    fail("args." .. name .. " must be an integer")
  end
  return n
end

local function bool_arg(name, default)
  local value = raw_args[name]
  if value == nil then
    return default
  end
  if type(value) ~= "boolean" then
    fail("args." .. name .. " must be a boolean")
  end
  return value
end

local function string_arg(name, default)
  local value = raw_args[name]
  if value == nil then
    return default
  end
  if type(value) ~= "string" then
    fail("args." .. name .. " must be a string")
  end
  return value
end

local function validate_gpio(name, value)
  if value < MIN_GPIO or value > MAX_GPIO then
    fail("args." .. name .. " must be in GPIO range " .. MIN_GPIO .. "-" .. MAX_GPIO)
  end
  return value
end

local function pin_list_arg(index, default)
  local list = raw_args.pins or raw_args.gpios
  if type(list) ~= "table" or list[index] == nil then
    return default
  end

  local n = tonumber(list[index])
  if n == nil or math.floor(n) ~= n then
    fail("args.pins[" .. index .. "] must be an integer GPIO number")
  end
  return n
end

local function ensure_unique_pins(pins)
  local seen = {}
  for _, pin in ipairs(pins) do
    if seen[pin] then
      fail("IN pins must be unique")
    end
    seen[pin] = true
  end
end

local function normalize_direction(value)
  local direction = string.lower(value or "cw")
  if direction == "clockwise" or direction == "forward" then
    return "cw"
  end
  if direction == "counterclockwise" or direction == "counter-clockwise" or
      direction == "anticlockwise" or direction == "reverse" or direction == "backward" then
    return "ccw"
  end
  if direction ~= "cw" and direction ~= "ccw" then
    fail("args.direction must be cw or ccw")
  end
  return direction
end

local function reverse_direction(direction)
  if direction == "cw" then
    return "ccw"
  end
  return "cw"
end

local function rounded(value)
  return math.floor(value + 0.5)
end

local function compute_motion(direction)
  local steps = raw_args.steps
  local degrees = raw_args.degrees
  local revolutions = raw_args.revolutions
  local amount
  local source

  if steps ~= nil then
    amount = int_arg("steps", nil)
    source = "steps"
  elseif degrees ~= nil then
    amount = number_arg("degrees", nil)
    if math.abs(amount) > MAX_DEGREES then
      fail("args.degrees must be between -" .. MAX_DEGREES .. " and " .. MAX_DEGREES)
    end
    amount = rounded((amount / 360.0) * DEFAULT_STEPS_PER_REV)
    source = "degrees"
  elseif revolutions ~= nil then
    amount = number_arg("revolutions", nil)
    if math.abs(amount) > MAX_REVOLUTIONS then
      fail("args.revolutions must be between -" .. MAX_REVOLUTIONS .. " and " .. MAX_REVOLUTIONS)
    end
    amount = rounded(amount * DEFAULT_STEPS_PER_REV)
    source = "revolutions"
  else
    amount = DEFAULT_STEPS
    source = "default"
  end

  if amount < 0 then
    direction = reverse_direction(direction)
    amount = -amount
  end

  if amount > MAX_STEPS then
    fail("requested move is too large; maximum is " .. MAX_STEPS .. " half-steps")
  end

  return amount, direction, source
end

local pins = {
  validate_gpio("in1", int_arg("in1", pin_list_arg(1, DEFAULT_IN1))),
  validate_gpio("in2", int_arg("in2", pin_list_arg(2, DEFAULT_IN2))),
  validate_gpio("in3", int_arg("in3", pin_list_arg(3, DEFAULT_IN3))),
  validate_gpio("in4", int_arg("in4", pin_list_arg(4, DEFAULT_IN4))),
}
ensure_unique_pins(pins)

local action = string.lower(string_arg("action", "move"))
local direction = normalize_direction(string_arg("direction", "cw"))
local rpm = number_arg("rpm", DEFAULT_RPM)
local release_after_move = bool_arg("release_after_move", DEFAULT_RELEASE_AFTER_MOVE)

if rpm < MIN_RPM or rpm > MAX_RPM then
  fail("args.rpm must be between " .. MIN_RPM .. " and " .. MAX_RPM)
end

if action ~= "move" and action ~= "release" then
  fail("args.action must be move or release")
end

local step_delay_us = rounded(60000000 / (DEFAULT_STEPS_PER_REV * rpm))

local function configure_pins()
  for _, pin in ipairs(pins) do
    gpio.set_direction(pin, "output")
  end
end

local function write_phase(phase)
  for i, pin in ipairs(pins) do
    gpio.set_level(pin, phase[i])
  end
end

local function release_motor()
  write_phase({ 0, 0, 0, 0 })
end

local function run_move()
  local total_steps, resolved_direction, source = compute_motion(direction)

  configure_pins()

  if total_steps == 0 then
    release_motor()
    print(TAG .. " no movement requested; phases released")
    return
  end

  for i = 1, total_steps do
    local offset = (i - 1) % #HALF_STEP_SEQUENCE
    local index

    if resolved_direction == "cw" then
      index = offset + 1
    else
      index = #HALF_STEP_SEQUENCE - offset
    end

    write_phase(HALF_STEP_SEQUENCE[index])
    delay.delay_us(step_delay_us)
    if i % 64 == 0 then
      delay.delay_ms(1)
    end
  end

  if release_after_move then
    release_motor()
  end

  local approx_degrees = (total_steps / DEFAULT_STEPS_PER_REV) * 360.0
  print(string.format(
    "%s moved %d half-steps direction=%s source=%s approx_degrees=%.1f rpm=%.1f pins=%d,%d,%d,%d release_after_move=%s",
    TAG,
    total_steps,
    resolved_direction,
    source,
    approx_degrees,
    rpm,
    pins[1],
    pins[2],
    pins[3],
    pins[4],
    tostring(release_after_move)
  ))
end

local function run()
  configure_pins()

  if action == "release" then
    release_motor()
    print(string.format("%s phases released pins=%d,%d,%d,%d", TAG, pins[1], pins[2], pins[3], pins[4]))
    return
  end

  run_move()
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
  pcall(function()
    configure_pins()
    release_motor()
  end)
  print(TAG .. " ERROR: " .. tostring(err))
  error(err)
end
