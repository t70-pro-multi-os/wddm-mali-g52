# Powering the Mali-G57 on the T70 Pro, checked against the stock firmware

The upstream driver expects the firmware to power the GPU and describe it in
ACPI (as edk2-rk3576 does for the RK3576). On the T70 Pro that is
`GpuPowerDxe` in Mu-Silicium (`Platforms/Teclast/t70proPkg/Drivers/GpuPowerDxe`).
No tablet was available while this was written, so every value was checked
against four stock sources instead of the hardware:

- **DT**: the T70 Pro's own effective stock device tree (sha256 `a3dd45e2...`),
  as recorded in the node audit of 2026-11 (`/soc/mali@13000000`,
  `/soc/gpufreq`, `/soc/opp-table0`, the MT6366 regulators).
- **SRC**: MediaTek's MT6789 kernel sources (adomerle/android_kernel_mediatek_mt6789,
  `alps-mp-s0.mp1`): `gpufreq_mt6789.[ch]`, `clk-mt6789.c`,
  `clk-mt6789-mfgcfg.c`, `clk-pll.c`, `clk-mux.c`, `mt6358-regulator.c`.
- **KO**: the T70's own `mtk_gpufreq_mt6789.ko`, from the stock package
  `T70Pro(G3D1)_Android 16_V1.08_20260803_ROW_SZ` (`super.img`,
  `vendor_dlkm_a`), disassembled. Its sha256 (`62806856...`) and that of
  `mali_kbase_mt6789_a16w_jm.ko` (`3b5a27d7...`) equal the ones read from
  the tablet's own `vendor_dlkm`, so it is the installed build. Where KO and
  SRC differ, the firmware follows KO.
- **FW**: what the Mu-Silicium firmware already had (`ClockImplLib`,
  `MT6358PmicDxe`, `MemoryMapLib`, the T70 DSDT).

## Where the T70 module differs from MediaTek's source

| | SRC | KO (T70 V1.08) | Firmware |
|---|---|---|---|
| Rail order at power on | `vsram_gpu`, then `vgpu` | `vgpu`, then `vsram_gpu` (handles: `_vgpu` at +0, `_vsram_gpu` at +8, enabled +0 then +8) | KO |
| Way enable `INFRA_BCRM +0x4C` [12:11] | written at power on and off | not written | KO |
| `mfg_pll_sel` to MFGPLL | one `clk_set_parent` | `clk_set_parent`, then `CLK_CFG_4` bit 18 checked and retried with 10 us waits | KO |
| BG3D gate | before `mfg_pll_sel` | after `mfg_pll_sel` | KO |
| GPU timestamp `MFG_TOP +0x130` = 3 | separate function | right after the clocks, inside power on | KO |
| Operating table | 45 points, 1100 MHz 850 mV to 390 MHz 625 mV | same 45 frequencies and post-dividers, up to 50 mV more: 1100 MHz 900 mV to 390 MHz 675 mV, `vsram_gpu` 750 mV at the bottom | KO opp44 |

Identical in SRC and KO: the MFG0 to MFG3 sequence (`PWR_CON` `0x308` to
`0x314`, status `0x16C`/`0x170` bits 2 to 5, 10 us polls up to 501 times,
100 us before `PWR_CLK_DIS`), the MFG1 bus protection release
(`0x80` to `+0x718`, `0x600000` to `+0x2A4`, `0x60` to `+0x718`,
`0x200000` to `+0x2AC`), the `mfg_ref_sel` gate between MFG1 and MFG2, and
the `MFG_TOP` writes (`+0x20 |= 0x3C00000`, `+0xB0 &= ~0x600`,
`+0xB4 |= 0x10`, `+0xB0` clear bit 8 and set bit 19, `+0x24 |= 1`,
`+0xF60 |= 1`). The DT operating table (`/soc/opp-table0`) equals KO's.

The stock preloader carries "latch VGPU %d uV" and "latch VSRAM_GPU %d uV":
it sets both rails before Linux. The read-only build records what it left.

## Addresses and interrupts

| Item | DT | SRC | FW | Used |
|---|---|---|---|---|
| GPU registers | `0x13000000`, 0x4000 | same | DSDT GPU0 | yes |
| Interrupts GPU, MMU, JOB | SPI 396, 397, 398 level high | | DSDT GPU0 428, 429, 430 | yes |
| EVENT, PWR | SPI 399, 400 | not requested by the JM probe | not in DSDT | no |
| MFG_TOP | `0x13FBF000` | same | not mapped | mapped by the driver |
| SPM | `0x10006000` | `PWR_STATUS +0x16C`, `_2ND +0x170`, `MFG0..3_PWR_CON +0x308..0x314` | mapped | yes |
| INFRACFG_AO | `0x10001000` | bus protect `+0x2A0/0x2A4/0x228`, `_1 +0x2A8/0x2AC/0x258`, `_2 +0x714/0x718/0x724` | mapped | yes |
| INFRA_BCRM | `0x10215000` | way enable `+0x4C` [12:11] | not mapped | mapped by the driver |
| APMIXED | `0x1000C000` | `MFGPLL_CON0 +0x268`, `CON1 +0x26C`, `CON3 +0x274` | same in `ClockImplLib` | yes |
| TOPCKGEN | `0x10000000` | `CLK_CFG_4 +0x50` (set `+0x54`, clr `+0x58`), update `+0x04` | same in `ClockImplLib` | yes |

## Clocks

| Item | SRC | FW (`ClockImplLib`) |
|---|---|---|
| `mfg_ref_sel` | `CLK_CFG_4` [17:16], gate bit 23, update bit 18 | same |
| `mfg_pll_sel` | `CLK_CFG_4` bit 18, parents `mfg_ref_ck`, `mfgpll_ck`; no update bit | same |
| MFGPLL | enable `CON0` bit 0, power `CON3` bit 0, isolation `CON3` bit 1, posdiv `CON1` [26:24], PCW `CON1` [21:0], change bit 31; F = PCW x 26 MHz / 2^14 / 2^posdiv | same registers; its PLL parent list is wrong (`TopCamSelParents`) and is not used |
| BG3D gate | `MFG_TOP` status `+0x0`, set `+0x4`, clear `+0x8`, bit 0 | absent; the driver writes it |
| PLL enable order | `clk-pll.c mtk_pll_prepare`: CON3 power on, 1 us, isolation off, 1 us, CON0 enable, 20 us | |
| Mux enable | `clk-mux.c`: write the gate bit to clear, then the update bit | |

## Rails (MT6366, driven as MT6358)

| Rail | DT | SRC (`mt6358-regulator.c`, registers.h) | FW before | FW now |
|---|---|---|---|---|
| `vgpu` | buck, 0.5 to 1.29375 V, not always on | enable `0x1508` bit 0, select `0x1526` [6:0], 6.25 mV from 0.5 V | same | same |
| `vsram_gpu` | LDO, 0.5 to 1.29375 V, not always on | enable `0x1BC8` bit 0, select `0x1BF4` [6:0], 6.25 mV from 0.5 V | **absent** | added as `ldo_vsram_gpu` |

The chip id 0x66 bypass list in `mt6358-regulator.c` names only camera rails
and `vldo28`, so the GPU rails are the MT6358 ones.

## The stock power-on, and what the firmware does

`__gpufreq_power_control(POWER_ON)` as in SRC (see above for what KO changes):

1. `INFRA_BCRM +0x4C` bits 11 and 12 set (way enable). Not in KO.
2. `__gpufreq_buck_control`: enable `vsram_gpu`, then `vgpu`. KO: `vgpu` first.
3. `__gpufreq_mtcmos_control` (`GPUFREQ_SELF_CTRL_MTCMOS` is 1 on MT6789):
   MFG0 on, MFG1 on and its bus protection released in the order
   STEP2_1, STEP2_0, STEP1_1, STEP1_0, `mfg_ref_sel` gate on, MFG2 on, MFG3 on.
   Each domain: `PWR_ON`, `PWR_ON_2ND`, wait for both status bits, 100 us,
   clear `PWR_CLK_DIS`, clear `PWR_ISO`, set `PWR_RST_B`, clear `SRAM_PDN`,
   wait for `SRAM_PDN_ACK` to clear.
4. `__gpufreq_clock_control`: BG3D gate on, `mfg_pll_sel` to MFGPLL, then the
   `MFG_TOP` writes of `__gpufreq_external_cg_control`
   (`+0x20` bits 22 to 25, `+0xB0` clear bits 9 and 10, `+0xB4` bit 4,
   `+0xB0` set bit 19 and clear bit 8, `+0x24` bit 0, `+0xF60` bit 0).

`GpuPowerDxe` follows KO's order: rails (`vgpu`, `vsram_gpu`), MFG0, MFG1
and its bus protection, `mfg_ref_sel`, MFG2, MFG3, MFGPLL, `mfg_pll_sel`
checked, BG3D, the `MFG_TOP` writes, the timestamp, then `GPU_ID`. The
differences from KO:

- The stock driver waits up to 5 ms per step and carries on after a timeout.
  The firmware stops at the first step that does not complete and records it.
- The stock driver starts at the operating point that matches the frequency
  the preloader left, then lets DVFS move it. The firmware sets one fixed
  point, KO's opp44: 390 MHz (MFGPLL, posdiv 4, PCW `0xF0000`), `vgpu`
  675 mV, `vsram_gpu` 750 mV. The per-chip AVS and aging adjustments are
  not applied.
- Voltages are set before the rails are enabled, keeping `vsram_gpu` at or
  above `vgpu` in between (the stock `__gpufreq_volt_scale_gpu` order).
- MFGPLL is changed only while `mfg_pll_sel` points at `mfg_ref`.

## The record and ACPI

The driver clears 0x60 bytes at `0x43F7F500` on every boot and writes:

| Offset | |
|---|---|
| 0x00 | `T70G` |
| 0x04 | version 1, bit 8 set in the powering build |
| 0x08 | 0 read only, 1 powered and verified, `0x80` + step that failed |
| 0x0C | failed step (powering build) |
| 0x10 to 0x30 | what the preloader left: `PWR_STATUS`, `_2ND`, `MFGPLL_CON0`, `CON1`, `CON3`, `CLK_CFG_4`, `vgpu`, `vsram_gpu` ([31] on, [30] read, [23:0] uV), way enable |
| 0x34 to 0x4C | after: `PWR_STATUS`, `MFGPLL_CON1`, `GPU_ID`, `CLK_CFG_4`, `vgpu`, `vsram_gpu`, BG3D status |

DSDT `GPU0._STA` returns present only when the record says 1 and `GPU_ID`
reads as arch 9.0 product 3 (Mali-G57, Mesa `PAN_PROD_ID(9, 0, 3)`; the
MT6789 reads `0x90930010`). A Windows driver never sees an unpowered GPU.

## Builds

- Default build: the driver only reads and records; nothing changes.
- `T70_GPU_POWER_ON=1 build.sh firmware`: the driver also powers the GPU.

On the virtual T70 board (no SPM or GPU model) the read-only build records
and boots; the powering build sets and reads back `vgpu` 675 mV and
`vsram_gpu` 750 mV, stops at MFG0 (step 4) as it should without an SPM, hides
GPU0, and Windows Setup starts normally.

## Still open

- Read the record of the read-only build on the tablet: it shows what the
  preloader leaves (`MFGPLL`, rails, domains) before anything is changed.
