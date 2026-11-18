# Chamber controller wiring reference

The verified as-built wiring is shown in `chamber_controller_schematic.drawio` and its checked PDF. The companion diagram `chamber_controller_wiring.drawio` predates the heater relay replacement; do not use its G3MB SSR control wiring. The verified as-built schematic is authoritative for the replacement 3.3 V relay module.

- `01 Low Voltage and Signals`
- `02 110V Heater Safety`

## U1 top-port and ESP32 wiring

| U1 pin / signal | Connection | Component | ESP32 / load | Notes |
|---|---|---|---|---|
| Pin 1 / PA9 inner PWM | Pin 1 -> 10 kΩ series | R1 | GPIO 27 | Test-rig wiring; monitor only |
| Pin 2 / GND | Common DC ground | - | ESP32 GND, fan returns | Required shared reference |
| Pin 3 / PA6 | Inner-fan tachometer | - | Nothing | Leave N/C; do not connect or simulate tach |
| Pin 4 / PA7 / FLAG2 | Purifier presence | 1 kΩ to Pin 2 | - | Follow the verified schematic |
| Pin 5 / PA8 exhaust PWM | Direct PWM signal | - | Noctua blue PWM wire and GPIO 26 monitor | GPIO monitor uses R3 13 kΩ series |
| GPIO 26 node | GPIO 26 -> 22 kΩ -> GND | R4 | Common GND | Divider return |
| Pin 6 | Fused U1 +24 V | - | Noctua red supply only | Do not use Pin 6 to power other loads |

## U1 onboard 24 V and 5 V wiring

| Source | Destination | Connection |
|---|---|---|
| U1 onboard 24 V supply | Heater-box 24 V bus | Powers the assembly as shown in the verified schematic |
| U1 Pin 6 | Noctua exhaust fan | Fused +24 V supply for the fan only |
| Heater-box 24 V bus | MP1584 input | IN+ / IN- |
| MP1584 output | ESP32 | +5 V to VIN, GND to common ground |
| MP1584 output | Servo | +5 V / GND |
| MP1584 output | Heater relay module control-side DC+ | +5 V as shown in the verified schematic |
| Common ground | Heater relay module DC- | Common ground as shown in the verified schematic |
| ESP32 3V3 | Heater relay module VCC | 3.3 V relay/module supply as shown in the verified schematic |
| GPIO 25 | Heater relay module IN | Active-high control: HIGH = relay ON, LOW = relay OFF |
| Heater-box 24 V bus | 7530 blower MOSFET supply | MOSFET DC+ |
| Heater-box 24 V bus | AEDICO relay coil DC+ | Coil supply |

## Loads

| Load | Positive / supply | Return | Control |
|---|---|---|---|
| Noctua 24 V PWM | U1 Pin 6, fused +24 V | Common GND | Blue wire from U1 Pin 5; green tach N/C |
| 7530 blower | U1 onboard 24 V through MOSFET OUT+ | MOSFET OUT- switched return | GPIO 23 |
| MG90S servo | +5 V buck output | Common GND | GPIO 18 |
| AEDICO relay coil | U1 onboard 24 V | MOSFET OUT- switched return | Energized only when GPIO 23 runs blower |
| Heater | 110 V live through safety chain | Neutral bus | Replacement relay-module contacts, GPIO 25 control, and AEDICO interlock contact |

## 110 V live safety chain

```text
AC Live
  -> KSD301 70°C NC thermostat
  -> AEDIKO relay COM/NO
  -> replacement relay-module contacts
  -> 121°C thermal fuse
  -> PTC heater live terminal
```

Neutral goes directly to the heater and pilot-light return. Protective earth bonds to the enclosure, not to the DC signal ground unless the power-supply safety design explicitly requires it.

## Safety notes

- Keep the 110 V zone physically separated from the ESP32, U1, and fan-control wiring.
- U1 Pin 6 supplies the Noctua exhaust fan only.
- The R1 10 kΩ-only Pin 1 connection is the tested rig connection, not a confirmed 5 V-to-3.3 V level shifter. Measure GPIO 27 before permanent installation.
- R3 is the 13 kΩ series resistor for the GPIO 26 exhaust monitor. The separate 15 kΩ component shown in the verified PDF is not R3.
- The AEDICO relay coil is powered from the U1 onboard 24 V bus, but its return is the MOSFET-switched blower return. Therefore the relay should de-energize whenever GPIO23 turns the blower off. Verify the relay module accepts this switched-return arrangement and that the MOSFET can sink the combined blower and relay-coil current.
- GPIO 25 is active-high for the replacement heater relay module (HIGH = ON, LOW = OFF). Keep its VCC/control supply wiring separate from GPIO 25 and follow the verified schematic. Confirm the module contact rating is suitable for the heater current before mains operation.
