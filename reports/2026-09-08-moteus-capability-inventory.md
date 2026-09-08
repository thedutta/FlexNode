# What the moteus firmware already gives FlexNode / CATBOT — capability inventory

_2026-09-08. Standing report for Aditya. Baseline: this fork at `5ba2bd8`, upstream firmware ABI
`0x010100` (`fw/moteus_hw.h:209`), register map version 5 (`fw/moteus_controller.cc:47`), board
identity `{family 0, hw_version 8}` (`fw/moteus_hw.cc:37-41`). Companion to
[`docs/can-layer.md`](../docs/can-layer.md) and
[`2026-09-08-can-subsystem-design.md`](2026-09-08-can-subsystem-design.md)._

> **How to read this.** Every claim below is either cited to a source line in `moteus-r4-parent/`
> (paths relative to that directory, `file:line`) or to an upstream doc under
> `moteus-r4-parent/docs/`. Where the firmware has been changed by FlexNode, the FlexNode file is
> cited instead. "Validated" means run on the FlexNode board; almost nothing in the motor path is
> — see the bring-up log. Where behaviour is hedged ("appears to", "the docs say but the code
> does not show") that is deliberate.
>
> **What is validated on FlexNode hardware as of 2026-09-06/07** (`notes/bringup-log.md`): boot,
> main loop, WS2812, IMU over aux2 I²C, standalone power-up. **Not validated:** anything that
> energises the gate driver, the encoder (not soldered), CAN (no adapter), `bus_V` vs DMM, config
> persistence. Everything in §1–§4 about torque production is therefore "what the code does on a
> stock r4.11", not "what we have seen on FlexNode".

---

## 0. The short version

**You get, for free, without writing a line:**

1. A complete FOC servo with fifteen control modes, of which a legged robot uses exactly three
   (position, zero-velocity as the timeout fallback, stopped) and the bench uses two more
   (voltage-FOC for open-loop spin, current for tuning). §1.
2. A position/velocity/feed-forward-torque controller whose command semantics were designed for
   idempotent commands from a high-rate host: `(pos, vel, ff_torque, kp_scale, kd_scale,
   max_torque)` every cycle, with an on-board acceleration-limited trajectory planner that you
   can switch off. §2.
3. Automatic calibration that measures resistance, inductance (d and q), Kv, pole count,
   direction, and a 64-point encoder-to-electrical-angle map, then sets the current-loop
   bandwidth and encoder PLL filter for you. §3.
4. Fault handling for over/under-voltage, FET and motor over-temperature with linear derating,
   gate-driver faults, encoder loss, position/velocity error, plus current, power, velocity and
   position-bound limiting — **in the current-controlled modes only.** §4.
5. Multi-turn absolute output position with a reducer: an output-side I²C encoder (AS5600) can be
   configured as `motor_position.output.reference_source` so the joint angle is known at boot
   without homing. This is the `enc_i2c` channel's primary job, and moteus already does it. §5.4.
6. A 100 ms command watchdog with configurable fallback (damped zero-velocity by default). §2.7.
7. Two aux ports with GPIO in/out, analog in, PWM-in measurement, I²C master (three devices),
   SPI encoders, UART modes including a full register-protocol server over a plain UART. §5.
8. The whole host stack: `moteus_tool`, `tview`, the Python and C++ client libraries, the CAN
   bootloader, a diagnostic tunnel with three streams, and flash-backed configuration. §6.
9. Thirteen telemetry channels readable live with binary schemas. §7.

**Five traps found in the source that will bite CATBOT specifically:**

| Trap | Where | Consequence |
|---|---|---|
| `servo.default_accel_limit` defaults to **50 rev/s²**, and a mode write resets the command to defaults every frame | `fw/bldc_servo_structs.h:594`, `fw/moteus_controller.cc:650`, `fw/bldc_servo_control.h:229-233` | Every gait-rate position command is trajectory-limited unless the corenode sends `accel_limit < 0` each cycle **or** `servo.default_accel_limit` is set to `nan`. Set the config. |
| `servopos.position_min/max` default to **±0.01 rev** | `fw/bldc_servo_structs.h:744-745` | Position mode refuses to start (fault 39) unless the joint is within ±3.6° of zero. Set real limits or `nan` per node. |
| The watchdog only fires in **position** and **stay-within** modes | `fw/bldc_servo_control.h:1670-1674` | A current, voltage-FOC, brake or zero-velocity command with no follow-up runs **forever**. Never leave a node in one of those from a script. |
| `max_current_A`, `max_power_W`, temperature derating, position bounds are enforced **only inside `ISR_DoCurrent`** | `fw/bldc_servo_control.h:817-1124`, §4.2 matrix | Voltage-FOC / PWM / voltage / voltage-DQ / measure-inductance have no current limit at all (only bus-voltage clamps). The bench already learned this. |
| The onboard AS5047 appears to be on **SPI2 = PB13/PB14/PB15**, not SPI1 | `fw/moteus_controller.cc:409-411,518-520`, `fw/aux_mbed.h:604-605`, §5.2 | If true, the "SPI2 expansion pads → TIM1 for the SimpleFOC Mini" plan disconnects the rotor encoder. Netlist check before anything else. |

**The real gaps** are exactly the channel model in `docs/can-layer.md` §3 plus four drivers
(servo pulse, load-cell ADC, ToF, Mini commutation) and the fleet layer. Ranked in §8.

---

## 1. Control modes

### 1.1 The full set

`enum BldcServoMode` (`fw/bldc_servo_structs.h:75-144`); wire names (`:811-829`); register
`0x000` (`docs/protocol/registers.md:77-100`). Column "needs" is what the mode requires before it
will run without faulting; "CATBOT" is whether the running robot ever commands it.

| # | Name | What it does | Needs | Limits enforced | CATBOT |
|---|---|---|---|---|---|
| 0 | `stopped` | Driver disabled after a cooldown (256 cycles of zero-current control then 64 cycles Hi-Z, `fw/bldc_servo_control.h:1434-1449`, config `:658-659`). **Also the only way to clear a fault or leave timeout** (`:1541-1544`). Re-calibrates ADC current offsets on the way out (`fw/bldc_servo.cc:1081-1108`). | — | — | **Yes** — boot state, fault clear |
| 1 | `fault` | Entered on any fault; driver enabled, outputs off. Cannot be commanded. | — | — | observed only |
| 2–4 | `enabling` / `calibrating` / `calib_complete` | Transit states from stopped: DRV8353 enable + CSA offset calibration (`fw/drv8323.cc:77-125`), then 256 ADC samples to find the three current-sense zeros, faulting if any is >200 counts off 2048 (`fw/bldc_servo.cc:1091-1102`). Cannot be commanded. | — | — | transit |
| 5 | `pwm` | Raw duty on the three phases, 0–1 (`:1709-1711`). | nothing | duty clamped to `[min_pwm, max_pwm]` only (`:478-480`) | bench only |
| 6 | `voltage` | Three phase voltages, re-centred on the bus midpoint (`:747-768`). | nothing | duty clamp only | bench only |
| 7 | `voltage_foc` | Sinusoidal voltage at a commanded electrical angle `theta`, advancing at `theta_rate` (`:1424-1432`). **Does not read the encoder and does not check `theta_valid` or `motor.poles`.** | nothing | voltage clamped to `(0.5 - min_pwm) · bus · kSvpwmRatio`; **no current limit** | bench: first spin (already planned, `notes/bringup-log.md` 2026-09-07) |
| 8 | `voltage_dq` | Commanded d/q voltage through the real encoder angle (`:795-815`). | `motor.poles`, `theta_valid` | voltage clamp; **no current limit** | no |
| 9 | `current` | Closed-loop d/q current (`:1725-1729` → `ISR_DoCurrent`). | poles, theta_valid | full chain (§4.2) | tuning only; **no watchdog** |
| 10 | `position` | The main mode: PID on position/velocity → torque → q current, with trajectory planner, feed-forward, limits (§2). | poles, theta_valid, valid output position | full chain + max torque + control-error faults | **Yes — the gait mode** |
| 11 | `pos_timeout` | Entered automatically when the watchdog expires in 10/13; runs `servo.timeout_mode` (`:1451-1476`). Exit only via stop (`:1620-1622`). Can also be commanded directly (`d tmt`). | as per fallback | as per fallback | **automatic** (host-loss floor, can-layer §7 G0) |
| 12 | `zero_vel` | Position controller with `kp_scale = 0`, `ilimit_scale = 0`, velocity target 0, torque capped at `timeout_max_torque_Nm` — a pure damper (`:1137-1154`). | as 10 | full chain, cap 5 Nm default | as the default timeout fallback; **no watchdog when commanded directly** |
| 13 | `within` | Zero torque (feed-forward only) inside `[bounds_min, bounds_max]`; outside, PID back to the violated bound (`:1370-1422`). | as 10 | full chain | maybe — a soft joint limit; see §2.5 |
| 14 | `meas_ind` | Square-wave voltage excitation on d or q to measure inductance (`:1478-1524`). Calibration internal. | theta_valid | voltage clamp only | calibration only |
| 15 | `brake` | All three low-side FETs on; phases shorted (`fw/bldc_servo.cc:1278-1283`). Passive braking, no control. | nothing | none (bus/temperature faults still checked) | candidate knee timeout fallback (design report §9.1); **no watchdog** |

Mode transitions (`ISR_MaybeChangeMode`, `fw/bldc_servo_control.h:1532-1626`): from `stopped`
every active mode first goes through 2→3→4 — the DRV enable takes >1 ms of the 1 kHz poll
(`fw/drv8323.cc:90-99`) and the ADC calibration 256 control cycles (≈8.5 ms at 30 kHz). **First
torque after a stop is therefore roughly 10–15 ms away.** Between active modes the switch is
immediate and clears the PID state (`:1606-1608`). From `fault` or `pos_timeout`, only `stopped` is
accepted; everything else is silently ignored (`:1564-1566`, `:1620-1622`).

### 1.2 What a gait controller actually drives

Only mode 10. The design report's fast lane writes `mode = 10` plus `0x020–0x022` every cycle
(`2026-09-08-can-subsystem-design.md` §4.1), which is also what `moteus.Controller.make_position`
emits (`lib/python/moteus/moteus.py:700-703`). Everything else is either automatic (11, 1, 2–4),
bench (5–9, 14), or a policy choice for the timeout fallback (0/10/12/15,
`fw/bldc_servo_structs.h:615-622`).

Torque-only control is not a separate mode: it is mode 10 with `kp_scale = kd_scale =
ilimit_scale = 0` and a feed-forward torque (`docs/guides/control-modes.md:113-170`). The same
doc's caveat applies to CATBOT: host-side torque control at 350 Hz has ~1/85 the bandwidth of
the on-board 30 kHz loop, so formulate as much of the law as possible as
`position + velocity + feedforward` with scaled gains.

### 1.3 The 30 kHz loop, for orientation

`servo.pwm_rate_hz` defaults to 30 000 for family 0 hw ≥ 3 (`fw/bldc_servo_structs.h:498-502`);
the control interrupt runs at PWM rate up to 30 kHz and at half rate above that
(`fw/bldc_servo_control.h:146-157`). The ISR is split into a top-priority timer half that waits
for the hardware-triggered ADCs, and a PendSV half at priority 6 that does the maths
(`fw/bldc_servo.cc:433-456`, `:758-848`). Encoder SPI reads and aux sampling happen inside that
ISR (`:885-1007`). Anything FlexNode adds to the main loop is pre-empted by it; anything added
to the ISR eats duty-cycle headroom (`:850-883`).

---

## 2. Position mode and the trajectory machinery

### 2.1 The command and its units

`BldcServoCommandData` (`fw/bldc_servo_structs.h:330-433`); registers `0x020–0x02d`
(`fw/moteus_controller.cc:270-283`, `docs/protocol/registers.md:319-465`). All position
quantities are **output-shaft revolutions**, velocities **rev/s**, torques **N·m at the output**
(after `rotor_to_output_ratio`). Integer wire scalings are in `fw/moteus_controller.cc:99-140`
and `docs/protocol/registers.md:9-73`:

| Reg | Field | Unit | int8 / int16 / int32 LSB | Default if not written | Notes |
|---|---|---|---|---|---|
| `0x020` | position | rev | 0.01 / 0.0001 / 0.00001 | 0.0 | NaN (or int min) = "start from where I am" |
| `0x021` | velocity | rev/s | 0.1 / 0.00025 / 0.00001 | 0.0 | the setpoint advances at this rate forever |
| `0x022` | feedforward torque | N·m | 0.5 / 0.01 / 0.001 | 0.0 | added after the PID |
| `0x023` | kp_scale | — | PWM mapping (1/127, 1/32767) | 1.0 | multiplies `servo.pid_position.kp` |
| `0x024` | kd_scale | — | PWM | 1.0 | multiplies kd |
| `0x025` | max torque | N·m | torque | NaN → **100 N·m** in the struct (`:359`); effectively "current limit decides" | see §4 |
| `0x026` | stop position | rev | position | NaN | **deprecated**; fault 45 if combined with limits (`fw/bldc_servo_control.h:286-292`) |
| `0x027` | watchdog timeout | s | 0.01 / 0.001 / 1e-6 | 0 → `servo.default_timeout_s`; NaN → never | §2.7 |
| `0x028` | velocity limit | rev/s | velocity | NaN → `servo.default_velocity_limit`; **negative → no limit** | §2.4 |
| `0x029` | accel limit | rev/s² | 0.05 / 0.001 / 0.00001 | NaN → `servo.default_accel_limit` (**50**); negative → none | §2.4 |
| `0x02a` | fixed voltage override | V | voltage | NaN | open-loop gimbal mode for this command |
| `0x02b` | ilimit_scale | — | PWM | 1.0 | scales the integrator clamp |
| `0x02c` | fixed current override | A | current | NaN | open-loop with a d-axis current |
| `0x02d` | ignore position bounds | bool | int | 0 | disables `servopos` limits for this command |

**Every frame that writes `0x000` resets the whole command struct to these defaults first**
(`fw/moteus_controller.cc:643-653`). That is what makes commands idempotent and stateless, and it
is why a field the corenode does not send each cycle silently reverts.

Fast-lane consequence: the design report uses int16 for position/velocity on the wire. With the
scalings above that is **±3.2767 rev** and **±8.19 rev/s** before saturation (`ScaleSaturate`
clamps to ±max and reserves int min for NaN, `fw/moteus_controller.cc:71-84`). Fine for a joint
that has been zeroed; not fine for a joint whose position was never set — see §2.6.

### 2.2 The PID, in physical units

`PID::Apply` (`fw/pid.h:108-162`) with config `servo.pid_position`
(`docs/reference/configuration.md:30-50`):

```
error       = measured_position - control_position          (rev)      pid.h:132
error_rate  = measured_velocity - control_velocity          (rev/s)    :133
integral   += error · ki · dt, clamped to ±(ilimit · ilimit_scale)     :136-152
p = kp_scale · kp · error ;  d = kd_scale · kd · error_rate            :154-155
command = sign · (p + d + integral)                         (N·m)      :158-159
```

`sign` is −1 by default so positive error (measured ahead of target) gives negative torque
(`fw/bldc_servo_structs.h:679`). Defaults `kp = 4 N·m/rev`, `ki = 1`, `ilimit = 0` (so the
integrator does nothing until `ilimit` is set), `kd = 0.05 N·m/(rev/s)` (`:674-680`). These are
r4.11 defaults for a generic small motor, not a tuned leg; expect to raise kp substantially and
set kd by the plant.

The measured velocity fed to the D term passes through `servo.velocity_threshold`, a deadband on
the velocity *error* (`fw/bldc_servo_control.h:1217-1220`, default 0). Then:

```
unlimited_torque = PID + feedforward_Nm + 2π · control_acceleration · inertia_feedforward   :1226-1237
limited_torque   = clamp(unlimited_torque, ±max_torque_Nm)  → sets limit code 102            :1254-1261
q_A              = torque_to_current(limited_torque · rotor_to_output_ratio) + cogging comp  :1266-1298
```

`torque_to_current` is the inverse of the Kv-derived torque constant with an optional saturation
model (`fw/torque_model.h`, `fw/bldc_servo.cc:313-320`, `fw/bldc_servo_structs.h:459-470`). If
Kv is zero (uncalibrated) the constant falls back to 0.1 N·m/A **and q current is clamped to
±5 A** (`fw/bldc_servo_control.h:176-177`, `:1300-1303`) — a useful built-in guard for the first
closed-loop test.

The docs say kd_scale "is internally limited to be no more than the kp scale"
(`docs/protocol/registers.md:372`); I could not find that clamp in `fw/pid.h` or the callers.
Treat the two scales as independent until proven otherwise.

### 2.3 What the setpoint does between commands

`BldcServoPosition::UpdateCommand` (`fw/bldc_servo_position.h:293-489`) maintains
`control_position` and `control_velocity` and integrates them every ISR cycle in 48-bit
fixed point (`:365-385`; 1 rev = 2⁴⁸ counts, `fw/motor_position.h:427-433`). Three regimes:

1. **No limits (both NaN).** The commanded position becomes `control_position` immediately, the
   commanded velocity becomes `control_velocity`, and the position command is consumed
   (`:322-331`). From then on the setpoint advances at `velocity` each cycle (`:374`). A host
   sending `(x_c, v_c)` at rate f therefore gets a PD around `x = x_c + v_c·t` between frames —
   the "match the trajectory" semantic of `docs/reference/configuration.md:71-74`. **This is what
   a gait controller wants**, with the internal planner off.
2. **Velocity limit only.** Setpoint moves toward the target at ±`velocity_limit` and snaps when
   it would cross (`:66-86`).
3. **Acceleration limit (with or without velocity limit).** A bang-bang planner: accelerate,
   cruise at the velocity limit, decelerate at exactly the rate needed to arrive at the target
   with the target velocity (`CalculateAcceleration`, `:97-181`; `DoVelocityAndAccelLimits`,
   `:183-270`). The target itself keeps advancing at the commanded velocity while the trajectory
   runs (`:387-396`). `trajectory_done` (register `0x00b`) is set when velocity and position are
   both within tolerance (`:246-269`). Once done, the final velocity is held indefinitely
   (`docs/guides/control-modes.md:22-28`).

When position control starts from stopped, `control_position` is captured from the measured
position and `control_velocity` from the filtered measured velocity — treated as exactly zero if
below `servo.velocity_zero_capture_threshold` (0.05 rev/s) — so a moving joint is picked up
without a torque step (`:332-342`). `0x133 RecapturePositionVelocity` does the same on demand
(`fw/bldc_servo.cc:407-417`), intended for use after a period of zero-gain torque control.

The setpoint velocity is always clamped to `motor_max_velocity`, the bus-voltage-derived
achievable speed (`:355-361`, computed in `fw/bldc_servo_control.h:535-624`), so a velocity
command the bus cannot support is trimmed rather than wound up.

### 2.4 Limits and defaults (read this before the first CAN session)

`PrepareCommand` (`fw/bldc_servo_control.h:212-310`) resolves defaults **before** the command
reaches the ISR:

- `timeout_s == 0` → `servo.default_timeout_s` (0.1 s); NaN → never (`:221-223`).
- `velocity_limit` NaN → `servo.default_velocity_limit` (NaN = none); **negative → NaN** (`:224-228`).
- `accel_limit` NaN → `servo.default_accel_limit` = **50 rev/s²** (`fw/bldc_servo_structs.h:594`);
  negative → NaN (`:229-233`).
- If either limit ends up finite, a missing velocity limit is filled with `servo.max_velocity`
  (500 rev/s) and any velocity limit is capped there (`:237-244`).
- The commanded velocity is clamped to ±velocity_limit (`:248-253`).

So with stock config **every position command is acceleration-limited to 50 rev/s²** (≈314
rad/s² at the output). For a joint that is a hard cap on how fast the setpoint can change,
invisible to the host except as tracking lag. For CATBOT either set `servo.default_accel_limit
nan` (and leave `default_velocity_limit nan`) per node, or send `0x029 = -1` every cycle (costs
bytes on the fast lane). Config is the right answer; note it in the bring-up config list.

`servo.max_position_slip` / `servo.max_velocity_slip` (`fw/bldc_servo_position.h:398-427`,
`docs/reference/configuration.md:144-158`) bound how far the setpoint may run ahead of the
measured state — the standard fix for "catch-up" lunges in velocity mode. Both NaN by default.

### 2.5 Position bounds and `stay_within`

`servopos.position_min/max` (`fw/bldc_servo_structs.h:743-752`) act three ways:

1. Starting position or stay-within mode outside them faults with 39 `kStartOutsideLimit`
   (`fw/bldc_servo_control.h:1594-1598`); bounds beyond ±32768 rev fault with 48 (`:1599-1604`).
2. The **setpoint** is saturated at the bounds every cycle, and hitting one zeroes the control
   velocity (`fw/bldc_servo_position.h:429-448`, `:474-479`).
3. Inside `ISR_DoCurrent`, q current that pushes further past a bound is derated linearly to
   zero over `servo.position_derate` (0.02 rev) beyond it, with limit code 103 (`:831-859`).

The defaults are **±0.01 rev**. They must be set per joint (a real range, or `nan` to disable)
before position mode will even start. `0x02d ignore_position_bounds` bypasses all three for one
command (`fw/bldc_servo_position.h:443`, `fw/bldc_servo_control.h:833`).

`stay_within` (13) is a softer tool: bounds are per-command (`0x040/0x041`), zero PID torque
inside, feed-forward still applied, PID to the violated bound outside
(`fw/bldc_servo_control.h:1370-1422`). It shares the watchdog with position mode. For CATBOT it
is a possible "compliant leg with hard stops" primitive during stand-up or fall; the gait itself
would not use it.

### 2.6 Position representation, wrap, homing, rezero

- Internal position is 64-bit fixed point (48 bits per revolution); float I/O is limited to
  ±32768 rev (`docs/reference/limits.md:3-26`; `ISR_InvalidLimits`). Wrap-around is safe when the
  position command is NaN or kp is zero (velocity control) — otherwise a wrap is a large error.
- Three "homed" states (`fw/motor_position.h:325-335`, register `0x00c`,
  `docs/protocol/registers.md:164-172`): `relative` (nothing known), `rotor` (referenced to the
  absolute rotor encoder — the state after boot with the AS5047), `output` (referenced to the
  output).
- **With a reducer the output position is ambiguous at boot.** The AS5047 gives the rotor angle;
  moteus latches the output position on the first sample as the rotor angle scaled by
  `rotor_to_output_ratio`, wrapped to ±½ of one rotor turn's worth of output travel
  (`fw/motor_position.h:924-938`, `output_ambiguity_scale_` `:769-772`). For an 8:1 hip that is
  ±1/16 rev; for a 14:1 knee ±1/28 rev. The host then has to disambiguate.
- Disambiguation tools, all stock:
  - `0x130 SetOutputNearest` — pick the whole number of rotor turns that puts the output nearest
    the given value (`fw/motor_position.h:458-470`, `:1465-1547`). Python `set_output_nearest`.
    This is what `moteus_tool --zero-offset` uses after storing an offset (`moteus_tool.py:1250-1263`).
  - `0x131 SetOutputExact` — force the output position (`:421-425`).
  - `0x132 RequireReindex` — back to `relative`, forces re-homing (`:472-478`).
  - `d cfg-set-output <pos>` — adjust `motor_position.output.offset` so the current reading equals
    `pos` (not persisted until `conf write`; `docs/protocol/diagnostic.md:159-167`).
  - **`motor_position.output.reference_source`** — a second, output-side absolute source
    (I²C AS5600 on the joint, `reference = output`) used at power-on to resolve the ambiguity
    automatically (`fw/motor_position.h:968-983`, `:1477-1530`;
    `docs/reference/encoders.md:316-319`). Homed goes straight to `output` with no host action.
    See §5.4 — this is the single most valuable thing the aux subsystem already does for a legged
    robot.
- `motor_position.output.offset` / `.sign` set the zero and direction at the output
  (`fw/motor_position.h:157-181`).

### 2.7 The watchdog, precisely

- Every accepted position/stay-within command carries `timeout_s` (resolved as in §2.4). The ISR
  latches a nonzero or NaN value into `status.timeout_s` once and then decrements it each cycle
  (`fw/bldc_servo.cc:890-894`, `fw/bldc_servo_control.h:1635-1638`).
- When it reaches zero **and the mode is 10 or 13**, mode becomes 11 (`:1670-1674`). Any other
  active mode is not watched.
- Mode 11 runs `servo.timeout_mode`: 0 stop, 10 decelerate-and-hold using the default
  velocity/accel limits and default gains, 12 zero-velocity damping capped at
  `timeout_max_torque_Nm` (default 5 N·m), 15 brake (`:1451-1476`;
  `docs/reference/configuration.md:301-314`). Anything else falls back to stop (`:1473-1475`).
- The **only exit is a stop command**, then re-arm (`:1620-1622`;
  `docs/troubleshooting/timeout-mode-11.md:14`). A corenode that comes back must send mode 0 then
  mode 10; it cannot just resume sending position frames. The design report's "the next valid
  command frame re-arms the axis" (§9.1) is **wrong as written** — it needs a stop first. The
  Python `set_position_wait_complete` helper treats mode 11 as a fault for the same reason
  (`moteus.py:787-788`).
- Diagnostic-channel commands (`d pos …`) default to no timeout unless `t` is given
  (`docs/troubleshooting/timeout-mode-11.md:16`), which is why tview sessions never time out and
  scripts do.

The design report's recommendation of 30 ms and mode 12 with a tuned `timeout_max_torque_Nm`
stands; add "stop-then-re-arm" to the corenode's recovery path.

---

## 3. Calibration and startup

### 3.1 What `moteus_tool --calibrate` does

`do_checked_calibrate` (`lib/python/moteus/moteus_tool.py:1497-1714`). Preconditions: commutation
source must be slot 0 (`:1513-1516`); the motor must spin freely; the host has the diagnostic
tunnel (CAN or the UART `kSerial` path — §6.6).

| Step | What happens | What it writes |
|---|---|---|
| 0 | Clears all 64 `motor.offset.N`, sends `d stop`, sets `motor.phase_invert 0` and `motor_position.sources.0.sign 1` (`:1524-1564`) | those |
| 1 | Winding resistance by stepping voltage-FOC and measuring current (`calibrate_winding_resistance2`) | `motor.resistance_ohm` |
| 2 | Inductance via `meas_ind` mode (`calibrate_inductance`) | `motor.inductance_d_H`, then d/q separately (`calibrate_dq_inductance`, `:1614-1617`) |
| 3 | Current-loop bandwidth from R and L (`calculate_bandwidth`; default `--cal-bw-hz`) | `servo.pid_dq_hz` (`:1593-1595`) — the firmware derives the PI gains itself (`fw/bldc_servo_control.h:392-397`) |
| 4 | Encoder PLL filter bandwidth (`set_encoder_filter`) | `motor_position.sources.0.pll_filter_hz` |
| 5 | **Encoder-to-phase map**: spins one full encoder revolution each way in voltage-FOC (or current mode on newer firmware if the current-sense quality is good, `:1886-1924`), records (commanded electrical angle, encoder count), fits pole count, direction and a 64-bin offset table (`calibrate_encoder.py` via `ce.calibrate`, `:1948-1961`) | `motor.poles`, `motor_position.sources.0.sign`, `motor.phase_invert`, `motor.offset.0..63` (`:1972-1985`) |
| 6 | Kv by spinning at known voltage and measuring speed (`calibrate_kv_rating`) | `motor.Kv` → torque constant (`fw/bldc_servo.cc:313-320`) |
| 7 | Optional Ld saturation (`--cal-measure-ld-saturation`) | `motor.inductance_d_scale` |
| 8 | `d rezero`, decides `servo.voltage_mode_control` from current-sense quality (`:1638-1659`) | that flag |
| 9 | `conf write` unless `--cal-no-update`; writes a JSON report to the moteus log directory (`:1664-1714`) | flash |

Options that matter for CATBOT motors (`docs/guides/calibration.md`): `--cal-motor-power` (default
7.5 W; the GIM8108 at 0.22 Ω line-to-neutral needs checking), `--cal-force-kv` to avoid the
high-speed run on a geared joint, `--cal-bw-hz` for torque bandwidth, `--cal-invert` to choose
the positive direction, `--cal-hall` is irrelevant. The **phase-order gate** in
`docs/firmware.md` is precisely step 5: on a current-limited supply, a converging fit with a
pole count that matches the datasheet (many 8108-class outrunners are 21 pole pairs = 42; verify
for the GIM8108-8 and the 5010 before trusting the fit) is the proof that logical phases and
sensed phases pair correctly.

### 3.2 Encoder-to-rotor alignment at runtime

`electrical_theta = wrap(ratio · poles/2 · 2π / rotor_scale + lerp(motor.offset, ratio))` where
`ratio = filtered_encoder / cpr` (`fw/motor_position.h:849-862`). The 64-entry table is linearly
interpolated around the circle; a jump >3.5 rad between neighbours (an old per-bin-wrapped table)
is refused with `discontinuous_offset` (`:544-556`). The commutation source may reference the
output rather than the rotor only for integral reductions ≤1 (`:693-717`); CATBOT keeps the
AS5047 on the rotor, which is always fine.

### 3.3 What must be re-run when

| Change | Re-run |
|---|---|
| New motor, or same motor re-mounted on the encoder magnet, or magnet moved | full `--calibrate` (`docs/guides/calibration.md:3-10`) |
| Different reducer ratio | only `motor_position.rotor_to_output_ratio` (config; also read by calibration for Kv scaling, `moteus_tool.py:1536-1540`) |
| Joint re-zeroed mechanically | `--zero-offset` / `--set-offset` (`:1250-1263`), or `d cfg-set-output` + `conf write` |
| Firmware upgrade across an ABI change | nothing manual — `moteus_tool --flash` captures `conf enumerate`, migrates it through `FirmwareUpgrade.fix_config`, and restores (`:1356-1383`) |
| Board swap onto an existing joint | full calibration (the offset table is per encoder/magnet pair) |

### 3.4 What happens on a board with no encoder

- `motor_position.sources.0` defaults to the onboard SPI (`fw/motor_position.h:192-198`). With no
  AS5047 the SPI reads never become active, so `theta_valid` and `position_relative_valid` stay
  false.
- Modes 8, 9, 10, 12, 13 fault immediately: 42 `theta_invalid` or 43 `position_invalid`
  (`fw/bldc_servo_control.h:802-806`, `:825-829`, `:1205-1215`); with `motor.poles = 0`, 36
  `motor_not_configured` first.
- Modes 5, 6, 7, 15 work — which is why the open-loop bench image uses 7.
- `servo.fixed_voltage_mode` (or the per-command `0x02a/0x02c` overrides) makes mode 10 run
  open-loop like a gimbal driver: the electrical angle is synthesised from the control position
  and pole count, the encoder is ignored, reported position equals the setpoint
  (`fw/bldc_servo_control.h:303-307`, `:500-507`, `:1178-1203`;
  `docs/reference/configuration.md:115-137`). **All current limits and derating are bypassed in
  that path** (only the over-temperature fault survives). Useful once for a no-encoder motion
  test, dangerous as anything else.
- Every boot also performs the DRV8353 CSA offset calibration and the 256-sample ADC zero
  (§1.1 rows 2–4) — that is not the same as `--calibrate` and needs no encoder.

### 3.5 Other startup facts

- The AS5047 is first read 10 ms after boot; the first SPI reading is discarded
  (`fw/aux_port.h:336-354`). Absolute sources go inactive after `sources.N.timeout_s` (0.2 s)
  without a new sample (`fw/motor_position.h:1348-1354`) → fault 35 `encoder_fault` in position
  mode (`fw/bldc_servo_control.h:1211-1215`).
- I²C devices are not touched until `aux2.i2c_startup_delay_ms` (30 ms) after boot
  (`fw/aux_port.h:264-269`).
- CAN id is clamped to 1–126 (`fw/moteus.cc:285-288`); `id.id` and `can.prefix` take effect
  immediately and re-init the filters (`:281-334`).
- Config load happens once in `main` before the controller starts (`fw/moteus.cc:340`).

---

## 4. Protection and limits already implemented

### 4.1 Hard faults (mode → 1, torque off, stop required)

`errc` (`fw/error.h:25-55`); prose in `docs/protocol/registers.md:187-238`. Where each is raised:

| Code | Name | Raised where | Modes | Notes for FlexNode |
|---|---|---|---|---|
| 32 | calibration fault | ADC zero >200 counts off (`fw/bldc_servo.cc:1095-1102`) | on arming | would indicate a current-sense wiring/offset problem |
| 33 | motor driver fault | DRV nFAULT line low, or a config register read-back mismatch (`fw/drv8323.cc:453-469`); checked every cycle (`fw/bldc_servo_control.h:1646-1650`) | all active | `drv8323` telemetry and regs `0x140/0x141` say which bit; uvlo = supply sagged (`docs/troubleshooting/gate-driver-faults.md`) |
| 34 | over voltage | `bus_V > servo.max_voltage` (`:1651-1654`) | all active | default 46 V for hw 8 (`fw/bldc_servo_structs.h:509-516`); set ≈38 V for 8S so flux braking at 35 V is reachable (`docs/firmware.md:81`) |
| 35 | encoder fault | position source error (`:1211-1215`) | 10/12/13 | encoder went silent or aux error |
| 36 | motor not configured | `motor.poles == 0` (`:797-800`, `:820-823`) | 8/9/10/12/13 | before calibration |
| 37 | PWM cycle overrun | a phase was still high when the ADC sampled (`fw/bldc_servo.cc:876-882`) | all | firmware timing; watch after adding ISR work |
| 38 | over temperature | FET > `fault_temperature` (75 °C) or motor > `motor_fault_temperature` (`:1659-1667`) | all active | FET NTC 47 kΩ (`fw/bldc_servo.cc:328`); motor NTC unpopulated on v1.0 (`docs/roadmap.md:52`) |
| 39 | start outside limit | §2.5 | entering 10/13 | **defaults ±0.01 rev** |
| 40 | under voltage | `bus_V < 4 V` (`:1655-1658`) | all active | with the R30 rescale wrong this is the first thing that fires |
| 41 | config changed | encoder config epoch changed while controlling (`:772-777`) | while driving | `conf set motor_position.*` mid-motion |
| 42 | theta invalid | no commutation angle | 8/9/10/12/13 | no encoder |
| 43 | position invalid | no output position | 10/12/13 | no encoder |
| 44 | driver enable fault | DRV CSA calibration did not clear (`fw/drv8323.cc:259-268`, `fw/bldc_servo.cc:347-350`) | arming | |
| 45 | stop position deprecated | stop position + limits (`:286-292`) | command | don't use `0x026` |
| 46 | timing violation | main loop missed ≥4 ms, only if `servo.timing_fault` (`fw/moteus.cc:358-362`) | any | off by default; **turn on during bring-up** to catch a WS2812/I²C stall |
| 47 | bemf ff without accel limit | (`:294-301`) | command | only if `servo.bemf_feedforward ≠ 0` (default 0) |
| 48 | invalid limits | bounds beyond ±32768 | entering 10/13 | |
| 49 / 50 | position / velocity control error | `servo.fault_position_error` / `fault_velocity_error` finite and exceeded (`:1239-1252`) | 10/12/13 | **off by default (NaN)** — a cheap collision/stall detector for a leg; consider enabling |

Codes 1–7 are DMA/UART errors from the aux UART paths.

### 4.2 Soft limits (mode unchanged, fault register shows 96–105 while limiting)

All of these live in `ISR_DoCurrent` (`fw/bldc_servo_control.h:817-1124`) and `ISR_DoPositionCommon`
(`:1156-1368`). Which modes run them is the whole point:

| Limit | Config | Mechanism | Code | 5 pwm | 6 volt | 7 vfoc | 8 vdq | 9 cur | 10 pos | 11 tmo | 12 zero | 13 within | 14 ind | 15 brake |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| duty clamp | derived from CSA settle time | `[min_pwm, max_pwm]` (`:478-480`, `:159-161`) | — | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | (✓) | ✓ | ✓ | ✓ | — |
| max phase voltage | bus | `max_V = ratio·svpwm·bus/2`; d priority (`:1007-1022`, `:808`, `:1427-1428`) | 98 | — | — | ✓ | ✓ | ✓ | ✓ | (✓) | ✓ | ✓ | ✓ | — |
| **max current** | `servo.max_current_A` (100 A default, clamped to what the CSA can sense `fw/bldc_servo.cc:302-309`) | clamp on d and q | 99 | — | — | — | — | ✓ | ✓ | (✓) | ✓ | ✓ | — | — |
| **FET temperature derate** | `fault_temperature − temperature_margin` = 50 °C start, linear to `derate_current_A` (−20 A → 0) at 75 °C (`:878-904`) | reduces the current clamp | 100 | — | — | — | — | ✓ | ✓ | (✓) | ✓ | ✓ | — | — |
| motor temperature derate | `motor_fault_temperature` finite (`:885-893`) | same | 101 | — | — | — | — | ✓ | ✓ | (✓) | ✓ | ✓ | — | — |
| **velocity derate** | `servo.max_velocity` 500 rev/s, `+max_velocity_derate` 2 (`:861-876`) | q current to zero above | 96 | — | — | — | — | ✓ | ✓ | (✓) | ✓ | ✓ | — | — |
| **power limit** | board profile 900 W below 30 V → 400 W above 38 V (`fw/moteus_hw.cc:136-140`), `min` with `servo.max_power_W` (`fw/bldc_servo.cc:1025-1041`) | scales d and q by `max_P / used_P` (`:981-1000`) | 97 | — | — | — | — | ✓ | ✓ | (✓) | ✓ | ✓ | — | — |
| position bounds derate | `servopos.*`, `position_derate` | q toward the bound × (1 − overshoot/0.02) (`:831-859`) | 103 | — | — | — | — | ✓ | ✓ | (✓) | ✓ | ✓ | — | — |
| **max torque** | per-command `0x025` | clamp before torque→current (`:1254-1261`) | 102 | — | — | — | — | — | ✓ | (10 only) | 5 N·m cap | ✓ | — | — |
| flux braking | `max_voltage − flux_brake_margin_voltage` (3 V), virtual resistor 0.025 Ω (`:1338-1362`) | negative d current when bus rises | 104 | — | — | — | — | — | ✓ | (✓) | ✓ | ✓ | — | — |
| regen power cap | `servo.max_regen_power_W` (NaN off) (`:1305-1336`) | pre-emptive d current | — | — | — | — | — | — | ✓ | (✓) | ✓ | ✓ | — | — |
| field weakening | `servo.fw.enable` (off) (`:910-965`) | negative d above base speed | 105 | — | — | — | — | ✓ | ✓ | (✓) | ✓ | ✓ | — | — |
| control-error faults | §4.1 49/50 | | | — | — | — | — | — | ✓ | (✓) | ✓ | ✓ | — | — |
| **watchdog** | `default_timeout_s` | §2.7 | | — | — | — | — | — | ✓ | — | — | ✓ | — | — |

"(✓)" for mode 11 means "if `timeout_mode` is 10 or 12". Hard faults 33/34/38/40 apply in every
active mode (`:1646-1668`); 37 in all modes.

What this matrix says, in words:

- **The bench finding is confirmed and general:** `max_current_A` and `max_power_W` do nothing in
  5/6/7/8/14. In those modes the only protections are the voltage clamp, the DRV8353's own VDS
  over-current (`vds_lvl_mv`, default **700 mV for hw 8**, `fw/drv8323.h:303-311` — the bring-up
  notes already say set 100–200), the sense-amp over-current (`sen_lvl_mv` 500 mV, `:322`), the
  1 kHz temperature/voltage checks, and the supply. The open-loop bench image's tightened defaults
  are the right pattern.
- `servo.voltage_mode_control = 1` (the calibration heuristic can set it for low-noise
  high-resistance motors, `moteus_tool.py:1641-1659`) **keeps** all the current-side limits — it
  runs the same chain and then converts amps to volts through R (`:1104-1123`). It is the
  `fixed_voltage_mode` / `0x02a` / `0x02c` path that bypasses them (§3.4).
- Mode 15 `brake` has **no** current control — regenerative current is whatever the winding
  resistance allows; only the hard faults (33/34/38/40) still apply. On a 14:1 knee falling under gravity that may be fine;
  on a backdrivable hip it is not a controlled quantity. Evaluate on the bench before choosing it
  as the timeout fallback.
- The `derate_current_A` default is −20 A, i.e. the linear derate reaches zero current 20 A
  *before* the fault temperature... in current, not temperature: current limit =
  `frac·(−20 − 100) + 100`, hitting zero at frac = 0.83, i.e. ≈70.8 °C, then faulting at 75 °C.
  For FlexNode's FETs set `max_current_A` to the real bring-up value (25–30 A per
  `docs/firmware.md:160`) and recheck that the derate slope still makes sense.
- Power limit at 8S: the hw-8 profile interpolates 900 W at 30 V to 400 W at 38 V, so at 33.6 V
  full charge the board limit is ≈675 W and at 29.6 V nominal 900 W — an inherited r4.11 curve,
  not a FlexNode thermal fact. Set `servo.max_power_W` explicitly.

### 4.3 The gate driver

`Drv8323` (`fw/drv8323.{h,cc}`) — DRV8353 register path selected by hw ≥ 7 (`fw/drv8323.cc:275-279`).
Config `drv8323_conf.*` (`fw/drv8323.h:224-360`): gate drive currents (100/200/100/600 mA for
hw 8, inherited and untested for BSC016N06NS, `docs/roadmap.md:50`), dead time 50 ns, OCP mode
**latched fault** (`:297`), deglitch 4 µs, VDS 700 mV, CSA gain 20, sense-OCP 500 mV. Written on
every enable and verified by read-back; a mismatch sets `fault_config` and is a permanent fault 33
(`fw/drv8323.cc:396-411`, `:453-455`). Status polled every 10 ms into the `drv8323` telemetry
channel with every FSR1/FSR2 bit named (`:138-198`, `fw/drv8323.h:117-201`). The `csa_gain`
setting feeds the current-sense scale and the minimum duty-cycle window
(`CsaSettlingTime`, `fw/drv8323.h:58-67`; `fw/bldc_servo.cc:280-309`).

---

## 5. The aux port subsystem

### 5.1 What FlexNode's two ports physically offer to the firmware

The family-0 hardware tables are stock r4.11 and were not edited for FlexNode
(`fw/moteus_controller.cc:401-497`; `notes/hardware-io.md` gotcha). Per pin — the columns are the
capabilities the table grants; a mode not granted returns an `aux?.error` and disables the port:

| Port.pin | MCU pin | ADC | I²C | SPI | UART | Timer | Modes available (`fw/aux_common.h:476-499`) |
|---|---|---|---|---|---|---|---|
| aux1.0 | PC13 | — | — | — | — | — | digital_in, digital_out, quad_sw, hall, index, **pwm_in** (EXTI, `fw/aux_mbed.h:346-420`), spi_cs |
| aux1.1 | PB13 | ADC3 ch5 | — | SPI2 SCK | — | — | + analog_in, sine/cosine, spi |
| aux1.2 | PB14 | ADC1 ch5 | — | SPI2 MISO | — | — | same |
| aux1.3 | PB15 | ADC2 ch15 | — | SPI2 MOSI | — | — | same |
| aux2.0 | PB8 | — | I2C1 SCL | — | USART3 | — | i2c, uart, gpio family |
| aux2.1 | PB9 | — | I2C1 SDA | — | USART3 | — | i2c, uart, gpio family |
| aux2.2 | PC14 | — | — | — | — | — | gpio family, pwm_in |
| aux2.3 | PC15 | — | — | — | — | — | gpio family, pwm_in |

Hard consequences of that table:

- **No hardware PWM output on either port.** `kPwmOutput` needs a timer in the table
  (`fw/aux_port.h:1442-1461`); every FlexNode entry has `nullptr`, so `aux1.pins.0.mode 17` yields
  `pwm_pin_error`. The servo pulse is not free (see §5.4).
- **No hardware quadrature** (same reason). Software quadrature on any pin works.
- **Analog input only on PB13/14/15.** PB11 (5 V rail sense) is in no table; adding a fifth aux1
  row `{4, PB_11, adc, ch}` is a one-line change (the ADC index is 0/1/2 = ADC1/2/3,
  `fw/aux_adc.h:32`, `:58-60`; PB11 is ADC1/2 channel 14 on the G474 — verify against the
  datasheet). Analog values are sampled every control cycle via injected channels and appear as
  0–1 in `aux?.analog_inputs` and registers `0x060–0x06c`.
- **I²C only on aux2**, and it is one bus for everything (IMU, ToF, joint encoder, load-cell ADC).
  Up to three configured devices (`fw/aux_common.h:397`), polled round-robin one transaction per
  control cycle when the bus is idle (`fw/aux_port.h:752-850`), minimum `poll_rate_us` 100
  (`:979-981`), 400 kHz fast mode default with fast-mode-plus selectable
  (`fw/aux_common.h:392-394`).
- **UART only on aux2 (PB8/PB9)** — and it shares the pins with I²C, so it is either/or.
- **SPI only on aux1 PB13/14/15 = SPI2**, shared with the onboard encoder (see §5.2).
- The digital/PWM/analog register interface is stock: `0x05c–0x05f` GPIO, `0x060–0x06c` analog,
  `0x072–0x075` PWM-in period/duty, `0x076–0x07f` PWM-out (`fw/moteus_controller.cc:315-349`).
  GPIO outputs are latched in the ISR each cycle (`fw/aux_port.h:201-211`).

### 5.2 The onboard encoder's bus — a discrepancy to settle

`aux1` is constructed with `kDefaultOnboardSpi` (`fw/moteus_controller.cc:518-520`). With the
pins left at board-default (→ NC), `FindSpiOption(..., kDoNotRequireCs)` selects SCK/MISO/MOSI
from the hardware table wherever the pin is NC (`fw/aux_mbed.h:604-605`), i.e. **PB13/PB14/PB15 on
SPI2**, with CS from `g_hw_pins.as5047_cs` = PC6 (`fw/aux_port.h:1157-1160`, `fw/moteus_hw.cc:128`).
Upstream documents the same: the onboard encoder "claims the CLK, MOSI, and MISO pins on
auxiliary port 1" (`docs/reference/encoders.md:188-190`). PA5/6/7 (SPI1) are the DRV8353's
bit-banged SPI (`fw/moteus_hw.cc:108-110`).

`notes/hardware-io.md` says the rotor encoder is on "SPI1"; `docs/can-layer.md` §9.2 and
`notes/open-questions.md` §2 plan to use PB13/14/15 as TIM1 outputs for the SimpleFOC Mini. If the
FlexNode PCB wires the AS5047 the way the unchanged firmware expects, **those pads are the rotor
encoder's bus and cannot become the Mini's PWM.** If the PCB wires it elsewhere, the firmware
does not know and the encoder will not work until the table is changed. Either way the netlist
decides; this must be checked before the AS5047 is soldered and before any TIM1 plan proceeds.

### 5.3 Drivers already in the tree

| Class | Devices / modes | Where | Status on FlexNode |
|---|---|---|---|
| SPI encoders | onboard AS5047, external AS5047, MA732, MA600, iC-PZ, CUI AMT22, RLS Orbis (`fw/aux_common.h:34-47`) | `fw/as5047.h` etc., `fw/aux_port.h:1146-1251` | onboard AS5047 is the rotor encoder; others need the SPI2 pads |
| I²C devices | AS5048, AS5600, **LSM6DS3 (FlexNode)** (`fw/aux_common.h:370-378`) | `fw/aux_port.h:671-850` | IMU verified; AS5600 driver present and unexercised |
| UART modes | AksIM-2, tunnel, debug stream, CUI AMT21, **`serial` = full register protocol** (`fw/aux_common.h:98-108`) | `fw/aux_port.h:1359-1420`, `fw/uart_fdcanusb_micro_server.h` | aux2 only; excludes I²C while active |
| Incremental | software/hardware quadrature, hall ×3, index, PWM-in | `fw/aux_mbed.h` | hall/index/sw-quad/pwm-in usable on PC13/PC14/PC15 |
| Analog | analog_in, sine/cosine | `fw/aux_port.h:1468-1508` | PB13/14/15 only |
| BiSS-C | RS422 encoders | `fw/bissc.h` | needs a UART-TX+timer pin: not available on FlexNode |
| GPIO | digital in/out | | any pin |
| PWM out | | | **unavailable** (no timer in table) |

Encoder sources (`motor_position.sources.N.type`: spi, uart, quadrature, hall, index,
sine_cosine, i2c, bissc; `sensorless` is an enum value that asserts if used,
`fw/motor_position.h:1253-1256`, `docs/reference/configuration.md:536`). Three source slots,
each with its own PLL velocity filter (`pll_filter_hz`, capped at ¼ of the device's poll rate
for I²C/UART, `:632-643`), CPR/offset/sign/compensation table, and rotor/output reference.

### 5.4 Mapping onto the CATBOT peripheral list

Against `docs/can-layer.md` §3.2 and the per-node assignment in `notes/hardware-io.md`:

| CATBOT peripheral | Channel type | What moteus already provides | Verdict |
|---|---|---|---|
| **Onboard BLDC** | axis 0 | everything in §1–§4 | free (pending hardware validation) |
| **IMU** LSM6DS3TR-C | `imu` 0x40 | FlexNode driver on aux2 device 0, regs 0x098–0x09F, verified | done |
| **WS2812 pixels** | `pixel` 0x50 | FlexNode driver, regs 0x0B0–0x0B4, `led.*` config, per-pixel `SetPixel()` hook (`fw/ws2812_led.h:127`) | done; per-pixel register missing |
| **Joint output encoder** AS5600 / AS5600L / MT6701 | `enc_i2c` 0x20 | AS5600 I²C driver (`fw/aux_port.h:739-750`, `:772-788`, `:826-828`); as a `motor_position` source it can be the **output reference** for boot-time absolute position (§2.6) and is readable live via regs `0x052/0x053` (`fw/moteus_controller.cc:1156-1161`, source slot 1) — position and velocity, with `0x058` validity bits. AGC/magnet diagnostics in `aux2.i2c.devices.N` | **free for the primary use** (absolute joint angle + PLL velocity). Small work: AS5600L programmable address (it is just `address`), MT6701 is a different register map → small driver. The channel-frame view (`0x2n0…`) is the gap, not the sensor |
| **ToF** VL53L7CX/L5CX | `tof` 0x41 | the I²C engine and a tunnel stream to carry the 84 KB blob (§6.5); no device driver | **real work**: multi-hundred-byte I²C transactions vs. the 14-byte engine buffer (`fw/aux_port.h:1672`), a state machine for firmware upload, and the PB10 interrupt |
| **Load cell** via NAU7802 (I²C) | `loadcell` 0x30 | I²C engine; nothing else | small driver (new `DeviceConfig::Type`, three switch cases per `notes/firmware-map.md`), plus on-node tare/threshold |
| **Load cell** via HX711 / ADS1220 | | HX711 needs two bit-banged GPIOs (available on PC14/PC15 but the timing would run from the main loop); ADS1220 needs SPI2 (shared with the AS5047, §5.2) | avoid; NAU7802 is the right call as the notes already say |
| **Analog load-cell amplifier** | `adc_in` 0x31 | analog_in on PB13/14/15, sampled at control rate, registers exist | free if the amplifier is analog and lands on those pins |
| **RC servo** DS3235/DS3230 | `servo_rc` 0x10 | nothing usable: no timer in the aux table, so no PWM-out; PC13 can only be GPIO | **small-to-medium driver**: either add TIM8_CH4N to the PC13 table row and teach `Stm32Pwm` complementary-output enable (`notes/hardware-io.md` constraints), or a main-loop software pulse. The channel's slew limiting and encoder binding are new code |
| **5 V rail current** PB11 | `rail_5v` 0x70 | ADC engine; PB11 not in table | one table row (§5.1) + reg 0x082 |
| **SimpleFOC Mini** | `bldc_ext` 0x11 | nothing — a second commutation path on TIM1 is new code, and §5.2 may take the pins away | **real work**, blocked on hardware questions |
| **Servo feedback encoder** | bound `enc_i2c` | same AS5600 driver on the same bus | free as a reading; the binding logic is new |
| **Broadcast to all nodes** | fleet | filter accepts destination `0x7f` (`fw/moteus.cc:307-323`) | free |
| **Node name** 0x0C0 | fleet | nothing (UUID at `0x150–0x153` is the pattern to copy, `fw/moteus_controller.cc:1293-1304`) | small |

One further free item: **PWM input** on any GPIO pin (`pwm_in`, one per port,
`fw/aux_port.h:1509-1521`) measures period and duty from 10 µs to 65 ms — usable for a fan
tach, a hobby-servo feedback wire, or a contact sensor with a PWM output, with zero new code.

---

## 6. The register protocol and tooling you inherit

### 6.1 Protocol (unchanged, so only the essentials)

CAN-FD 1/5 Mbit with the moteus multiplex subframe format; already worked through in
`docs/can-layer.md` §1 and `2026-09-08-can-subsystem-design.md` §3. Two facts from the source
worth restating: the `0x000` mode write must be the first subframe in a command
(`docs/protocol/registers.md:99-100`) because it resets the command struct; and the frame is
either accepted whole or discarded whole — a UUID-mask mismatch discards the rest of the frame
(`fw/moteus_controller.cc:624-635`, `:852-868`), which is the mechanism for addressing one board
among several with the same id. Upstream recommends a 0.666 sample point and large SJW
(`docs/integration/can-fd.md:7`); the design report's §2.4/§2.5 recommendations (later sample
point, TDC on) are a deliberate deviation for FlexNode's transceiver and should be applied to
both ends.

### 6.2 Register map areas (`fw/moteus_controller.cc:235-399`)

| Range | Content | Use on CATBOT |
|---|---|---|
| `0x000–0x00f` | mode, position, velocity, torque, q/d current, abs position (source 1), power, motor temp, trajectory complete, home state, voltage, FET temp, fault | fast lane `0x000–0x003`; slow lane `0x00d–0x00f` |
| `0x010–0x01e` | raw-mode commands (pwm, voltage, vfoc, vdq, current) | bench |
| `0x020–0x02d` | position-mode command (§2.1) | fast lane `0x020–0x022`; slow lane gains/limits |
| `0x030–0x03d` | PID term breakdown, control position/velocity/torque, errors | tuning / diagnostics |
| `0x040–0x048` | stay-within command | optional |
| `0x050–0x058` | encoder 0/1/2 position and velocity, validity bits | joint output encoder readback |
| `0x05c–0x07f` | aux GPIO, analog, ms counter, clock trim, PWM in/out | contact switch, rail sense |
| `0x080–0x0ff` | **FlexNode block** (v1) | as specified |
| `0x100–0x158` | model, firmware version, map version, serial, set-output/reindex/recapture, driver faults, UUID and mask | enumeration |

### 6.3 Python library (`lib/python/moteus/`)

`moteus.Controller` builds and parses frames; `make_*` returns a frame, `set_*` sends it and
returns the reply (`moteus.py:433-1201`): `query`, `custom_query`, `stop`, `zero_velocity`,
`set_output_nearest/exact`, `rezero`, `require_reindex`, `recapture_position_velocity`,
`position`, `position_wait_complete`, `vfoc`, `current`, `stay_within`, `brake`, `write_gpio`,
`read_gpio`, `diagnostic_write/read`, `set_trim`, `aux_pwm`. Only the fields you pass are written
(`:683-698`) — so `set_position(position=, velocity=, feedforward_torque=)` is byte-identical to
the class-A fast-lane command. The default query is mode/position/velocity/torque as float plus
voltage/temperature/fault as int8 (`:69-101`); `QueryResolution._extra` adds arbitrary registers,
which is how the FlexNode block is read from Python without touching the library.
`moteus.Stream` wraps the diagnostic channel (`:1249-1430`): `command`, `read_data(channel)`
returns a telemetry struct as a named tuple by fetching the binary schema.
Transports: fdcanusb, python-can/socketcan, and a UART device (`transport_factory.py`,
`fdcanusb.py`, `pythoncan.py`). Flashing is refused over UART (`moteus_tool.py:1325-1328`).

A C++ header-only client with the same surface lives in `lib/cpp/mjbots/moteus/` — the
corenode's frame templates (design report §7.4) can be lifted straight from
`moteus_multiplex.h` and `moteus_protocol.h` rather than written from the spec.

### 6.4 `moteus_tool` and `tview`

`moteus_tool` actions (`moteus_tool.py:2797-2950`): `--info` (model, firmware, serial, UUID),
`--console`, `--dump-config` / `--restore-config` / `--write-config`, `--read CHAN`,
`--flash ELF`, `--calibrate` and its `--cal-*` family, `--zero-offset` / `--set-offset`, `--stop`,
`--can-prefix`, transport selection. `tview` (`utils/tview.py`, `utils/gui/moteus_gui`) shows the
config tree and every telemetry channel live with plotting, and its console speaks the
diagnostic protocol with `ID>` prefixes, `&&` sequencing, `:ms` delays and `?` trajectory waits
(`docs/reference/client-tools.md:3-66`) — a ten-node fleet can be exercised from one window
(`A>d stop`). `utils/decode_can_frame.py` decodes any hex frame into named registers
(`docs/integration/can-fd.md:11-33`); `utils/fdcanusb_capture.py` sniffs the bus. Also in
`utils/`: `compensate_cogging.py` / `compensate_encoder.py` (fill `motor.cogging_dq_comp` and the
source compensation table), `measure_inertia.py`, `encoder_bandwidth.py`, `clock_cal.py`.

### 6.5 The diagnostic tunnel

`multiplex_protocol` provides three tunnel streams (`fw/moteus.cc:232-238`): channel 1 is the
text console (`conf`, `tel`, `d`, `aux1/2` commands, `docs/protocol/diagnostic.md`); channels 2
and 3 are wired to aux1's and aux2's UART when in `tunnel` mode (`fw/moteus_controller.cc:516`,
`:529`; `fw/aux_port.h:861-883`), with 64-byte buffers each way (`:1677-1685`). The ToF blob
plan in `docs/can-layer.md` §9.4 would need a **fourth** stream or a new command on channel 1
that forwards to I²C — the existing UART tunnel does not reach the I²C engine. Throughput is
bounded by the diagnostic read/write frames (48-byte reads by default, `moteus.py:1068`), which
is the "bench-validate before committing" item the spec already carries.

### 6.6 Configuration persistence

`PersistentConfig` (mjlib) with groups `id`, `can`, `uuid`, `servo`, `servopos`, `motor`,
`motor_position`, `aux1`, `aux2`, `drv8323_conf`, `led`, plus anything FlexNode registers
(`fw/moteus.cc:336-338`, `fw/bldc_servo.cc:182-185`, `fw/motor_position.h:376-381`,
`fw/aux_port.h:107`, `fw/drv8323.cc:72`). Semantics (`docs/protocol/diagnostic.md:340-384`):
`conf set` changes RAM and takes effect immediately through the group's callback (which can
re-init a subsystem — an aux change bumps the encoder epoch and, if driving, causes fault 41);
`conf write` commits everything to the flash page at `0x0807f000`; `conf load` reloads; `conf
default` resets RAM. `moteus_tool --flash` snapshots and restores config across a firmware
upgrade, applying ABI migrations (`moteus_tool.py:1356-1383`). **Not yet exercised on FlexNode**
(`notes/bringup-log.md` gates).

The `id` group (`id.id`) is the CAN address; `uuid` is per-silicon (`fw/uuid.cc`); the UUID
mask registers let one command frame target a board by UUID regardless of id.

### 6.7 The CAN bootloader

Resident at `0x0800c000` (`docs/firmware.md:95`, `fw/stm32g474_bootloader.ld`), entered from the
application by `d flash` (`fw/board_debug.cc:975`), then driven by `moteus_tool --flash` over the
same diagnostic tunnel with `unlock` / `w addr hex` / `r addr len` / `lock` / `reset`
(`moteus_tool.py:1370-1424`, `fw/can_bootloader.cc`). It uses TIM5 as its time base
(`can_bootloader.cc:72-96`). Fleet reflash of ten nodes is therefore a loop over ids with no
SWD cable — once one node has been flashed over SWD with both images (`fw/flash.py`).
`--bootloader-active` recovers a board stuck in the bootloader (`:1339-1345`).

### 6.8 The serial escape hatch

`aux2.uart.mode = serial` (5) turns PB8/PB9 into a `UartFdcanusbMicroServer` speaking the
fdcanusb ASCII framing of the same register protocol (`fw/aux_common.h:104`,
`fw/aux_port.h:1404-1411`, `fw/multi_transport_datagram_server.h`), at 921600 baud by default
(`fw/aux_common.h:26`). `notes/hardware-io.md` already identifies this as the way to run
`moteus_tool` on FlexNode before a CAN adapter arrives; the cost is the IMU while the port is a
UART, and no `--flash`. It is stock and already compiled in.

---

## 7. Telemetry and introspection

### 7.1 The mechanism

`TelemetryManager` (mjlib) over diagnostic channel 1: `tel list`, `tel schema <ch>` (binary
schema), `tel get`, `tel rate <ch> <ms>` for periodic emission, `tel fmt`/`tel text`
(`docs/protocol/diagnostic.md:282-338`). Python `Stream.read_data("servo_stats")` does schema +
get in one call. tview subscribes to everything at a few Hz and plots any leaf. Output buffer is
2 KB (`fw/moteus.cc:244`), so a `tel rate` on a large struct at a high rate will drop.

### 7.2 Registered channels

| Channel | Struct | What is in it | Registered at |
|---|---|---|---|
| `servo_stats` | `BldcServoStatus` (`fw/bldc_servo_structs.h:146-328`) | mode, fault, raw ADCs and offsets, phase currents, electrical theta, bus_V (raw + two filters), FET/motor temp, d/q current, position, velocity, torque, power, filtered velocity, all three PID states, control position/velocity/acceleration, timeout countdown, trajectory_done, max/base velocity, max_power_W, effective_max_current_A, field-weakening, torque error, ISR timing (`final_timer`/`total_timer` = ISR duty) | `fw/bldc_servo.cc:186` |
| `servo_cmd` | `BldcServoCommandData` | the last accepted command after default resolution | `:187` |
| `servo_control` | `BldcServoControl_Control` | pwm ×3, voltage ×3, d/q V, d/q A commanded, cogging comp, torque | `:188` |
| `motor_position` | `MotorPosition::Status` (`fw/motor_position.h:284-366`) | per-source raw/compensated/filtered value, velocity, active flags, nonce; relative/absolute position, homed state, theta_valid, electrical_theta, error | `:382` |
| `aux1`, `aux2` | `AuxStatus` (`fw/aux_common.h:572-614`) | error, spi/uart/quadrature/hall/index/sine-cosine/pwm-in/bissc status, I²C devices ×3 + **`imu`**, GPIO bits, analog ×5, pwm ×5, epoch | `fw/aux_port.h:109` |
| `ic_pz1`, `ic_pz2` | iC-PZ status | unused on FlexNode (space is still allocated) | `:115` |
| `drv8323` | `Drv8323::Status` | every DRV8353 fault bit, fault line, power/enable state, config verify mask, counters | `fw/drv8323.cc:74` |
| `system_info` | (`fw/system_info.cc:29-52`) | `pool_size`/`pool_available`, **`idle_rate`** (main-loop iterations per 10 ms — the only CPU-headroom metric), `can_reset_count`, `ms_count` (starts near 2³¹ on purpose, `:39`), flash `mem_error` | `:59` |
| `firmware` | (`fw/firmware_info.cc:23-38`) | ABI version (0x010100), 96-bit serial from `0x1fff7590`, model (0), family, hwrev | `:63` |
| `git` | `GitInfo` | build commit hash and dirty flag | `fw/moteus.cc:274` |
| `board_debug` | | debug-command state | `fw/board_debug.cc:138` |
| `led` | `Ws2812Led::Status` (`fw/ws2812_led.h:94-113`) | frames, mode, pixels, fault code shown, pixel-0 colour | `fw/ws2812_led.cc:93` |

`firmware` is what `moteus_tool --info` prints and what the fleet manifest should assert; the
FlexNode block's proposed `0x083/0x084` profile/build-hash registers can be backed by `git`.

### 7.3 Live observability that costs nothing

- **Which limit is active right now**: register `0x00f` shows 96–105 while in a control mode
  (`fw/bldc_servo_control.h:1063-1066`, `:1258-1261`, `:1360-1362`). Reading it on the slow lane
  tells the corenode whether a joint is current-, power-, thermally- or torque-limited.
- **Encoder health**: `0x058` validity bits, `motor_position.sources[N].active_*`, I²C
  `error_count`, AS5600 `ams_diag`/`ams_agc`.
- **ISR headroom**: `servo_stats.final_timer / total_timer`; **main-loop headroom**:
  `system_info.idle_rate`.
- **Per-cycle debug stream**: `servo.emit_debug` bitmask streams selected quantities at control
  rate out a debug UART (`fw/bldc_servo.cc:1110-1243`) — on FlexNode only if aux2's UART is set
  to `debug` mode (`fw/aux_port.h:432-437`), which again displaces I²C.
- `MOTEUS_PERFORMANCE_MEASURE` DWT stage timings exist but are compiled out
  (`fw/bldc_servo_structs.h:221-252`).

---

## 8. The honest gap list

Cross-referenced to `docs/can-layer.md` (§3 channel model, §4 node block, §6 profiles, §7
host-loss, §9 hardware gaps, §11 build order) and the design report. Ranked by effort; "S" is a
day, "M" a week, "L" longer or blocked on a decision.

| # | Gap | Spec ref | Effort | Why it is not free / what to reuse |
|---|---|---|---|---|
| 1 | Bring-up config set: `servopos.position_min/max`, `servo.default_accel_limit nan`, `servo.max_current_A`, `max_power_W`, `max_voltage 38`, `default_timeout_s 0.03`, `drv8323_conf.vds_lvl_mv`, `servo.timing_fault 1` | design §9.1, `docs/firmware.md:160` | S (config only) | nothing to write; write it down per node and keep it in `tools/bench/` as a `--write-config` file |
| 2 | Node name `0x0C0–0x0C3` and `flexnode.*` config group | §4.1 | S | copy the UUID register pattern |
| 3 | 5 V rail sense: aux1 table row for PB11 + reg `0x082` | §4, §5.4 | S | the ADC engine and register scaffolding exist |
| 4 | AS5600 as output reference for boot-time absolute joint angle | §3.2 `enc_i2c`, §11 step 5 | S (config) | **already implemented**; needs the encoder in hand and `aux2.i2c.devices.1` + `motor_position.sources.1` + `output.reference_source 1`. Do this before writing the channel driver — it is the payoff |
| 5 | Channel infrastructure: slot registers `0x200+`, type/caps/status/mode/nonce dispatch, `0x085` highest slot, `0x0FF → 2` | §3.1–3.3, §11 step 4 | M | pure register plumbing over existing status structs; no new drivers needed for `imu`, `pixel`, `enc_i2c`, `adc_in`, `gpio`, `rail_5v` |
| 6 | Load cell: NAU7802 I²C device type + tare/threshold + `loadcell` channel | §3.4, §11 step 6 | S–M | new device type in the existing engine (three switch cases); contact flag is node-side logic |
| 7 | RC servo output on PC13: TIM8_CH4N table entry + complementary-output enable in `Stm32Pwm`, or a main-loop software pulse; `servo_rc` slew/hold/timeout semantics | §3.4, §9.1, §11 step 7, design §9.1 | M | no PWM-out on any FlexNode aux pin today; the register path (`0x076`) exists once a timer does |
| 8 | Per-channel timeout actions (G0 for channels) | §7 G0, design §9.1 | S–M | the axis watchdog exists; channels need their own hold/release timers |
| 9 | Corenode re-arm sequence: **stop then position** after timeout | design §9.1 (correction) | S (corenode side) | firmware is correct as is; the spec text is wrong |
| 10 | TDC enable + `can.*` bit-timing override exposure | §9.5 | S | `ApplyRateOverride` exists; two lines plus a config field |
| 11 | Profiles / `flexnode_profile.h` / `0x083–0x084` | §6 | M | off CATBOT's path; `git` telemetry can back the build hash immediately |
| 12 | G1 fleet-state broadcast handling (`0x086/0x087`) | §7 G1, design §9.3 | S–M | broadcast reception is free; the state machine is new |
| 13 | ToF: driver, firmware-blob upload path, PB10 interrupt, `tof` channel | §3.4, §9.4 | L | I²C engine's 14-byte buffer and one-transaction-per-cycle model do not fit a 84 KB upload or multi-hundred-byte frames; needs its own I²C state machine (still on the same `Stm32I2c`) and a tunnel or command channel that reaches it |
| 14 | SimpleFOC Mini (`bldc_ext`): second commutation path on TIM1, software sin/cos, `enc_i2c` closure | §3.2, §9.2, design §11 | L, **blocked** | new code that must not touch `bldc_servo`; hardware questions (pad breakout, and §5.2's SPI2 conflict) come first |
| 15 | G2 deputy, time sync | §7 G2, "Time sync" | — | spec says do not build for CATBOT; nothing in moteus helps or hinders |
| 16 | IMU re-init after bus fault; configurable ODR/full-scale | `notes/firmware-map.md` | S | FlexNode driver, not moteus |
| 17 | Per-pixel register for `SetPixel()` | `notes/open-questions.md` §6 | S | hook exists |

Things the earlier notes list as gaps that are **not** gaps:

- "No CPU instrumentation" — `system_info.idle_rate` and `servo_stats.final_timer/total_timer`
  exist (§7.3).
- "Servo needs a PWM expander" — withdrawn already; and PWM-in for a feedback wire is free.
- "Encoder-2 fast-lane path for AS5600L" — registers `0x052/0x053` already publish source 1 at
  int16 in one subframe; the channel view is optional for the fast lane.
- "Host must home the joints" — `output.reference_source` removes that once the AS5600 is fitted.

Corrections to existing notes surfaced by this inventory, for the coordinator to fold in:

1. `notes/hardware-io.md`: rotor encoder bus is SPI2 (PB13/14/15) per firmware, not SPI1 — verify
   against the netlist; the §9.2 TIM1 plan depends on it (§5.2).
2. `2026-09-08-can-subsystem-design.md` §9.1: recovery from timeout needs a stop command before
   the next position command (§2.7).
3. `notes/hardware-io.md` / `docs/can-layer.md` §9.1: "software timing is a fallback" — today the
   firmware offers **no** PWM output on PC13 at all; both options are new code (§5.1).
4. Bring-up config: add `servo.default_accel_limit nan` and real `servopos` limits to the list in
   `docs/firmware.md:160` (§2.4, §2.5).

---

## Appendix A — where things live

| Topic | File |
|---|---|
| modes, command, config, status structs | `fw/bldc_servo_structs.h` |
| control law, limits, mode machine | `fw/bldc_servo_control.h` (header-only template) |
| trajectory planner | `fw/bldc_servo_position.h` |
| ISR, ADC, PWM timer, DRV enable sequence | `fw/bldc_servo.cc` |
| PID | `fw/pid.h`; current PI `fw/simple_pi.h` |
| encoder sources, commutation angle, output position, homing | `fw/motor_position.h` |
| register map and aux hardware tables | `fw/moteus_controller.cc` |
| aux port engine | `fw/aux_port.h`, `fw/aux_common.h`, `fw/aux_mbed.h`, `fw/aux_adc.h` |
| gate driver | `fw/drv8323.{h,cc}` |
| CAN, filters, main loop | `fw/moteus.cc`, `fw/fdcan.{h,cc}` |
| bootloader | `fw/can_bootloader.cc`, `fw/stm32g474_bootloader.ld` |
| FlexNode deltas | `fw/moteus_hw.cc`, `fw/ws2812_led.{h,cc}`, IMU cases in `fw/aux_port.h` |
| upstream reference docs | `docs/protocol/registers.md`, `docs/reference/configuration.md`, `docs/protocol/diagnostic.md`, `docs/reference/encoders.md`, `docs/guides/control-modes.md`, `docs/guides/calibration.md` |
| host tools | `lib/python/moteus/{moteus,moteus_tool,calibrate_encoder}.py`, `utils/tview.py`, `utils/decode_can_frame.py`, `lib/cpp/mjbots/moteus/` |

## Appendix B — config keys CATBOT touches, with defaults

| Key | Default (hw 8) | Source | CATBOT |
|---|---|---|---|
| `id.id` | 1 | `fw/moteus.cc:285-288` | 1–10 |
| `servopos.position_min / max` | ±0.01 rev | `fw/bldc_servo_structs.h:744-745` | per joint or `nan` |
| `servo.pid_position.kp / kd / ki / ilimit` | 4 / 0.05 / 1 / 0 | `:674-680` | tune |
| `servo.default_velocity_limit / default_accel_limit` | nan / **50** | `:593-594` | nan / nan |
| `servo.default_timeout_s` | 0.1 | `:612` | 0.03 |
| `servo.timeout_mode / timeout_max_torque_Nm` | 12 / 5 | `:613-622` | 12, tune cap |
| `servo.max_current_A` | 100 | `:639-644` | 25–30 at bring-up |
| `servo.max_power_W` | nan (board profile) | `:520` | set explicitly |
| `servo.max_voltage` | 46 | `:509-516` | 38 |
| `servo.flux_brake_margin_voltage` | 3 | `:626` | keep |
| `servo.fault_temperature / temperature_margin` | 75 / 25 | `:529-534` | keep, verify NTC |
| `servo.fault_position_error / fault_velocity_error` | nan | `:544-545` | consider |
| `servo.max_position_slip / max_velocity_slip` | nan | `:609-610` | consider |
| `servo.timing_fault` | 0 | `:668` | 1 during bring-up |
| `servo.pwm_rate_hz` | 30000 | `:498-502` | keep |
| `motor_position.rotor_to_output_ratio` | 1 | `fw/motor_position.h:186` | 1/8 hips, 1/14 knees (verify) |
| `motor_position.output.reference_source` | −1 | `:172` | 1 once an AS5600 is on the joint |
| `motor_position.sources.1.*` | none | `:41-146` | `aux_number 2, type i2c, i2c_device 1, cpr 4096, reference output` |
| `aux2.i2c.devices.1.type / address / poll_rate_us` | none | `fw/aux_common.h:369-389` | AS5600 `0x36` |
| `drv8323_conf.vds_lvl_mv` | 700 | `fw/drv8323.h:303-311` | 100–200 |
| `led.*` | see `docs/firmware.md` | `fw/ws2812_led.h:59-92` | as wanted |
