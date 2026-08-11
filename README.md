# owgw-spi-master — Linux IIO driver for OWGW SPI Master (STM32 1-Wire gateway)

[![Build status](https://img.shields.io/badge/build-matrix-yellow)](#build-status)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)

Out-of-tree Linux IIO driver for the **OWGW** SPI master: an STM32-based
gateway that polls up to 16 1-Wire temperature sensors and exposes the
readings to userspace through the standard Linux **IIO** (Industrial I/O)
subsystem. Access is via sysfs and `libiio` — no custom API.

The driver speaks a small request/response protocol over SPI (see
[`include/linux/iio/temperature/owgw_iface.h`](include/linux/iio/temperature/owgw_iface.h)),
and the slave firmware running on the STM32 is responsible for the actual
1-Wire bit-banging and DS18B20 / similar conversions.

## Protocol

Every SPI transaction is a fixed-length exchange:

  Master → Slave request:  `[CMD:1] [LEN:1] [PAYLOAD:0..N]`
  Slave  → Master reply :  `[STATUS:1] [LEN:1] [PAYLOAD:0..N] [CRC8:1]`

  * Little-endian for any multi-byte fields
  * CRC-8 over `STATUS || LEN || PAYLOAD` with polynomial 0x07 (MSB-first)
  * Up to 32 bytes of payload (16 sensors × 2 bytes raw temperature)

The driver implements commands `OW_CMD_START` / `OW_CMD_STOP` /
`OW_CMD_GET_DATA` and never blocks waiting for a sensor conversion — the
STM32 firmware owns the 1-Wire timing.

## Hardware

- Interface: **SPI mode 0**, up to 10 MHz, MSB-first, 8 bits per word
  (the driver hard-codes `SPI_MODE_0` in `owgw_probe`; `spi-cpha` /
  `spi-cpol` DT properties are NOT respected)
- Slave: STM32 with custom 1-Wire master firmware (out of scope)
- 1-Wire side: up to 16 DS18B20-class sensors
- Power and level shifting: board-specific, out of scope

## Kernel compatibility

Originally written and shipped against **4.19.y** LTS. The CI matrix below
verifies builds against several modern LTS releases — no source changes
are typically needed.

| Kernel | Status                                |
|--------|---------------------------------------|
| 4.19   | tested (in-house target)              |
| 5.10   | builds in CI                          |
| 5.15   | builds in CI                          |
| 6.1    | builds in CI                          |
| 6.6    | builds in CI                          |
| 6.12   | builds in CI                          |

The driver only consumes stable kernel APIs (`devm_*`, `mutex`,
`delayed_work`, `spi_sync`, `crc8`, `IIO` channels), so any LTS ≥ 4.5
should be fine.

## Building

### (a) Out-of-tree, on the target machine

```sh
make
sudo make install           # to /lib/modules/$(uname -r)/extra/
sudo depmod -a
sudo modprobe owgw-spi-master
```

`KDIR` defaults to `/lib/modules/$(uname -r)/build`. Override it if your
kernel sources are elsewhere:

```sh
make KDIR=/path/to/kernel/source
```

### (b) DKMS (recommended for end users)

DKMS rebuilds the module automatically when the kernel changes.

```sh
sudo cp -r . /usr/src/owgw-spi-master-0.1.0
sudo dkms add owgw-spi-master/0.1.0
sudo dkms build owgw-spi-master/0.1.0
sudo dkms install owgw-spi-master/0.1.0
sudo modprobe owgw-spi-master
```

### (c) CI (just look at the badges)

The `.github/workflows/build.yml` workflow compiles the driver against
the matrix above on every push. Click the build badge at the top of this
README.

## Device Tree

A minimal binding example:

```dts
&spi0 {
    status = "okay";

    owgw: owgw@0 {
        compatible = "proglyk,owgw-spi-master";
        reg = <0>;                /* CS0 */
        spi-max-frequency = <10000000>;
    };
};
```

A full binding schema lives in
[`Documentation/devicetree/bindings/iio/temperature/proglyk,owgw-spi-master.yaml`](Documentation/devicetree/bindings/iio/temperature/proglyk,owgw-spi-master.yaml).

## Usage

After loading, the device appears under `/sys/bus/iio/devices/iio:deviceN`.

```sh
# Discover the device
iio_info

# Read raw temperature on channel 0 (s16, 1 unit == 1 °C, scale = 1.0)
cat /sys/bus/iio/devices/iio:device0/in_temp0_raw

# Driver status (running / stopped)
cat /sys/bus/iio/devices/iio:device0/status

# Polling interval in ms (10..60000, default 1000)
echo 500 > /sys/bus/iio/devices/iio:device0/poll_interval
```

Note: temperature values come from a background `delayed_work` that the
driver schedules in `owgw_probe`. `in_tempN_raw` returns the most
recent sample, not a freshly-converted one.

## Known limitations

- **Source provenance:** the `owgw-spi-master.c` and `owgw_iface.h` files
  were copied verbatim from an in-house `kernel-firefly` tree (branch
  `larus100rev3`); as of 2026-08-11 they are still untracked in that
  tree, so there is no upstream kernel commit to point at.
- **Unused `struct OneWireHW`** at the end of `owgw_iface.h` is leftover
  from the STM32 firmware contract; the Linux driver does not reference
  it.
- **Russian-language comments** survive in `owgw_poll_work` and
  `owgw_probe` (e.g. `/* Безопасный выход без вызова cleanup_trigger */`).
  Harmless, but ugly for a public repo.
- **`poll-interval-ms` is declared in the DT binding but ignored by the
  driver.** The driver always uses the 1000 ms C-level default. Setting
  this property in DT has no effect today.
- **No `MODULE_VERSION("0.1.0")` macro** yet.
- **24-hour hardware stress test** not yet documented (smoke-tested
  only).
- **SPI mode is hard-coded** to mode 0 in `owgw_probe` and is not
  configurable through Device Tree.
- **SPDX / `MODULE_LICENSE` mismatch** in `owgw-spi-master.c`: the
  SPDX header says `GPL-2.0+` (or later) while `MODULE_LICENSE` says
  `"GPL v2"` (only). This is carried over verbatim from the upstream
  in-house source; resolving it requires a source patch in addition
  to a `MODULE_LICENSE` change, so it is tracked in TODO rather than
  silently edited in the public repo.

## TODO

- [ ] Remove `struct OneWireHW` from `owgw_iface.h` (or move it to a
      `firmware/` subdir if the STM32 side still needs the contract).
- [ ] Translate Russian comments to English; run
      `scripts/checkpatch.pl --strict` and resolve every warning.
- [ ] Parse `poll-interval-ms` from DT in `owgw_probe` and apply it to
      `st->poll_interval_ms`.
- [ ] Add `MODULE_VERSION("0.1.0")` after `MODULE_LICENSE`.
- [ ] Wire a real `make dt_binding_check` step into the CI matrix.
- [ ] Run a 24 h continuous poll on real hardware and document the
      result here.
- [ ] Reconcile the SPDX `GPL-2.0+` header with `MODULE_LICENSE("GPL v2")`
      in `owgw-spi-master.c` — pick one (recommended: `GPL-2.0` to match
      the module license) and ship both changes in a single commit.

## Credits

Driver by **Ilya Pronyashin** `<msg@proglyk.ru>`, written in-house on a
Firefly ITX-3568Q (RK3568) running kernel 4.19.

## License

GPLv2-only. See [`LICENSE`](LICENSE). The driver is taint-clean with
`MODULE_LICENSE("GPL v2")`; no `EXPORT_SYMBOL_GPL` consumers are needed
by this driver itself, but GPLv2-only is kept for consistency with the
rest of the IIO subsystem.
