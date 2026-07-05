---
{
  "name": "stepper_28byj48",
  "description": "Control a 5V 28BYJ-48 stepper motor through a ULN2003 four-phase driver board. Supports natural language requests such as stepper motor, 28BYJ48, ULN2003, clockwise, counterclockwise, rotate one turn, half turn, degrees, or steps.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# 28BYJ-48 Stepper Motor

Use this skill when the user asks to control a 28BYJ-48 stepper motor, ULN2003 driver board, four-phase stepper, or says phrases such as 步进电机, 28BYJ48, ULN2003, 转一圈, 转半圈, 转一下, 顺时针, 逆时针, 反方向, 转90度, or 转200步.

Run exactly one script with `lua_run_script`.

Default wiring:
- ULN2003 IN1 -> GPIO46
- ULN2003 IN2 -> GPIO17
- ULN2003 IN3 -> GPIO18
- ULN2003 IN4 -> GPIO21

These are defaults only. If the user says which GPIO pins are connected to IN1,
IN2, IN3, and IN4, the tool call MUST pass those exact pins as `in1`, `in2`,
`in3`, and `in4`. Do not keep the defaults when the user gives wiring. If the
user gives all four pins as an ordered list, treat the order as IN1, IN2, IN3,
IN4 and pass them as `pins: [in1, in2, in3, in4]` or as the explicit fields.
If the user gives fewer than four stepper wiring pins, ask for the missing IN
pins before moving instead of guessing.

The motor must use an external 5V supply for the ULN2003 board, and the ULN2003 GND must be connected to ESP32 GND. Do not power the motor from ESP32 GPIO.

If `lua_run_script` returns an error, report that error directly to the user. Do not retry with different pins, speed, or direction unless the user explicitly asks.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "direction": {
      "type": "string",
      "default": "cw",
      "enum": ["cw", "ccw"]
    },
    "steps": {
      "type": "integer",
      "minimum": 0,
      "maximum": 16384
    },
    "degrees": {
      "type": "number",
      "minimum": -1440,
      "maximum": 1440
    },
    "revolutions": {
      "type": "number",
      "minimum": -4,
      "maximum": 4
    },
    "rpm": {
      "type": "number",
      "default": 8,
      "minimum": 1,
      "maximum": 15
    },
    "release_after_move": {
      "type": "boolean",
      "default": true
    },
    "action": {
      "type": "string",
      "default": "move",
      "enum": ["move", "release"]
    },
    "in1": { "type": "integer", "default": 46 },
    "in2": { "type": "integer", "default": 17 },
    "in3": { "type": "integer", "default": 18 },
    "in4": { "type": "integer", "default": 21 },
    "pins": {
      "type": "array",
      "items": { "type": "integer" },
      "minItems": 4,
      "maxItems": 4
    },
    "gpios": {
      "type": "array",
      "items": { "type": "integer" },
      "minItems": 4,
      "maxItems": 4
    }
  }
}
```

Argument precedence for motion amount is `steps`, then `degrees`, then `revolutions`. If none is provided, use `steps: 128` for a small test movement.

Negative `steps`, `degrees`, or `revolutions` reverse the selected direction and use the absolute amount.

## Tool Call Inputs

Small clockwise test:
```json
{"path":"{CUR_SKILL_DIR}/scripts/stepper_28byj48.lua","args":{"direction":"cw","steps":128}}
```

Clockwise one revolution:
```json
{"path":"{CUR_SKILL_DIR}/scripts/stepper_28byj48.lua","args":{"direction":"cw","revolutions":1}}
```

Counterclockwise 90 degrees:
```json
{"path":"{CUR_SKILL_DIR}/scripts/stepper_28byj48.lua","args":{"direction":"ccw","degrees":90}}
```

Move 200 steps slowly:
```json
{"path":"{CUR_SKILL_DIR}/scripts/stepper_28byj48.lua","args":{"direction":"cw","steps":200,"rpm":4}}
```

User-specified wiring and motion:
```json
{"path":"{CUR_SKILL_DIR}/scripts/stepper_28byj48.lua","args":{"in1":4,"in2":5,"in3":6,"in4":7,"direction":"cw","steps":200}}
```

Ordered wiring list, interpreted as IN1, IN2, IN3, IN4:
```json
{"path":"{CUR_SKILL_DIR}/scripts/stepper_28byj48.lua","args":{"pins":[46,17,18,21],"direction":"ccw","degrees":90}}
```

Release all motor phases:
```json
{"path":"{CUR_SKILL_DIR}/scripts/stepper_28byj48.lua","args":{"action":"release"}}
```

## Recommended Flow

1. Use this skill for all 28BYJ-48, ULN2003, four-phase stepper, or 步进电机 requests.
2. For "转一下" or a vague move request, run a small test movement with `steps: 128`.
3. For "一圈", use `revolutions: 1`; for "半圈", use `revolutions: 0.5`.
4. For degree requests, use `degrees`; for explicit step requests, use `steps`.
5. Map 顺时针 or clockwise to `direction: "cw"`.
6. Map 逆时针, 反方向, or counterclockwise to `direction: "ccw"`.
7. If the user asks to stop, release, or de-energize the motor, use `action: "release"`.
8. Keep the default pins only when the user does not specify wiring.
9. When the user specifies stepper wiring, pass every specified IN pin in the same tool call that moves or releases the motor.
10. Report the script output directly to the user.
