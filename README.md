# libsdi12-verifier

Open-source SDI-12 compliance tester. Plug in a sensor, run the tests, get a pass/fail report.

Uses [libsdi12](https://github.com/phillipweinstock/libsdi12) under the hood.

## What it does

- Runs compliance tests against the SDI-12 v1.4 spec — commands, timing, CRC, the lot
- Measures response timing down to the microsecond (response latency, inter-character gaps, break duration)
- Outputs a compliance report as plain text or JSON, with references to the relevant spec sections
- Can also just sit on the bus and log traffic (monitor mode)
- Works on Windows, Linux, and macOS with any USB-to-serial adapter

## Quick Start

### Build

```bash
git clone --recursive https://github.com/phillipweinstock/libsdi12-verifier.git
cd libsdi12-verifier
mkdir build && cd build
cmake ..
cmake --build .
```

### Run

```bash
# Test a sensor on COM3 at address '0'
sdi12-verifier --port COM3 --test-sensor --addr 0

# JSON report to file
sdi12-verifier --port /dev/ttyUSB0 --test-sensor --format json -o report.json

# Passive bus monitor
sdi12-verifier --port COM3 --monitor

# With RTS direction control (for half-duplex adapters)
sdi12-verifier --port COM3 --test-sensor --rts
```

## Sensor Compliance Tests (21)

| # | Test | Command | Spec | What It Checks |
|---|------|---------|------|----------------|
| 1 | Acknowledge | `a!` | §4.4.2 | Sensor responds with `a\r\n` |
| 2 | Query Address | `?!` | §4.4.1 | Returns correct address |
| 3 | Identify | `aI!` | §4.4.3 | Valid vendor/model/firmware/serial fields |
| 4 | Wrong Address Silence | `x!` | §4.3 | No response to wrong address |
| 5 | Response Timing | `a!` | §4.2.3 | Response within 15ms |
| 6 | Inter-Character Gap | `aI!` | §4.2.4 | Max gap ≤1.66ms between response chars |
| 7 | Standard Measurement | `aM!` | §4.4.6 | Valid `atttn` response format |
| 8 | CRC Measurement | `aMC!` | §4.4.12 | CRC-16-IBM verified on data response |
| 9 | Concurrent Measurement | `aC!` | §4.4.8 | Valid `atttnn` response format |
| 10 | Concurrent + CRC | `aCC!` | §4.4.8/12 | CRC verified on concurrent data |
| 11 | Service Request Timing | `aM!` | §4.4.6 | Service request within ttt seconds |
| 12 | Data Retrieval | `aD0!` | §4.4.7 | All declared values retrievable |
| 13 | Data Retention | `aD0!` ×2 | §4.4.7 | Data persists across multiple reads |
| 14 | Continuous Measurement | `aR0!` | §4.4.9 | Immediate data response |
| 15 | Continuous + CRC | `aRC0!` | §4.4.9/12 | CRC verified on continuous data |
| 16 | Verification | `aV!` | §4.4.10 | Self-check command accepted |
| 17 | Address Change | `aAb!` | §4.4.4 | Change + restore roundtrip |
| 18 | Break Abort | break | §4.2.1 | Measurement aborted by break signal |
| 19 | Extended Command | `aX!` | §4.4.11 | Extended command response |
| 20 | Measurement Groups | `aM1!`–`aM9!` | §4.4.6 | Additional measurement groups |
| 21 | Bus Scan | `0!`–`z!` | §4.4.1 | All 62 addresses scanned |

## Example Output

```
══════════════════════════════════════════════════════════
  SDI-12 Compliance Report
  Generated: 2026-01-15 14:30:00
  Suite:     Sensor Compliance
  Port:      COM3
  Address:   '0'
  SDI-12:    v1.4
══════════════════════════════════════════════════════════

  [PASS ] Acknowledge (a!)                        §4.4.2
          Acknowledged in 2.340 ms
          Measured: 2.340 ms (limit: 15.000 ms)

  [PASS ] Response Timing (<=15ms)                §4.2.3
          8.120 ms (limit: 15.000 ms)
          Measured: 8.120 ms (limit: 15.000 ms)

  [FAIL ] Inter-Character Gap (<=1.66ms)          §4.2.4
          Max gap 2.450 ms exceeds 1.660 ms limit
          Measured: 2.450 ms (limit: 1.660 ms)

  [SKIP ] Continuous Measurement (aR0!)           §4.4.9
          Sensor does not support R0

────────────────────────────────────────────────────────────
  SUMMARY: 18 passed, 1 failed, 2 skipped, 0 errors
  RESULT:  NON-COMPLIANT
══════════════════════════════════════════════════════════
```

## Hardware

Any USB-to-TTL serial adapter that supports **1200 baud 7E1**:

| Adapter | Cost | Notes |
|---------|------|-------|
| FTDI FT232R | ~$5 | Most reliable, best timing |
| CP2102 | ~$3 | Good general purpose |
| CH340 | ~$2 | Budget option |

Plus an **SDI-12 level shifter** or DIY inverter circuit to convert between UART (0–3.3V) and SDI-12 (0–5V inverted logic).

Total hardware cost: around $5–15

### Wiring

```
USB-Serial Adapter          SDI-12 Sensor
┌──────────────────┐        ┌─────────────┐
│  TX  ──────────────────── │  Data       │
│  RX  ──────────────────── │  Data       │
│  GND ──────────────────── │  GND        │
└──────────────────┘        │  +12V ← PSU │
                            └─────────────┘
         (via level shifter / inverter)
```

> **Note:** SDI-12 uses inverted logic. A level shifter or simple transistor inverter
> is required between the UART adapter and the SDI-12 bus.

## How it works

```
CLI (main.c)           — arg parsing, mode dispatch, report output
Test Suite             — test registration + execution
Sensor Tests (21)      — the actual compliance checks
Timing Layer           — wraps libsdi12 callbacks to capture timestamps
libsdi12               — SDI-12 command formatting + response parsing
HAL (win32 / posix)    — serial I/O + platform timing
```

The timing layer sits between the test code and libsdi12's master API, recording microsecond timestamps on every send/receive without needing to mess with the protocol engine at all.

## Building from Source

### Prerequisites

- **CMake** 3.14+
- **C11 compiler** — GCC, Clang, or MSVC
- **Git** (for submodule)

### Windows (MSVC)

```cmd
git clone --recursive https://github.com/phillipweinstock/libsdi12-verifier.git
cd libsdi12-verifier
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

### Windows (MinGW)

```cmd
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
```

### Linux / macOS

```bash
mkdir build && cd build
cmake ..
make
```

## Roadmap

- [x] Sensor compliance tests (21 tests)
- [x] Windows + Linux/macOS HAL
- [x] Text + JSON compliance reports
- [x] Bus monitor mode
- [ ] Recorder compliance tests (verifier simulates sensor)
- [ ] GUI frontend (ImGui / tauri)
- [ ] Automated CI with simulated sensors
- [ ] Hardware-in-the-loop test mode

## License

MIT — © 2026 Phillip Weinstock

See [LICENSE](LICENSE) for the full text.
