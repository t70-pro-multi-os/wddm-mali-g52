// SPDX-License-Identifier: GPL-2.0
/* The MT6789's GPU clock and supplies, for maliwddm (Teclast T70 Pro).
 *
 * The GPU runs from MFGPLL (APMIXED 0x1000C000) through TOPCKGEN's
 * mfg_pll_sel (CLK_CFG_4 bit 18; when clear, mfg_ref). Its supplies are the
 * MT6366's vgpu buck and vsram_gpu LDO, driven as the MT6358's. The UEFI
 * (Mu-Silicium GpuPowerDxe) powers the GPU at the stock module's lowest
 * operating point, 390 MHz with vgpu 675 mV and vsram 750 mV, and records
 * what it did at 0x43F7F500 ('T70G', docs/MT6789-POWER.md).
 *
 * Read always: the clock now (MwMt6789GpuHz) and the firmware's record.
 *
 * Changed only with the setting Dvfs = 1 (Device Parameters; off by
 * default until measured on the tablet): a governor thread moves the GPU
 * along the T70 module's own table by load, as the Pixel 6 port's
 * governor.c does (50 ms windows, up above 45% busy aiming at 40%, down
 * below 25% for three windows, the lowest point after four idle ones).
 *   - Voltages go through the firmware's PMIC broker, a PRM handler
 *     (MtkPmicBroker.h, GUID 9f58ca73-...), which allows only the four GPU
 *     rail registers; Windows never maps the PMIC wrapper itself.
 *   - Going up: vsram, then vgpu, settle, then the clock. Going down: the
 *     clock, then vgpu, then vsram (the stock __gpufreq_volt_scale_gpu
 *     order), so vsram >= vgpu throughout.
 *   - The clock: mfg_pll_sel parked on mfg_ref, MFGPLL_CON1 written with its
 *     change bit, 20 us to relock, mfg_pll_sel back on MFGPLL (checked and
 *     retried as the stock module checks CLK_CFG_4 bit 18).
 *   - A GPU fault (MwMt6789Fault, from the interrupt) asks for the lowest
 *     point; the device stopping puts the lowest point back.
 * Per-chip AVS and aging adjustments are not applied: the table is the T70
 * module's default table, up to 50 mV above MediaTek's source. */
#include "maliwddm.h"

#ifdef MALI_SOC_MT6789
#include <prminterface.h>

#define MT_APMIXED_PA           0x1000C000u
#define MT_MFGPLL_CON0          0x268u          /* bit 0 enable */
#define MT_MFGPLL_CON1          0x26Cu          /* [31] change, [26:24] posdiv, [21:0] pcw */
#define MT_TOPCKGEN_PA          0x10000000u
#define MT_CLK_CFG_4            0x050u
#define MT_CLK_CFG_4_SET        0x054u
#define MT_CLK_CFG_4_CLR        0x058u
#define MT_MFG_PLL_SEL          (1u << 18)
#define MT_REF_HZ               26000000ull

#define MT_RECORD_PA            0x43F7F500u
#define MT_RECORD_SIZE          0x60u
#define MT_RECORD_MAGIC         0x47303754u     /* 'T70G' */

#define MT_PMIC_VGPU_SEL        0x1526u         /* [6:0] = (uV - 500000) / 6250 */
#define MT_PMIC_VSRAM_SEL       0x1BF4u
#define MT_PMIC_UV(sel)         (500000u + 6250u * (sel))

#define MT_GOV_PERIOD_MS        50
#define MT_GOV_UP_PCT           45
#define MT_GOV_AIM_PCT          40
#define MT_GOV_DOWN_PCT         25

/* The T70 V1.08 mtk_gpufreq_mt6789.ko g_default_gpu table: kHz, vgpu and
 * vsram in 10 uV, the PLL post-divider as 2^n. Index 44 is what the
 * firmware sets. */
static const struct { ULONG Khz; USHORT Vgpu, Vsram; UCHAR Posdiv; } MtOpp[] = {
    { 1100000, 90000, 90000, 1 }, { 1086000, 89375, 89375, 1 }, { 1072000, 88750, 88750, 1 },
    { 1058000, 88125, 88125, 1 }, { 1045000, 87500, 87500, 1 }, { 1031000, 86875, 86875, 1 },
    { 1017000, 86250, 86250, 1 }, { 1003000, 85625, 85625, 1 }, {  990000, 85000, 85000, 1 },
    {  976000, 84375, 84375, 1 }, {  962000, 83125, 83125, 1 }, {  948000, 82500, 82500, 2 },
    {  935000, 81875, 81875, 2 }, {  921000, 81250, 81250, 2 }, {  907000, 80625, 80625, 2 },
    {  893000, 80000, 80000, 2 }, {  880000, 79375, 79375, 2 }, {  868000, 78750, 78750, 2 },
    {  857000, 78125, 78125, 2 }, {  846000, 77500, 77500, 2 }, {  835000, 76875, 76875, 2 },
    {  823000, 76250, 76250, 2 }, {  812000, 75625, 75625, 2 }, {  801000, 75625, 75625, 2 },
    {  790000, 75000, 75000, 2 }, {  778000, 74375, 75000, 2 }, {  767000, 73750, 75000, 2 },
    {  756000, 73125, 75000, 2 }, {  745000, 72500, 75000, 2 }, {  733000, 71875, 75000, 2 },
    {  722000, 71250, 75000, 2 }, {  711000, 70625, 75000, 2 }, {  700000, 70000, 75000, 2 },
    {  674000, 70000, 75000, 2 }, {  648000, 70000, 75000, 2 }, {  622000, 69375, 75000, 2 },
    {  596000, 69375, 75000, 2 }, {  570000, 69375, 75000, 2 }, {  545000, 68750, 75000, 2 },
    {  519000, 68750, 75000, 2 }, {  493000, 68750, 75000, 2 }, {  467000, 68125, 75000, 2 },
    {  441000, 68125, 75000, 2 }, {  415000, 68125, 75000, 2 }, {  390000, 67500, 75000, 2 },
};
#define MT_OPP_LOWEST           (ARRAYSIZE(MtOpp) - 1)

/* ---- the PMIC broker (Mu-Silicium MtkPmicBroker.h, BSD-2-Clause-Patent) ---- */

typedef struct {
    UINT32 Size;
    UINT16 Version, Operation;
    UINT32 Flags, State;
    UINT64 Generation;
    UINT32 Result, Address, Value, Reserved0;
    UINT64 Reserved[3];
} MT_PMIC_REQUEST;
C_ASSERT(sizeof(MT_PMIC_REQUEST) == 64);

enum { MtPmicBeginRead = 1, MtPmicBeginWrite = 2, MtPmicStep = 3, MtPmicAck = 4 };
enum { MtPmicIdle = 0, MtPmicPending = 1, MtPmicDone = 2, MtPmicFault = 3 };
enum { MtPmicOk = 0, MtPmicInProgress = 1, MtPmicInvalid = 2, MtPmicBusy = 3 };

static const GUID MtPmicHandler = { 0x9f58ca73, 0x6bc4, 0x45b8, { 0xb6, 0x9c, 0xf3, 0x3c, 0x62, 0x61, 0xd0, 0x81 } };

/* Once a transfer the broker may have accepted fails, the module stays
 * locked and nothing more is asked of it until the next boot (as the
 * Mu-Silicium clients do): its journal must not be overwritten. */
static volatile LONG MtPmicState;   /* 0 free, 1 in use, 2 quarantined */

static NTSTATUS PmicInvoke(PRM_INTERFACE *Prm, MT_PMIC_REQUEST *R, UINT16 Op, UINT64 Gen, UINT32 Addr, UINT32 Value)
{
    ULONG64 efi = MAXULONG64;
    NTSTATUS st;

    RtlZeroMemory(R, sizeof(*R));
    R->Size = sizeof(*R);
    R->Version = 1;
    R->Operation = Op;
    R->Generation = Gen;
    R->Address = Addr;
    R->Value = Value;
    st = Prm->InvokeHandler((GUID *)&MtPmicHandler, R, 0, &efi);
    if (st != STATUS_SUCCESS || efi != 0)
        return STATUS_UNSUCCESSFUL;
    if (R->Size != sizeof(*R) || R->Version != 1 || R->Operation != Op)
        return STATUS_DATA_ERROR;
    if (R->Result == MtPmicBusy)
        return STATUS_DEVICE_BUSY;
    return STATUS_SUCCESS;
}

/* One read or write of a GPU rail register: Begin, Step until Done, Ack.
 * PASSIVE_LEVEL. */
static NTSTATUS Pmic(BOOLEAN Write, UINT32 Addr, USHORT In, USHORT *Out)
{
    PRM_INTERFACE prm;
    MT_PMIC_REQUEST r;
    BOOLEAN found = FALSE, locked = FALSE, pinned = FALSE;
    UINT64 gen = 0;
    UINT32 value = Write ? In : 0, got = 0;
    UINT16 op = Write ? MtPmicBeginWrite : MtPmicBeginRead;
    ULONG64 start = KeQueryInterruptTime();
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    LONG was;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    was = InterlockedCompareExchange(&MtPmicState, 1, 0);
    if (was != 0)
        return was == 2 ? STATUS_DEVICE_NOT_READY : STATUS_DEVICE_BUSY;
    RtlZeroMemory(&prm, sizeof(prm));
    if (ExGetPrmInterface(PRM_INTERFACE_V1, &prm) != STATUS_SUCCESS || prm.Version != PRM_INTERFACE_V1 ||
        prm.LockModule == NULL || prm.UnlockModule == NULL || prm.QueryHandler == NULL || prm.InvokeHandler == NULL) {
        st = STATUS_NOT_SUPPORTED;
        goto out;
    }
    if (prm.LockModule((GUID *)&MtPmicHandler) != STATUS_SUCCESS)
        goto out;
    locked = TRUE;
    if (prm.QueryHandler((GUID *)&MtPmicHandler, &found) != STATUS_SUCCESS || !found) {
        st = STATUS_NOT_SUPPORTED;
        goto out;
    }
    for (;;) {                                  /* Begin, while the broker is busy */
        st = PmicInvoke(&prm, &r, op, 0, Addr, value);
        if (st != STATUS_DEVICE_BUSY)
            break;
        if (KeQueryInterruptTime() - start > 100000)    /* 10 ms */
            goto out;
        KeStallExecutionProcessor(10);
    }
    pinned = TRUE;                              /* from here the broker may hold our job */
    if (!NT_SUCCESS(st) || r.Result != MtPmicInProgress || r.State != MtPmicPending || r.Generation == 0 ||
        r.Address != Addr) {
        st = STATUS_UNSUCCESSFUL;
        goto out;
    }
    gen = r.Generation;
    for (;;) {                                  /* Step until done */
        st = PmicInvoke(&prm, &r, MtPmicStep, gen, 0, 0);
        if (st == STATUS_SUCCESS && r.State == MtPmicDone)
            break;
        if ((st != STATUS_SUCCESS && st != STATUS_DEVICE_BUSY) || (st == STATUS_SUCCESS && r.State != MtPmicPending) ||
            KeQueryInterruptTime() - start > 100000) {
            st = STATUS_UNSUCCESSFUL;
            goto out;
        }
        KeStallExecutionProcessor(10);
    }
    if (r.Generation != gen || r.Address != Addr || r.Result != MtPmicOk || r.Value > 0xFFFF ||
        (Write && r.Value != value)) {
        st = STATUS_UNSUCCESSFUL;
        goto out;
    }
    got = r.Value;
    for (;;) {                                  /* Ack */
        st = PmicInvoke(&prm, &r, MtPmicAck, gen, 0, 0);
        if (st != STATUS_DEVICE_BUSY)
            break;
        if (KeQueryInterruptTime() - start > 100000)
            goto out;
        KeStallExecutionProcessor(10);
    }
    if (st != STATUS_SUCCESS || r.State != MtPmicIdle || r.Generation != gen || r.Result != MtPmicOk) {
        st = STATUS_UNSUCCESSFUL;
        goto out;
    }
    pinned = FALSE;
    st = STATUS_SUCCESS;
out:
    if (locked && !pinned && prm.UnlockModule((GUID *)&MtPmicHandler) != STATUS_SUCCESS) {
        st = STATUS_UNSUCCESSFUL;
        pinned = TRUE;
    }
    if (NT_SUCCESS(st) && !Write && Out != NULL)
        *Out = (USHORT)got;
    InterlockedExchange(&MtPmicState, pinned ? 2 : 0);
    return st;
}

/* Set a rail's selector to the step at or above Uv, keeping the register's
 * other bits. */
static NTSTATUS RailSet(UINT32 Reg, ULONG Uv)
{
    USHORT v;
    ULONG sel = (Uv - 500000u + 6249u) / 6250u;
    NTSTATUS st;

    if (Uv < 500000u || sel > 0x7F)
        return STATUS_INVALID_PARAMETER;
    st = Pmic(FALSE, Reg, 0, &v);
    if (!NT_SUCCESS(st))
        return st;
    if ((v & 0x7F) == sel)
        return STATUS_SUCCESS;
    return Pmic(TRUE, Reg, (USHORT)((v & ~0x7Fu) | sel), NULL);
}

/* ---- the clock ---- */

static ULONG Rd(PUCHAR Base, ULONG Off)
{
    return READ_REGISTER_ULONG((PULONG)(Base + Off));
}

static void Wr(PUCHAR Base, ULONG Off, ULONG V)
{
    WRITE_REGISTER_ULONG((PULONG)(Base + Off), V);
}

static ULONG Con1For(ULONG Khz, UCHAR Posdiv)
{
    ULONG64 pcw = ((ULONG64)Khz * 1000ull << (14 + Posdiv)) / MT_REF_HZ;
    return (ULONG)Posdiv << 24 | (ULONG)(pcw & 0x3FFFFF);
}

static ULONG HzFromCon1(ULONG Con1)
{
    ULONG64 pcw = Con1 & 0x3FFFFF;
    return (ULONG)((pcw * MT_REF_HZ) >> (14 + ((Con1 >> 24) & 7)));
}

/* The GPU clock now in Hz (0 if not known), and MFGPLL_CON1 in *Raw. */
ULONG MwMt6789GpuHz(MW_ADAPTER *A, PULONG Raw)
{
    PHYSICAL_ADDRESS pa;
    PUCHAR ap = A->Mt.Apmixed, tc = A->Mt.Topckgen;
    ULONG con0, con1, cfg, hz = 0;

    if (ap == NULL) {
        pa.QuadPart = MT_APMIXED_PA;
        ap = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (tc == NULL) {
        pa.QuadPart = MT_TOPCKGEN_PA;
        tc = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (ap != NULL && tc != NULL) {
        con0 = Rd(ap, MT_MFGPLL_CON0);
        con1 = Rd(ap, MT_MFGPLL_CON1);
        cfg = Rd(tc, MT_CLK_CFG_4);
        if (Raw != NULL)
            *Raw = con1;
        if ((con0 & 1) && (cfg & MT_MFG_PLL_SEL))
            hz = HzFromCon1(con1);
    }
    if (ap != NULL && ap != A->Mt.Apmixed)
        MmUnmapIoSpace(ap, PAGE_SIZE);
    if (tc != NULL && tc != A->Mt.Topckgen)
        MmUnmapIoSpace(tc, PAGE_SIZE);
    return hz;
}

ULONG MwMt6789MaxHz(MW_ADAPTER *A)
{
    return A->Mt.GovOn ? MtOpp[0].Khz * 1000u : MwMt6789GpuHz(A, NULL);
}

/* Park on mfg_ref, reprogram MFGPLL, back on MFGPLL. PASSIVE_LEVEL. */
static BOOLEAN PllSet(MW_ADAPTER *A, ULONG Opp)
{
    PUCHAR ap = A->Mt.Apmixed, tc = A->Mt.Topckgen;

    Wr(tc, MT_CLK_CFG_4_CLR, MT_MFG_PLL_SEL);
    for (int i = 0; i < 10 && (Rd(tc, MT_CLK_CFG_4) & MT_MFG_PLL_SEL); i++)
        KeStallExecutionProcessor(10);
    Wr(ap, MT_MFGPLL_CON1, 1u << 31 | Con1For(MtOpp[Opp].Khz, MtOpp[Opp].Posdiv));
    KeStallExecutionProcessor(20);
    for (int i = 0; i < 6; i++) {
        Wr(tc, MT_CLK_CFG_4_SET, MT_MFG_PLL_SEL);
        KeStallExecutionProcessor(10);
        if (Rd(tc, MT_CLK_CFG_4) & MT_MFG_PLL_SEL)
            return TRUE;
    }
    return FALSE;
}

/* Rails up before the clock, the clock down before the rails. */
static NTSTATUS OppSet(MW_ADAPTER *A, ULONG To)
{
    ULONG from = A->Mt.Opp;
    ULONG vg = MtOpp[To].Vgpu * 10u, vs = MtOpp[To].Vsram * 10u;
    NTSTATUS st = STATUS_SUCCESS;

    if (To == from)
        return STATUS_SUCCESS;
    if (MtOpp[To].Khz > MtOpp[from].Khz) {
        st = RailSet(MT_PMIC_VSRAM_SEL, vs);
        if (NT_SUCCESS(st))
            st = RailSet(MT_PMIC_VGPU_SEL, vg);
        if (!NT_SUCCESS(st))
            return st;
        /* the buck ramps at about 10 mV/us; give it 1 us per mV and 50 more */
        KeStallExecutionProcessor(50 + (vg - MtOpp[from].Vgpu * 10u) / 1000u);
        if (!PllSet(A, To))
            return STATUS_DEVICE_HARDWARE_ERROR;
    } else {
        if (!PllSet(A, To))
            return STATUS_DEVICE_HARDWARE_ERROR;
        A->Mt.Opp = To;
        A->Mt.Changes++;
        st = RailSet(MT_PMIC_VGPU_SEL, vg);
        if (NT_SUCCESS(st))
            st = RailSet(MT_PMIC_VSRAM_SEL, vs);
        return st;          /* a rail left high is safe at a lower clock */
    }
    A->Mt.Opp = To;
    A->Mt.Changes++;
    return STATUS_SUCCESS;
}

/* The GPU's busy share of the last window, in percent. */
static ULONG Busy(MW_ADAPTER *A)
{
    ULONG64 now = MwTick(), job = A->JobTicks, run = 0, window = now - A->Mt.LastTick;

    if (A->Running && A->JobStartTick < now)
        run = now - (A->JobStartTick > A->Mt.LastTick ? A->JobStartTick : A->Mt.LastTick);
    job = job - A->Mt.LastJobTicks + run - A->Mt.LastRun;
    A->Mt.LastTick = now;
    A->Mt.LastJobTicks = A->JobTicks;
    A->Mt.LastRun = run;
    if (window == 0 || (LONG64)job < 0)
        return 0;
    return job >= window ? 100u : (ULONG)(job * 100u / window);
}

static VOID Governor(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    LARGE_INTEGER period;
    ULONG low = 0, idle = 0;

    period.QuadPart = -10000LL * MT_GOV_PERIOD_MS;
    a->Mt.LastTick = MwTick();
    a->Mt.LastJobTicks = a->JobTicks;
    while (KeWaitForSingleObject(&a->Mt.Stop, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        ULONG busy = Busy(a), cur = a->Mt.Opp, to = cur;

        if (InterlockedExchange(&a->Mt.FaultAsked, 0) != 0) {
            to = MT_OPP_LOWEST;
        } else if (busy > MT_GOV_UP_PCT) {
            ULONG64 want = (ULONG64)MtOpp[cur].Khz * busy / MT_GOV_AIM_PCT;
            to = cur > 0 ? cur - 1 : 0;                 /* at least one up */
            while (to > 0 && MtOpp[to].Khz < want)
                to--;
            low = idle = 0;
        } else if (busy == 0) {
            low = 0;
            if (++idle >= 4)
                to = MT_OPP_LOWEST;
        } else if (busy < MT_GOV_DOWN_PCT) {
            idle = 0;
            if (++low >= 3 && cur < MT_OPP_LOWEST) {
                to = cur + 1;
                low = 0;
            }
        } else {
            low = idle = 0;
        }
        if (to != cur && !NT_SUCCESS(OppSet(a, to))) {
            a->Mt.Fails++;
            if (a->Mt.Fails >= 3)
                break;                                  /* leave the clock where it is */
        }
        a->Mt.LastBusy = busy;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* The firmware's record: the vgpu it set (uV), 0 if it did not power the GPU. */
static ULONG RecordUv(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    volatile ULONG *r;
    ULONG uv = 0;

    pa.QuadPart = MT_RECORD_PA;
    r = (volatile ULONG *)MmMapIoSpaceEx(pa, MT_RECORD_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (r == NULL)
        return 0;
    if (r[0] == MT_RECORD_MAGIC && r[2] == 1) {
        ULONG w = r[0x44 / 4];                          /* vgpu after powering: [31] on, [30] read, [23:0] uV */
        if ((w & 0xC0000000u) == 0xC0000000u)
            uv = w & 0xFFFFFF;
    }
    MwLog(A, L"Mt_Record", r[0] == MT_RECORD_MAGIC ? r[2] : 0xFFFFFFFFu);
    MwLog(A, L"Mt_RecordGpuId", r[0x3C / 4]);
    MmUnmapIoSpace((PVOID)r, MT_RECORD_SIZE);
    return uv;
}

/* PASSIVE_LEVEL, once per start, after MkGpuInit. Returns the vgpu in uV
 * (0 not known). */
ULONG MwMt6789Start(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    ULONG raw = 0, hz, uv;
    HANDLE h;

    RtlZeroMemory(&A->Mt, sizeof(A->Mt));
    pa.QuadPart = MT_APMIXED_PA;
    A->Mt.Apmixed = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = MT_TOPCKGEN_PA;
    A->Mt.Topckgen = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    uv = RecordUv(A);
    hz = MwMt6789GpuHz(A, &raw);
    MwLog(A, L"Mt_GpuHz", hz);
    MwLog(A, L"Mt_MfgpllCon1", raw);
    /* where the clock is now on the table; the governor starts from there */
    A->Mt.Opp = MT_OPP_LOWEST;
    for (ULONG i = 0; i < ARRAYSIZE(MtOpp); i++)
        if (raw != 0 && (raw & 0x073FFFFFu) == Con1For(MtOpp[i].Khz, MtOpp[i].Posdiv))
            A->Mt.Opp = i;
    MwLog(A, L"Mt_Opp", A->Mt.Opp);
    if (MwSetting(A, L"Dvfs", 0) != 1 || !A->GpuUp || A->Mt.Apmixed == NULL || A->Mt.Topckgen == NULL)
        return uv;
    {
        USHORT v = 0;
        NTSTATUS st = Pmic(FALSE, MT_PMIC_VGPU_SEL, 0, &v);
        MwLog(A, L"Mt_PrmVgpu", NT_SUCCESS(st) ? v : (ULONG)st);
        if (!NT_SUCCESS(st))
            return uv;
        uv = MT_PMIC_UV(v & 0x7F);
    }
    KeInitializeEvent(&A->Mt.Stop, NotificationEvent, FALSE);
    if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, Governor, A))) {
        (void)ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&A->Mt.Thread, NULL);
        ZwClose(h);
        A->Mt.GovOn = A->Mt.Thread != NULL;
    }
    MwLog(A, L"Mt_Dvfs", A->Mt.GovOn);
    return uv;
}

/* From the interrupt after a job fault: the governor drops to the lowest point. */
void MwMt6789Fault(MW_ADAPTER *A)
{
    if (A->Mt.GovOn)
        InterlockedExchange(&A->Mt.FaultAsked, 1);
}

/* PASSIVE_LEVEL: the governor stopped, the firmware's point back. */
void MwMt6789Stop(MW_ADAPTER *A)
{
    if (A->Mt.Thread != NULL) {
        KeSetEvent(&A->Mt.Stop, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(A->Mt.Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(A->Mt.Thread);
        A->Mt.Thread = NULL;
        if (A->Mt.Opp != MT_OPP_LOWEST)
            (void)OppSet(A, MT_OPP_LOWEST);
        MwLog(A, L"Mt_DvfsChanges", A->Mt.Changes);
        MwLog(A, L"Mt_DvfsFails", A->Mt.Fails);
    }
    A->Mt.GovOn = FALSE;
    if (A->Mt.Apmixed != NULL)
        MmUnmapIoSpace(A->Mt.Apmixed, PAGE_SIZE);
    if (A->Mt.Topckgen != NULL)
        MmUnmapIoSpace(A->Mt.Topckgen, PAGE_SIZE);
    A->Mt.Apmixed = A->Mt.Topckgen = NULL;
}

#endif /* MALI_SOC_MT6789 */
