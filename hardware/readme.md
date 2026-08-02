## Electronics

[Schematics](schematics.pdf)

ESP32 DevKit Pinout:

![ESP32 DevKit Pinout](esp32%20devkit%20pinout.jpg)

### Parts

| Ref             | Part                              | Description                                                                          | Qty |
|-----------------|------------------------------------|---------------------------------------------------------------------------------------|-----|
| U1              | ESP32-WROOM-32U dev kit           | Main microcontroller board                                                            | 1   |
| U2              | DS3231M                           | I2C real-time clock module                                                            | 1   |
| BT1             | CR2032                            | 3V coin cell, RTC backup battery                                                       | 1   |
| J6              | SSD1309 OLED 2.4"                     | I2C OLED display module                                                               | 1   |
| SW1             | Rotary encoder w/ push-button     | User input for menu navigation                                                         | 1   |
| U4              | AC-DC PSU, 230VAC → 5VDC 3A       | Powers ESP32, logic and display                                                        | 1   |
| —               | Transformer, 230VAC → 24VAC       | Powers valves relay coils (Eco wiring only)                                            | 1   |
| L1, L2          | HWCT-5A/5mV current clamp sensor  | Non-invasive AC current sensing — L1 on 24VAC (valves), L2 on 230VAC (pump)            | 2   |
| J1–J3           | 4-channel 5V relay module         | 12 relays total: 11 valves + 1 pump ("Relays group 1/2/3")                             | 3   |
| D1–D24          | 1N4003 diode                      | Only for non-opto-isolated ("red") relay modules; use a jumper instead on opto ("blue") modules | 24 (optional) |
| D25             | Blue LED, 5mm                     | Status/fault indicator                                                                | 1   |
| R1–R3, R8, R9, R12–R25 | 10kΩ resistor               | Pull-ups: rotary encoder, current sensors, relay inputs                               | 19  |
| R4, R5, R7, R11 | 100Ω resistor                     | Rotary encoder and current sensor signal conditioning                                  | 4   |
| R6, R10         | 1kΩ resistor                      | Current sensor burden resistor (24VAC / 230VAC)                                        | 2   |
| R26             | 30Ω resistor                      | LED current limiting                                                                   | 1   |
| C1, C4, C5      | 100nF capacitor                   | Rotary encoder debounce                                                                | 3   |
| C6, C7          | 10nF capacitor                    | Rotary encoder debounce                                                                | 2   |
| C2, C10         | 100µF 10V capacitor               | Decoupling (ESP32, OLED)                                                               | 2   |
| C8, C9          | 10µF 10V capacitor                | Current sensor filtering (24VAC / 230VAC)                                              | 2   |
| C3              | 470µF 16V capacitor               | PSU output smoothing                                                                   | 1   |
| J4, J5          | 19-pin headers                    | ESP32 devkit sockets                                                                   | 2   |
| J7–J9           | Screw terminal / JST connectors   | Mains AC in, PSU AC in, PSU DC out wiring                                              | 3   |

### About the relay modules
There are 2 types of "cheap" 4x relay modules on the market:

| PCB color | Blue | Red |
|--|--|--|
| Connectivity | 0.1" header | Terminal blocks |
| Active | Low | Configurable with jumper High/Low |
| Dimensions | Slightly larger | Slightly smaller |
| Optocopler | Less sensitive | Very sensitive |
| Assembly | simple | requires voltage drop diodes |

Red boards require volrage drop diodes to set the optocouplers in high state (non-active) when the MCU is high. All boards require pullups.

## Mechanical design

![Irrigator device](../docs/irrigator.jpg)

- [Base](irrigator%20box.stl)
- [Lid](irrigator%20lid.stl)

## Installation wiring

In industrial irrigators the 24VAC transformer is often integrated into the irrigator. This reduces the customer flexibility where to mount the irrigator and what type of valves to use. Also, always connected transformer has parasitic current consumptions which hits customer wallet and hurts to the ecology. Our irrigator allows using any 24VAC transformer. It can be mounted on DIN rail or standalone. And it can be driven together with pump, i.e. it is not always connected to mains. This means **more flexibility, less electricilty bill, and less environment impact**.

### Simple

![Simple Wiring](irrigator%20wiring-simple.png)

### Eco

![Eco Wiring](irrigator%20wiring-eco.png)

![Separated Eco Wiring](irrigator%20wiring-eco%20separated.png)

## Current sensing

There are only 2 current sensors in the irrigator:
- For the pump, 220-240VAC, high current up to 12A.
- For valves, 24VAC, sub-amper current, typically 250mA.

The current measurement is not mandatory for the irrigator functionality. However, it can be useful to detect overcurrent and under current events:
- Overcurrent:
  - Pump blocked.
  - Valve wiring short circuit.
- Undercurrent: circuit discontinuity resulting non-fucntioning pump and/or valve.

In both cases there is a risk of over-drying of your garden.

One valve current sensor is enough for 11 valves because valves are never opened together, only 1 zone works at every given moment. So it is easy to correlate which valve is malfunctioning.

The current sensing becomes functional only after [current sensor calibration](../docs/usage_irrigator.md#current-sensor-calibration-explained).
