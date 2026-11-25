// SPDX-License-Identifier: GPL-2.0
/* Scan-out on the MediaTek MT6789 (Teclast T70 Pro): the scanout.c API for
 * a display whose overlay engine reads physical addresses.
 *
 * The firmware (LK, then the Mu-Silicium GOP) leaves the panel running in
 * DSI video mode: OVL0 blends one layer from the GOP framebuffer, RDMA0 is
 * fed directly by OVL0, and the display M4U is bypassed (LARB port bit 0
 * clear), so OVL reads physical addresses, 32 bits of them. OVL_EN bit 22
 * (shadow bypass) is set, so a layer address written takes effect at once.
 * There is no IOMMU here to make a primary's scattered pages look
 * contiguous, as the RK3576 build does with the VOP2's.
 *
 * So a primary is shown the way the Pixel 6 port (isaacjamessss,
 * pixel6-woa-drivers maliwddm 0.9.11) shows it on DECON: when VidMm maps a
 * primary of exactly the framebuffer's shape, its GPU addresses are pointed
 * at a physically contiguous framebuffer instead of VidMm's pages, the GPU
 * draws there, and a flip only tells the overlay which framebuffer to read.
 *
 * The ScanOut setting (Device Parameters, read at start) chooses how far
 * this goes:
 *   0  (default) nothing here: every flip is copied by the CPU into the
 *      GOP framebuffer (command.c). No display register is read or written.
 *   1  one framebuffer: the first primary of the right shape draws straight
 *      into the GOP framebuffer A. No display register is written; frames
 *      can tear, the others' flips are still copied.
 *   2  two framebuffers: A and B, a contiguous buffer below 4 GB taken now,
 *      and a flip writes the OVL layer's address. Used only if one enabled
 *      layer of OVL0 or OVL0_2L reads A with A's pitch; otherwise as 1.
 *
 * What a primary is aliased to is kept in its ScanIova (slot + 1, 0 none);
 * ScanSlot[] holds each framebuffer's owner. */
#include "maliwddm.h"

#ifdef MALI_SOC_MT6789

#define OVL0_PA                 0x14005000u     /* 4 layers */
#define OVL0_2L_PA              0x14006000u     /* 2 layers */
#define OVL_SRC_CON             0x02Cu          /* bits 0-3: layer enable */
#define OVL_L_PITCH(l)          (0x044u + (l) * 0x20u)  /* [15:0] pitch */
#define OVL_L_ADDR(l)           (0xF40u + (l) * 0x20u)
#define MT_FB_SLOTS             2

static __inline ULONG OvlRd(MW_ADAPTER *A, ULONG Off)
{
    return READ_REGISTER_ULONG((PULONG)(A->VopVa + Off));
}

static __inline void OvlWr(MW_ADAPTER *A, ULONG Off, ULONG V)
{
    WRITE_REGISTER_ULONG((PULONG)(A->VopVa + Off), V);
}

static ULONG64 SlotPa(MW_ADAPTER *A, ULONG Slot)
{
    return Slot == 0 ? (ULONG64)A->Fb.PhysicAddress.QuadPart : (ULONG64)A->ScanTablesPa.QuadPart;
}

static ULONG Slots(MW_ADAPTER *A)
{
    return A->ScanPaging ? 2u : 1u;
}

/* Show framebuffer Slot. Only with two framebuffers and a verified layer. */
static void SetSlot(MW_ADAPTER *A, ULONG Slot)
{
    if (!A->ScanPaging || A->VopVa == NULL)
        return;
    KeMemoryBarrier();
    OvlWr(A, OVL_L_ADDR(A->ScanTablesUsed), (ULONG)SlotPa(A, Slot));
}

/* The overlay and layer reading A: one enabled layer whose address is A's
 * and whose pitch is A's. Registers only read. */
static BOOLEAN FindLayer(MW_ADAPTER *A, ULONG Pa, PULONG Layer)
{
    PHYSICAL_ADDRESS pa;
    static const struct { ULONG Pa, Layers; } ovl[] = { { OVL0_PA, 4 }, { OVL0_2L_PA, 2 } };

    for (ULONG o = 0; o < ARRAYSIZE(ovl); o++) {
        PUCHAR va;
        ULONG en, found = 0, layer = 0;

        pa.QuadPart = ovl[o].Pa;
        va = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (va == NULL)
            continue;
        A->VopVa = va;
        en = OvlRd(A, OVL_SRC_CON);
        MwLog(A, o == 0 ? L"Mt_Ovl0SrcCon" : L"Mt_Ovl2lSrcCon", en);
        for (ULONG l = 0; l < ovl[o].Layers; l++) {
            if (!(en & (1u << l)))
                continue;
            MwLog(A, L"Mt_LayerAddr", OvlRd(A, OVL_L_ADDR(l)));
            if (OvlRd(A, OVL_L_ADDR(l)) == Pa && (OvlRd(A, OVL_L_PITCH(l)) & 0xFFFF) == A->Fb.Pitch) {
                found++;
                layer = l;
            }
        }
        if (found == 1) {
            *Layer = layer;
            MwLog(A, L"Mt_Ovl", ovl[o].Pa);
            MwLog(A, L"Mt_Layer", layer);
            return TRUE;
        }
        A->VopVa = NULL;
        MmUnmapIoSpace(va, PAGE_SIZE);
    }
    return FALSE;
}

/* PASSIVE_LEVEL, once the GOP framebuffer is ours (display.c). */
void MwScanStart(MW_ADAPTER *A)
{
    ULONG64 fb = (ULONG64)A->Fb.PhysicAddress.QuadPart;
    ULONG mode = MwSetting(A, L"ScanOut", 0);

    A->ScanReady = FALSE;
    A->ScanPaging = FALSE;
    A->ScanShowing = FALSE;
    A->ScanOn = 0;
    A->VopVa = NULL;
    A->ScanTables = NULL;
    A->ScanTablesPa.QuadPart = 0;
    for (ULONG i = 0; i < MW_SCAN_SLOTS; i++)
        A->ScanSlot[i] = NULL;
    A->ScanSetting = mode != 0;
    MwLog(A, L"ScanOutSetting", mode);
    if (mode == 0)
        return;
    if (A->FbVa == NULL || A->FbSize == 0 || fb + A->FbSize > 0x100000000ull || A->Fb.Pitch == 0 ||
        (A->Fb.ColorFormat != D3DDDIFMT_X8R8G8B8 && A->Fb.ColorFormat != D3DDDIFMT_A8R8G8B8)) {
        MwLog(A, L"Fail_ScanFb", (ULONG)(fb >> 12));
        return;
    }
    A->ScanReady = TRUE;
    A->ScanOn = 1;
    if (mode >= 2) {
        ULONG layer;
        if (FindLayer(A, (ULONG)fb, &layer)) {
            PHYSICAL_ADDRESS lo, hi, skip;
            lo.QuadPart = 0x40000000ll;         /* DRAM; OVL takes 32-bit addresses */
            hi.QuadPart = 0xFFFFFFFFll;
            skip.QuadPart = 0;
            A->ScanTables = MmAllocateContiguousMemorySpecifyCache(A->FbSize, lo, hi, skip, MmNonCached);
            if (A->ScanTables != NULL) {
                RtlCopyMemory(A->ScanTables, A->FbVa, A->FbSize);
                A->ScanTablesPa = MmGetPhysicalAddress(A->ScanTables);
                A->ScanTablesUsed = layer;      /* the layer to write */
                A->ScanPaging = TRUE;
            } else {
                MmUnmapIoSpace(A->VopVa, PAGE_SIZE);
                A->VopVa = NULL;
            }
        }
        MwLog(A, L"Mt_FbB", A->ScanPaging ? (ULONG)(A->ScanTablesPa.QuadPart >> 12) : 0);
    }
    MwLog(A, L"ScanOutAtStart", A->ScanPaging ? 2u : 1u);
}

void MwScanStop(MW_ADAPTER *A)
{
    if (A->ScanPaging) {
        SetSlot(A, 0);                          /* the GOP framebuffer, for whoever comes next */
        A->ScanPaging = FALSE;
    }
    if (A->VopVa != NULL) {
        MmUnmapIoSpace(A->VopVa, PAGE_SIZE);
        A->VopVa = NULL;
    }
    if (A->ScanTables != NULL && A->ScanSlot[1] == NULL) {
        MmFreeContiguousMemorySpecifyCache(A->ScanTables, A->FbSize, MmNonCached);
        A->ScanTables = NULL;
    }
    A->ScanReady = FALSE;
    A->ScanOn = 0;
    A->ScanShowing = FALSE;
}

/* The CPU is about to draw into A (a blt, a copied flip, the bugcheck
 * screen): show A. Any IRQL. */
void MwScanToFb(MW_ADAPTER *A)
{
    if (!A->ScanReady)
        return;
    if (A->ScanShowing)
        A->ScanToFbs++;
    SetSlot(A, 0);
    A->ScanShowing = FALSE;
}

ULONG MwScanWhy(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    if (!A->ScanReady || !A->ScanOn)
        return MW_SCAN_WHY_OFF;
    if (P == NULL || P->ScanIova == 0 || !P->Mapped)
        return MW_SCAN_WHY_NOT_MAPPED;
    if (MwFlipDegrees(A, P) != 0)
        return MW_SCAN_WHY_ROTATED;
    return 0;
}

BOOLEAN MwScanUsable(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    return MwScanWhy(A, P) == 0;
}

void MwScanCheck(MW_ADAPTER *A)
{
    UNREFERENCED_PARAMETER(A);
}

ULONG MwScanMmuStatus(MW_ADAPTER *A, ULONG Which, ULONG *FaultAddr)
{
    UNREFERENCED_PARAMETER(A);
    UNREFERENCED_PARAMETER(Which);
    *FaultAddr = 0;
    return 0xFFFFFFFFu;
}

/* Leaving D0: the GOP framebuffer shown, for the firmware or Basic
 * Display. PASSIVE_LEVEL. */
void MwScanPower(MW_ADAPTER *A, BOOLEAN On)
{
    if (!On)
        MwScanToFb(A);
}

/* A flip of an aliased primary: the GPU already drew its framebuffer. */
BOOLEAN MwScanFlip(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    if (!MwScanUsable(A, P))
        return FALSE;
    SetSlot(A, P->ScanIova - 1);
    A->ScanShowing = TRUE;
    A->ScanFlips++;
    return TRUE;
}

/* No scaler is set up on the MT6789: only the firmware's own size. */
BOOLEAN MwScanScale(MW_ADAPTER *A, UINT Width, UINT Height, UINT How)
{
    UNREFERENCED_PARAMETER(How);
    return Width == A->Fb.Width && Height == A->Fb.Height;
}

void MwScanNativeWindow(MW_ADAPTER *A)
{
    UNREFERENCED_PARAMETER(A);
}

/* VidMm has given a primary its pages and they are mapped at its GpuVa.
 * Under MmuLock. A primary of the framebuffer's exact shape is pointed at a
 * free framebuffer instead; the others keep their pages and are copied. */
void MwScanMap(MW_ADAPTER *A, MW_ALLOCATION *P, PPFN_NUMBER Pfn, ULONG Pages)
{
    PPFN_NUMBER fb;
    PFN_NUMBER base;
    ULONG n, slot, slots = Slots(A);

    UNREFERENCED_PARAMETER(Pfn);
    if (!A->ScanReady || !A->ScanOn || !(P->Flags & MW_ALLOC_PRIMARY))
        return;
    if (P->Width != A->Fb.Width || P->Height != A->Fb.Height || P->Pitch != A->Fb.Pitch) {
        A->ScanMismatch++;
        A->ScanLastW = P->Width;
        A->ScanLastH = P->Height;
        A->ScanLastPitch = P->Pitch;
        return;
    }
    n = (ULONG)((A->FbSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
    if (n > Pages)
        return;
    for (slot = 0; slot < slots && A->ScanSlot[slot] != P; slot++)
        ;
    if (slot == slots)
        for (slot = 0; slot < slots && A->ScanSlot[slot] != NULL; slot++)
            ;
    if (slot == slots) {
        A->ScanNoSlot++;
        return;
    }
    fb = (PPFN_NUMBER)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)n * sizeof(PFN_NUMBER), MW_TAG);
    if (fb == NULL)
        return;
    base = (PFN_NUMBER)(SlotPa(A, slot) >> PAGE_SHIFT);
    for (ULONG i = 0; i < n; i++)
        fb[i] = base + i;
    if (NT_SUCCESS(MkMmuMapPages(&A->Gpu, P->GpuVa, fb, n, TRUE))) {
        A->ScanSlot[slot] = P;
        P->ScanIova = slot + 1;
        A->ScanMaps++;
    }
    ExFreePoolWithTag(fb, MW_TAG);
}

/* Before VidMm takes the pages back, or the allocation goes. Under MmuLock. */
void MwScanUnmap(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    if (P->ScanIova != 0 && P->ScanIova <= MT_FB_SLOTS && A->ScanSlot[P->ScanIova - 1] == P)
        A->ScanSlot[P->ScanIova - 1] = NULL;
    P->ScanIova = 0;
}

/* MW_ESCAPE_SCANOUT: Mode 0 reads, 1 turns aliasing on, 2 off. */
NTSTATUS MwScanSet(MW_ADAPTER *A, MW_ESCAPE_SCANOUT_DATA *D)
{
    if (D->Mode == 1 && A->ScanReady)
        InterlockedExchange(&A->ScanOn, 1);
    else if (D->Mode == 2) {
        InterlockedExchange(&A->ScanOn, 0);
        MwScanToFb(A);
    }
    D->Status = A->ScanReady ? ((A->ScanOn ? 1u : 0) | (A->ScanPaging ? 2u : 0) | (A->ScanShowing ? 4u : 0))
                             : 0xFFFFFFFFu;
    D->Mmu0Status = D->Mmu1Status = 0xFFFFFFFFu;
    D->Flips = A->ScanFlips;
    D->Maps = A->ScanMaps;
    D->ToFbs = A->ScanToFbs;
    D->Mismatch = A->ScanMismatch;
    D->NoSlot = A->ScanNoSlot;
    D->FbPa = A->Fb.PhysicAddress.LowPart;
    D->Faults = 0;
    D->FaultAddr = 0;
    return STATUS_SUCCESS;
}

#endif /* MALI_SOC_MT6789 */
