# PATCHES.md – local patches and how they work

Two patches.  One is applied to **your ESP-IDF checkout** (required
before building), one is **already applied** to the vendored
cpp_bus_driver in this tree (recorded for provenance).

---

## 1. `patches/esp-idf-v5.4.1-sd-coexistence.patch`  (APPLY TO ESP-IDF)

```
cd $IDF_PATH        # must be ESP-IDF v5.4.1
git apply /path/to/adsb-scope/patches/esp-idf-v5.4.1-sd-coexistence.patch
```

Validated 2026-07-29: applies cleanly to pristine `v5.4.1`
(round-tripped: apply → diff → re-apply on fresh checkout).
469 lines, 316 insertions / 4 deletions across three files.
`sdmmc_ll_enable_dma()` confirmed present in v5.4.1's ESP32-P4 HAL
(`hal/esp32p4/include/hal/sdmmc_ll.h:702`) and already included by
`sdmmc_host.c`.

### The problem it addresses

On the T-Display-P4, WiFi runs on a C6 co-processor over **SDMMC slot 1**
(ESP-Hosted SDIO) while the SD card runs on **GP-SPI3** (moved off SDMMC
to dodge espressif/esp-idf#17889).  Under WiFi load, SDHOST activity
disrupts concurrent GP-SPI3 SD transactions two ways:

- the SDHOST ISR fires mid-SPI-transaction; its `portYIELD_FROM_ISR()`
  preempts the SPI master's polling bookkeeping (`host->polling` /
  `cur_cs`), surfacing as `ESP_ERR_INVALID_STATE` (0x103/0x108);
- AXI-level interference corrupts bytes on the wire, surfacing as card
  CRC rejects (0x109), timeouts (0x107), or a **garbled data-response
  token** (e.g. `0x80` – not a legal SPI-mode response and not 0xFF).

### What the patch changes, per file

**`esp_driver_sdspi/src/sdspi_host.c`**
- Switches the two remaining interrupt-mode transfers in the read/write
  block paths (`spi_device_transmit`) to **`spi_device_polling_transmit`**,
  making every step of an SD block transaction polling-mode: no GDMA
  completion interrupt, deterministic AXI timing, fewer interrupt
  sources during the contention-sensitive window.
- Adds **garbled-response classification**: a data-response token that is
  invalid *and* not 0xFF sets `s_sdspi_garbled_response`.  Garbled means
  the card's internal state is unknown – fundamentally different from a
  clean CRC reject.
- Adds `WRITE_DIAG` logging at every failure step, correlated with
  SDHOST ISR count/recency (via `sdmmc_host_get_isr_stats()`), plus a
  `bounce=` flag showing whether the PSRAM→DMA bounce path was in use.

**`fatfs/diskio/diskio_sdmmc.c`**
- Retry taxonomy for reads and writes: up to 3 retries with 10 ms
  spacing on transient errors (0x106/0x107/0x108/0x109).
- **Garbled ⇒ no retry**: the write is failed immediately and
  `s_diskio_card_needs_reset` is set – retrying against a card in an
  unknown state risks silent corruption; the correct remedy is a power
  cycle.
- A successful retry deliberately does **not** trigger a reset: the card
  either rejected the bad frame cleanly (CRC) or recovered from a
  transient – a power cycle would cause more disruption than the error.
- IO/retry/fail counters exposed via `diskio_sdmmc_get_stats()`.

**`esp_driver_sdmmc/src/sdmmc_host.c`**
- ISR count/timestamp diagnostics (feeds `WRITE_DIAG` correlation).
- `sdmmc_host_intr_disable/enable()` – mask the SDHOST ISR around an SPI
  transaction (the peripheral latches pending interrupts; the C6 buffers
  outbound data; nothing is lost, only delayed ≤ ~20 ms).
- `sdmmc_host_is_idmac_idle()` – IDSTS[16:13] DMAC FSM == 0 and
  `status.data_busy` clear.
- `sdmmc_host_quiesce/resume()` – disable/enable the IDMAC via
  `BMOD.enable` (`sdmmc_ll_enable_dma`) to silence SDHOST descriptor
  polling on the AXI bus entirely.  The card clock is deliberately NOT
  touched: the clock-update pseudo-command times out (0x107) when the
  CIU is in certain states, and the card clock generates no AXI traffic.

### Active vs. dormant – read this before reasoning about the fence

As of the cut, the **application calls none of the sdmmc_host additions**
(verified by grep).  The production fence is the *passive* hardening:
all-polling SPI transfers + garbled classification + diskio retries,
with recovery flowing through the normal error-return path into
`sd_logger.c`'s existing power-cycle-and-remount handling.  The
intr-mask / IDMAC-quiesce / `diskio_card_needs_reset()` machinery is
**dormant scaffolding** – the prepared next escalation if soak testing
shows the passive fence insufficient.  Keep it; wiring it up is a small,
well-documented app-side change.

Related: `esp_hosted_sdio_dma_lock()` (weak no-op in `sd_logger.c`,
called best-effort from `main.cpp:sd_io_take`) has **no strong
implementation anywhere** – not in this patch, not in esp_hosted 2.12.3
(verified).  It is a vestigial hook name from an earlier fence design;
older README text describing "acquiring the ESP-Hosted SDIO DMA lock"
described intent, not mechanism.  See the correction block in
`docs/ARCHITECTURE.md`.

Cross-component note: the patch adds an `extern` reference from
`sdspi_host.c` to `sdmmc_host_get_isr_stats()` in esp_driver_sdmmc –
both components always build on ESP32-P4, so this links fine, but it is
a coupling to remember if either file is ever rebased.

Provenance note: John's original working-tree diff also contained an
incidental `components/openthread/openthread` submodule pointer change –
local checkout drift, unrelated to the fence, deliberately excluded here
(`git submodule update` in the IDF checkout clears it).

---

## 2. `patches/cpp_bus_driver_adsb_scope.patch`  (ALREADY APPLIED)

Applied to `components/cpp_bus_driver` (upstream pin `158b9c3`).  Do not
re-apply; the file is kept so the vendored tree is reconstructible and
reviewable.  Contents:

- **`Hardware_Spi`: persistent DMA bounce buffers.**  Pre-allocates
  DMA-capable TX/RX bounce buffers at `begin()` and routes `write()`
  (and read) through them.  Rationale: SX1262 (Meshy) callers pass
  PSRAM/stack pointers; per-transfer temporary DMA allocations both
  fragmented internal RAM and interacted with the AXI contention window.
  One allocation, forever, bounded by `DMA_BUF_SIZE`.
- **`Hardware_Iis::set_clock_rate()` / `get_clock_rate()`.**  Reconfigure
  I2S clocks without tearing down channels (music player sample-rate
  switching).  Upstream later implemented the same idea independently
  (`set_clock_reconfig`, cbd 2026-04-14) – noted for any future rebase.
- `dma_desc_num = 6` for I2S channel config; comment normalization.
- **Log levels (`src/config.h`).**  `DEBUG` off (it dumps every NMEA
  sentence) and, since coh3, `INFO` off as well: it printed two lines a
  second from the GNSS parser and a dozen per bus at start-up, each about
  130 characters on a 115200-baud console.  `BUS` and `CHIP` – the levels
  failures are reported at – stay on.  (The build the first device test
  replaced, `5b1dbb7-dirty` of 2026-04-18, did not print them either.)
  Re-checked 2026-11-18: the patch applies to a fresh clone at the pin and
  the result is byte-identical to `components/cpp_bus_driver`.
