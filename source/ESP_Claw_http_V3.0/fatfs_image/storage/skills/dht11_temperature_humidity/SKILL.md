---
{
  "name": "dht11_temperature_humidity",
  "description": "Read temperature and humidity from a DHT11 or DHT-family sensor connected to a GPIO pin.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# DHT11 Temperature And Humidity

Use this skill when the user asks for temperature, humidity, room temperature, environmental readings, DHT11, DHT22, or 温湿度.

Strong trigger phrases include: DHT11, DHT22, temperature, humidity, temp, humid, 温度, 湿度, 温湿度, 读取温湿度, 当前温度, 当前湿度, 环境传感器.

Run exactly one script with `lua_run_script`.

Default wiring:
- DHT11 DATA -> GPIO40

If the user specifies a GPIO pin, pass it as `pin`. Do not keep the default when the user gives a DHT data pin.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "pin": {
      "type": "integer",
      "default": 40,
      "minimum": 0,
      "maximum": 48
    },
    "sensor_type": {
      "type": "string",
      "default": "dht11",
      "enum": ["dht11", "dht22", "si7021"]
    }
  }
}
```

## Tool Call Inputs

Read the default DHT11 on GPIO40:

```json
{"path":"{CUR_SKILL_DIR}/scripts/read_dht.lua","args":{}}
```

Read DHT11 on a user-specified GPIO:

```json
{"path":"{CUR_SKILL_DIR}/scripts/read_dht.lua","args":{"pin":4,"sensor_type":"dht11"}}
```

## Recommended Flow

1. Use this skill for all DHT11/DHT22/temperature/humidity/温湿度 requests.
2. If the user names a GPIO pin, pass it as `pin`.
3. Report the script output directly, including temperature, humidity, sensor type, and pin.
4. If reading fails, tell the user to check data pin, 3.3V/5V power, pull-up resistor, and GND.
