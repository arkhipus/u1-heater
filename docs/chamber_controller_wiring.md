# Chamber controller wiring reference

The original all-in-one schematic remains in `chamber_controller_schematic.drawio`. The companion diagrams are in `chamber_controller_wiring.drawio`:

- `01 Low Voltage and Signals`
- `02 110V Heater Safety`

## U1 top-port and ESP32 wiring

| U1 pin / signal | Connection | Component | ESP32 / load | Notes |
|---|---|---|---|---|
| Pin 1 / PA9 inner PWM | Pin 1 -> 10 kΩ series | R1 | GPIO 27 | Test-rig wiring; monitor only |
| Pin 2 / GND | Common DC ground | - | ESP32 GND, fan returns | Required shared reference |
| Pin 3 / PA6 | Inner-fan tachometer | - | Nothing | Leave N/C; do not connect or simulate tach |
| Pin 4 / PA7 / FLAG2 | Accessory detection | Reserved | Nothing in current controller | Keep available only for validated accessory-detection work |
| Pin 5 / PA8 exhaust PWM | Pin 5 -> 13 kΩ series | R3 | GPIO 26 | Monitor only |
| GPIO 26 node | GPIO 26 -> 22 kΩ -> GND | R4 | Common GND | Divider return |
| Pin 5 / PA8 exhaust PWM | Direct PWM signal | - | Noctua blue PWM wire | U1 provides variable exhaust control |
| Pin 6 | Not used for controller power | - | Nothing | Switched/fused U1 output; unavailable in some modes |

## External 24 V and 5 V wiring

| Source | Destination | Connection |
|---|---|---|
| UI power supply | Heater box | External +24 V and GND |
| Heater-box +24 V | MP1584 input | IN+ / IN- |
| MP1584 output | ESP32 | +5 V to VIN, GND to common ground |
| MP1584 output | Servo | +5 V / GND |
| MP1584 output | G3MB SSR control side | 5 V / GND; GPIO 25 drives IN |
| External +24 V | Noctua red | Fan supply |
| External +24 V | 7530 blower MOSFET supply | MOSFET DC+ |
| External +24 V | AEDICO relay coil DC+ | Coil supply |

## Loads

| Load | Positive / supply | Return | Control |
|---|---|---|---|
| Noctua 24 V PWM | External +24 V | Common GND | Blue wire from U1 Pin 5; green tach N/C |
| 7530 blower | External +24 V through MOSFET OUT+ | MOSFET OUT- switched return | GPIO 23 |
| MG90S servo | +5 V buck output | Common GND | GPIO 18 |
| AEDICO relay coil | External +24 V | MOSFET OUT- switched return | Energized only when GPIO 23 runs blower |
| Heater | 110 V live through safety chain | Neutral bus | G3MB SSR output, GPIO 25, and AEDICO relay contact |

## 110 V live safety chain

```text
AC Live
  -> KSD301 70°C NC thermostat
  -> AEDIKO relay COM/NO
  -> G3MB SSR AC output
  -> 121°C thermal fuse
  -> PTC heater live terminal
```

Neutral goes directly to the heater and pilot-light return. Protective earth bonds to the enclosure, not to the DC signal ground unless the power-supply safety design explicitly requires it.

## Safety notes

- Keep the 110 V zone physically separated from the ESP32, U1, and fan-control wiring.
- Do not use U1 Pin 6 as the external 24 V supply.
- The R1 10 kΩ-only Pin 1 connection is the tested rig connection, not a confirmed 5 V-to-3.3 V level shifter. Measure GPIO 27 before permanent installation.
- The AEDICO relay coil is powered from external +24 V, but its return is the MOSFET-switched blower return. Therefore the relay should de-energize whenever GPIO23 turns the blower off. Verify the relay module accepts this switched-return arrangement and that the MOSFET can sink the combined blower and relay-coil current.
