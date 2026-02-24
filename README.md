# libsdi12-verifier

Open-source SDI-12 compliance tester. Plug in a sensor (or recorder), run the tests, get a pass/fail report.

Uses [libsdi12](https://github.com/phillipweinstock/libsdi12) under the hood.

## What it does

- Runs **47 compliance tests** against the SDI-12 v1.4 spec — commands, timing, CRC, the lot
- Tests both **sensors** (verifier acts as recorder) and **recorders** (verifier simulates a sensor)
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

# Test a data recorder — verifier acts as a simulated sensor
sdi12-verifier --port COM3 --test-recorder --addr 0

# JSON report to file
sdi12-verifier --port /dev/ttyUSB0 --test-sensor --format json -o report.json

# Passive bus monitor
sdi12-verifier --port COM3 --monitor

# With RTS direction control (for half-duplex adapters)
sdi12-verifier --port COM3 --test-sensor --rts

# Self-test — runs the verifier against itself via loopback (no hardware needed)
sdi12-verifier --self-test
```

## Sensor Compliance Tests (31)

The verifier acts as the data recorder, sends commands, and validates every aspect of the sensor's responses.

| # | Key | Test | Command | Spec | What It Checks |
|---|-----|------|---------|------|----------------|
| 1 | `acknowledge` | Acknowledge | `a!` | §4.4.2 | Sensor responds with `a\r\n` |
| 2 | `query_address` | Query Address | `?!` | §4.4.1 | Returns correct address |
| 3 | `identify` | Identify | `aI!` | §4.4.3 | Valid vendor/model/firmware/serial fields |
| 4 | `identify_ver` | Identify Version | `aI!` | §4.4.3 | SDI-12 version field (`ll`) is "13" or "14" |
| 5 | `wrong_addr` | Wrong Address Silence | `x!` | §4.3 | No response to wrong address |
| 6 | `response_time` | Response Timing | `a!` | §4.2.3 | Response within 15 ms |
| 7 | `interchar_gap` | Inter-Character Gap | `aI!` | §4.2.4 | Max gap ≤ 1.66 ms between response chars |
| 8 | `meas_m` | Standard Measurement | `aM!` | §4.4.6 | Valid `atttn` response format |
| 9 | `meas_mc` | CRC Measurement | `aMC!` | §4.4.12 | CRC-16-IBM verified on data response |
| 10 | `meas_c` | Concurrent Measurement | `aC!` | §4.4.8 | Valid `atttnn` response format |
| 11 | `meas_cc` | Concurrent + CRC | `aCC!` | §4.4.8/12 | CRC verified on concurrent data |
| 12 | `service_req` | Service Request Timing | `aM!` | §4.4.6 | Service request within `ttt` seconds |
| 13 | `data_d0` | Data Retrieval | `aD0!` | §4.4.7 | All declared values retrievable |
| 14 | `data_retain` | Data Retention | `aD0!` ×2 | §4.4.7 | Data persists across multiple reads |
| 15 | `continuous` | Continuous Measurement | `aR0!` | §4.4.9 | Immediate data response |
| 16 | `continuous_crc` | Continuous + CRC | `aRC0!` | §4.4.9/12 | CRC verified on continuous data |
| 17 | `verify` | Verification | `aV!` | §4.4.10 | Self-check command accepted |
| 18 | `addr_change` | Address Change | `aAb!` | §4.4.4 | Change + restore roundtrip |
| 19 | `break_abort` | Break Abort | break | §4.2.1 | Measurement aborted by break signal |
| 20 | `extended` | Extended Command | `aX!` | §4.4.11 | Extended command response |
| 21 | `extended_ml` | Extended Multi-Line | `aX!` | §4.4.11 | Multi-line extended response handling |
| 22 | `meas_groups` | Measurement Groups | `aM1!`–`aM9!` | §4.4.6 | Additional measurement groups |
| 23 | `conc_groups` | Concurrent Groups | `aC1!`–`aC9!` | §4.4.8 | Additional concurrent groups |
| 24 | `cont_groups` | Continuous Groups | `aR1!`–`aR9!` | §4.4.9 | Additional continuous groups |
| 25 | `bus_scan` | Full Bus Scan | `0!`–`z!` | §4.4.1 | All 62 addresses scanned |
| 26 | `resp_compliance` | Response Compliance | various | §4.3/4.4 | Response format, addressing, terminator |
| 27 | `hv_ascii` | High-Volume ASCII | `aHA!` | §4.4.13 | HV ASCII measurement + data pages |
| 28 | `hv_ascii_crc` | High-Volume ASCII + CRC | `aHAC!` | §4.4.13/12 | CRC verified on HV ASCII data |
| 29 | `hv_binary` | High-Volume Binary | `aHB!` | §4.4.14 | HV binary type/length/payload decode |
| 30 | `hv_binary_crc` | High-Volume Binary + CRC | `aHBC!` | §4.4.14/12 | CRC verified on raw HV binary response |
| 31 | `identify_meas` | Identify Measurement | `aIM!` | §4.4.15 | Measurement metadata listing |

## Recorder Compliance Tests (16)

The verifier acts as a simulated SDI-12 sensor. It responds correctly to commands while measuring the recorder's protocol compliance from the sensor's perspective.

| # | Key | Test | Spec | What It Checks |
|---|-----|------|------|----------------|
| 1 | `rec_break` | Break Duration | §4.2.1 | Break signal ≥ 12 ms |
| 2 | `rec_marking` | Marking After Break | §4.2.1 | Marking interval ≥ 8.33 ms |
| 3 | `rec_cmd_fmt` | Command Format | §4.3 | Valid address + command + terminator |
| 4 | `rec_address` | Correct Address | §4.3 | Commands addressed to the target sensor |
| 5 | `rec_ack` | Acknowledge Handling | §4.4.2 | Recorder sends `a!` and accepts response |
| 6 | `rec_identify` | Identify Command | §4.4.3 | Recorder sends `aI!` and parses response |
| 7 | `rec_sequence` | M → D Sequence | §4.4.6 | Correct `aM!` → wait → `aD0!` flow |
| 8 | `rec_interchar` | Command Inter-Char Gap | §4.2.4 | Gap between command characters |
| 9 | `rec_brk_between` | Break Between Commands | §4.2.1 | Break signal between successive commands |
| 10 | `rec_svc_req` | Service Request Handling | §4.4.6 | Recorder waits for and responds to service request |
| 11 | `rec_concurrent` | Concurrent Measurement | §4.4.9 | Recorder sends `aC!` + `aD0!` sequence |
| 12 | `rec_crc` | CRC Measurement | §4.4.7 | Recorder handles CRC variants (MC/CC) |
| 13 | `rec_continuous` | Continuous Measurement | §4.4.10 | Recorder sends `aR0!` for immediate data |
| 14 | `rec_addr_change` | Address Change | §4.4.4 | Recorder sends `aAb!` to change sensor address |
| 15 | `rec_verify` | Verification Command | §4.4.10 | Recorder sends `aV!` for self-check |
| 16 | `rec_extended` | Extended Command | §4.4.11 | Recorder sends `aX...!` extended command |

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
  SUMMARY: 26 passed, 1 failed, 4 skipped, 0 errors
  RESULT:  NON-COMPLIANT
══════════════════════════════════════════════════════════
```

### JSON Output

```bash
sdi12-verifier --port COM3 --test-sensor --format json -o report.json
```

Each test result includes a machine-readable `key` field (e.g. `"meas_m"`, `"rec_break"`) for programmatic processing.

### Exit Codes

| Code | Meaning |
|------|---------|
| `0` | All tests passed |
| `1` | One or more tests failed |
| `2` | Error (port not found, init failure, etc.) |

## Hardware

Any USB-to-TTL serial adapter that supports **1200 baud 7E1**:

| Adapter | Cost | Notes |
|---------|------|-------|
| FTDI FT232R | ~$5 | Most reliable, best timing |
| CP2102 | ~$3 | Good general purpose |
| CH340 | ~$2 | Budget option |

Plus an **SDI-12 level shifter** or DIY inverter circuit to convert between the UART logic levels (typically 0–3.3V or 0–5V) and the SDI-12 bus (typically 0–5V or 0–12V, inverted logic).

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

## Architecture

```
CLI (main.c)                — arg parsing, mode dispatch, report output
Test Suite                  — test registration + execution engine
├─ Sensor Tests (31)        — verifier as recorder → tests sensors
└─ Recorder Tests (16)      — verifier as sensor  → tests recorders
Timing Layer                — wraps libsdi12 callbacks for µs timestamps
Reporter                    — text + JSON output with spec references
libsdi12                    — SDI-12 command formatting + response parsing
HAL (win32 / posix)         — serial I/O + platform timing
```

The timing layer sits between the test code and libsdi12's master API, recording microsecond timestamps on every send/receive without needing to mess with the protocol engine at all.

For recorder tests, the verifier configures itself as a simulated sensor using libsdi12's sensor API, responding correctly to commands while measuring the recorder's protocol behavior from the sensor's perspective.

## Self-Test Mode

Run `--self-test` to verify the tool itself — no sensor, no serial port, no hardware:

```bash
sdi12-verifier --self-test
```

This spins up a **loopback HAL** that wires the master and sensor sides of
libsdi12 together in-memory. Every command the master sends is processed by a
virtual sensor (address `'0'`, vendor `LOOPBAK`, model `SELFTS`) and the
response is fed straight back — all in the same process.

The loopback sensor supports all 31 sensor tests:

| Result | Count | Notes |
|--------|------:|-------|
| PASS   |    27 | Full protocol coverage |
| SKIP   |     4 | Service-request timing (ttt=0), measurement groups (M1–M9, C1–C9, R1–R9) |

Self-test is useful for:
- **CI / smoke testing** — runs in milliseconds, no hardware in the loop
- **Library validation** — the loopback mode found real bugs in libsdi12's
  sensor API (see below)
- **Development** — iterate on test logic without a physical sensor

### Bugs Found via Self-Test

Running the verifier against its own library caught several genuine bugs in
libsdi12's sensor implementation:

| Bug | Spec Reference | Fix |
|-----|---------------|-----|
| `send_response()` used `strlen()` on binary payloads, truncating at embedded null bytes | §5.2 (HV binary packet) | Added `resp_len` field; use explicit length when set |
| `sdi12_crc_append()` used `strlen()` — same truncation for binary CRC | §4.4.12 (CRC computation) | Added `sdi12_crc_append_n()` with explicit length |
| HV dispatch always passed `crc=true` for both `aHA!` and `aHB!` | §5.1/5.2 | Fixed to detect `'C'` suffix (`aHAC!`/`aHBC!`) |

## SDI-12 v1.4 Spec Cross-Reference

The test suite was cross-referenced against the
[SDI-12 Specification v1.4](https://www.sdi-12.org/) (February 20, 2023).
Key points verified:

| Area | Spec Section | Status |
|------|-------------|--------|
| CRC-16-IBM algorithm | §4.4.12.1 | ✅ Polynomial 0xA001, init 0x0000, matches spec pseudocode |
| CRC ASCII encoding | §4.4.12.2 | ✅ 3 chars, each `0x40 \| 6bits` |
| CRC scope | §4.4.12.1 | ✅ Address through last data char (before CR) |
| HV ASCII: CRC mandatory | §5.1 | ✅ Tested via `aHA!` / `aHAC!` |
| HV binary data types | §5.2.1 Table 16 | ✅ All 10 types (int8–uint64, float32/64) |
| HV binary packet format | §5.2 Table 14 | ⚠️ Spec uses `aDBn!` + binary CRC (2 bytes); library uses `aDn!` + ASCII CRC (3 chars) — simplified |
| Response timing ≤15 ms | §4.2.3 | ✅ |
| Inter-character gap ≤1.66 ms | §4.2.4 | ✅ |
| Break ≥12 ms | §4.2.1 | ✅ |
| Marking after break ≥8.33 ms | §4.2.1 | ✅ |
| Service request format | §4.4.6 | ✅ |
| Metadata commands | §6.0–6.2 | ✅ `aIM!`, `aIM_nnn!` |

> **Note on HV binary**: The SDI-12 v1.4 spec (§5.2) defines binary data
> retrieval via `aDBn!` commands with a structured packet format (address +
> 16-bit packet size + 8-bit type + payload + 16-bit binary CRC). The current
> library implementation uses the simpler `aDn!` command with ASCII CRC for
> both HV ASCII and HV binary modes. This is sufficient for functional testing
> but does not exercise the full spec-defined binary packet framing. A future
> version may add native `aDBn!` support.

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

- [x] Sensor compliance tests (31 tests)
- [x] Recorder compliance tests (16 tests)
- [x] Windows + Linux/macOS HAL
- [x] Text + JSON compliance reports (with test keys + exit codes)
- [x] Bus monitor mode
- [x] High-volume measurement tests (ASCII + binary, with CRC)
- [x] Service request and concurrent measurement tests
- [x] Self-test via loopback (no hardware required)
- [ ] Native `aDBn!` binary packet framing per §5.2
- [ ] GUI frontend (ImGui / Tauri)
- [ ] Automated CI with simulated sensors
- [ ] Hardware-in-the-loop test mode

## License

MIT — © 2026 Phillip Weinstock

See [LICENSE](LICENSE) for the full text.
