/*
 * IGD device quirks
 *
 * Copyright Red Hat, Inc. 2016
 *
 * Authors:
 *  Alex Williamson <alex.williamson@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/qmp/qerror.h"
#include "hw/hw.h"
#include "hw/nvram/fw_cfg.h"
#include "pci.h"
#include "pci-quirks.h"
#include "trace.h"

/*
 * Intel IGD support
 *
 * Obviously IGD is not a discrete device, this is evidenced not only by it
 * being integrated into the CPU, but by the various chipset and BIOS
 * dependencies that it brings along with it.  Intel is trying to move away
 * from this and Broadwell and newer devices can run in what Intel calls
 * "Universal Pass-Through" mode, or UPT.  Theoretically in UPT mode, nothing
 * more is required beyond assigning the IGD device to a VM.  There are
 * however support limitations to this mode.  It only supports IGD as a
 * secondary graphics device in the VM and it doesn't officially support any
 * physical outputs.
 *
 * The code here attempts to enable what we'll call legacy mode assignment,
 * IGD retains most of the capabilities we expect for it to have on bare
 * metal.  To enable this mode, the IGD device must be assigned to the VM
 * at PCI address 00:02.0, it must have a ROM, it very likely needs VGA
 * support, we must have VM BIOS support for reserving and populating some
 * of the required tables, and we need to tweak the chipset with revisions
 * and IDs and an LPC/ISA bridge device.  The intention is to make all of
 * this happen automatically by installing the device at the correct VM PCI
 * bus address.  If any of the conditions are not met, we cross our fingers
 * and hope the user knows better.
 *
 * NB - It is possible to enable physical outputs in UPT mode by supplying
 * an OpRegion table.  We don't do this by default because the guest driver
 * behaves differently if an OpRegion is provided and no monitor is attached
 * vs no OpRegion and a monitor being attached or not.  Effectively, if a
 * headless setup is desired, the OpRegion gets in the way of that.
 */

/*
 * This presumes the device is already known to be an Intel VGA device, so we
 * take liberties in which device ID bits match which generation.  This should
 * not be taken as an indication that all the devices are supported, or even
 * supportable, some of them don't even support VT-d.
 * See linux:include/drm/i915_pciids.h for IDs.
 */
static int igd_gen(VFIOPCIDevice *vdev)
{
    /*
     * Device IDs for Broxton/Apollo Lake are 0x0a84, 0x1a84, 0x1a85, 0x5a84
     * and 0x5a85, match bit 11:1 here
     * Prefix 0x0a is taken by Haswell, this rule should be matched first.
     */
    if ((vdev->device_id & 0xffe) == 0xa84) {
        return 9;
    }

    switch (vdev->device_id & 0xff00) {
    case 0x0100:    /* SandyBridge, IvyBridge */
        return 6;
    case 0x0400:    /* Haswell */
    case 0x0a00:    /* Haswell */
    case 0x0c00:    /* Haswell */
    case 0x0d00:    /* Haswell */
    case 0x0f00:    /* Valleyview/Bay Trail */
        return 7;
    case 0x1600:    /* Broadwell */
    case 0x2200:    /* Cherryview */
        return 8;
    case 0x1900:    /* Skylake */
    case 0x3100:    /* Gemini Lake */
    case 0x5900:    /* Kaby Lake */
    case 0x3e00:    /* Coffee Lake */
    case 0x9B00:    /* Comet Lake */
        return 9;
    case 0x8A00:    /* Ice Lake */
    case 0x4500:    /* Elkhart Lake */
    case 0x4E00:    /* Jasper Lake */
        return 11;
    case 0x9A00:    /* Tiger Lake */
    case 0x4C00:    /* Rocket Lake */
    case 0x4600:    /* Alder Lake */
    case 0xA700:    /* Raptor Lake */
        return 12;
    }

    /*
     * Unfortunately, Intel changes it's specification quite often. This makes
     * it impossible to use a suitable default value for unknown devices.
     */
    return -1;
}

#define IGD_ASLS 0xfc /* ASL Storage Register */
#define IGD_GMCH 0x50 /* Graphics Control Register */
#define IGD_BDSM 0x5c /* Base Data of Stolen Memory */
#define IGD_BDSM_GEN11 0xc0 /* Base Data of Stolen Memory of gen 11 and later */

#define IGD_GMCH_GEN6_GMS_SHIFT     3       /* SNB_GMCH in i915 */
#define IGD_GMCH_GEN6_GMS_MASK      0x1f
#define IGD_GMCH_GEN8_GMS_SHIFT     8       /* BDW_GMCH in i915 */
#define IGD_GMCH_GEN8_GMS_MASK      0xff

static uint64_t igd_stolen_memory_size(int gen, uint32_t gmch)
{
    uint64_t gms;

    if (gen < 8) {
        gms = (gmch >> IGD_GMCH_GEN6_GMS_SHIFT) & IGD_GMCH_GEN6_GMS_MASK;
    } else {
        gms = (gmch >> IGD_GMCH_GEN8_GMS_SHIFT) & IGD_GMCH_GEN8_GMS_MASK;
    }

    if (gen < 9) {
            return gms * 32 * MiB;
    } else {
        if (gms < 0xf0) {
            return gms * 32 * MiB;
        } else {
            return (gms - 0xf0 + 1) * 4 * MiB;
        }
    }

    return 0;
}

/*
 * The OpRegion includes the Video BIOS Table, which seems important for
 * telling the driver what sort of outputs it has.  Without this, the device
 * may work in the guest, but we may not get output.  This also requires BIOS
 * support to reserve and populate a section of guest memory sufficient for
 * the table and to write the base address of that memory to the ASLS register
 * of the IGD device.
 */
static bool vfio_pci_igd_opregion_init(VFIOPCIDevice *vdev,
                                       struct vfio_region_info *info,
                                       Error **errp)
{
    int ret;

    vdev->igd_opregion = g_malloc0(info->size);
    ret = pread(vdev->vbasedev.fd, vdev->igd_opregion,
                info->size, info->offset);
    if (ret != info->size) {
        error_setg(errp, "failed to read IGD OpRegion");
        g_free(vdev->igd_opregion);
        vdev->igd_opregion = NULL;
        return false;
    }

    /*
     * Provide fw_cfg with a copy of the OpRegion which the VM firmware is to
     * allocate 32bit reserved memory for, copy these contents into, and write
     * the reserved memory base address to the device ASLS register at 0xFC.
     * Alignment of this reserved region seems flexible, but using a 4k page
     * alignment seems to work well.  This interface assumes a single IGD
     * device, which may be at VM address 00:02.0 in legacy mode or another
     * address in UPT mode.
     *
     * NB, there may be future use cases discovered where the VM should have
     * direct interaction with the host OpRegion, in which case the write to
     * the ASLS register would trigger MemoryRegion setup to enable that.
     */
    fw_cfg_add_file(fw_cfg_find(), "etc/igd-opregion",
                    vdev->igd_opregion, info->size);

    trace_vfio_pci_igd_opregion_enabled(vdev->vbasedev.name);

    pci_set_long(vdev->pdev.config + IGD_ASLS, 0);
    pci_set_long(vdev->pdev.wmask + IGD_ASLS, ~0);
    pci_set_long(vdev->emulated_config_bits + IGD_ASLS, ~0);

    return true;
}

bool vfio_pci_igd_setup_opregion(VFIOPCIDevice *vdev, Error **errp)
{
    g_autofree struct vfio_region_info *opregion = NULL;
    int ret;

    /* Hotplugging is not supported for opregion access */
    if (vdev->pdev.qdev.hotplugged) {
        error_setg(errp, "IGD OpRegion is not supported on hotplugged device");
        return false;
    }

    ret = vfio_get_dev_region_info(&vdev->vbasedev,
                    VFIO_REGION_TYPE_PCI_VENDOR_TYPE | PCI_VENDOR_ID_INTEL,
                    VFIO_REGION_SUBTYPE_INTEL_IGD_OPREGION, &opregion);
    if (ret) {
        error_setg_errno(errp, -ret,
                         "Device does not supports IGD OpRegion feature");
        return false;
    }

    if (!vfio_pci_igd_opregion_init(vdev, opregion, errp)) {
        return false;
    }

    return true;
}

/*
 * The rather short list of registers that we copy from the host devices.
 * The LPC/ISA bridge values are definitely needed to support the vBIOS, the
 * host bridge values may or may not be needed depending on the guest OS.
 * Since we're only munging revision and subsystem values on the host bridge,
 * we don't require our own device.  The LPC/ISA bridge needs to be our very
 * own though.
 */
typedef struct {
    uint8_t offset;
    uint8_t len;
} IGDHostInfo;

static const IGDHostInfo igd_host_bridge_infos[] = {
    {PCI_REVISION_ID,         2},
    {PCI_SUBSYSTEM_VENDOR_ID, 2},
    {PCI_SUBSYSTEM_ID,        2},
};

static const IGDHostInfo igd_lpc_bridge_infos[] = {
    {PCI_VENDOR_ID,           2},
    {PCI_DEVICE_ID,           2},
    {PCI_REVISION_ID,         2},
    {PCI_SUBSYSTEM_VENDOR_ID, 2},
    {PCI_SUBSYSTEM_ID,        2},
};

static int vfio_pci_igd_copy(VFIOPCIDevice *vdev, PCIDevice *pdev,
                             struct vfio_region_info *info,
                             const IGDHostInfo *list, int len)
{
    int i, ret;

    for (i = 0; i < len; i++) {
        ret = pread(vdev->vbasedev.fd, pdev->config + list[i].offset,
                    list[i].len, info->offset + list[i].offset);
        if (ret != list[i].len) {
            error_report("IGD copy failed: %m");
            return -errno;
        }
    }

    return 0;
}

/*
 * Stuff a few values into the host bridge.
 */
static int vfio_pci_igd_host_init(VFIOPCIDevice *vdev,
                                  struct vfio_region_info *info)
{
    PCIBus *bus;
    PCIDevice *host_bridge;
    int ret;

    bus = pci_device_root_bus(&vdev->pdev);
    host_bridge = pci_find_device(bus, 0, PCI_DEVFN(0, 0));

    if (!host_bridge) {
        error_report("Can't find host bridge");
        return -ENODEV;
    }

    ret = vfio_pci_igd_copy(vdev, host_bridge, info, igd_host_bridge_infos,
                            ARRAY_SIZE(igd_host_bridge_infos));
    if (!ret) {
        trace_vfio_pci_igd_host_bridge_enabled(vdev->vbasedev.name);
    }

    return ret;
}

/*
 * IGD LPC/ISA bridge support code.  The vBIOS needs this, but we can't write
 * arbitrary values into just any bridge, so we must create our own.  We try
 * to handle if the user has created it for us, which they might want to do
 * to enable multifunction so we don't occupy the whole PCI slot.
 */
static void vfio_pci_igd_lpc_bridge_realize(PCIDevice *pdev, Error **errp)
{
    if (pdev->devfn != PCI_DEVFN(0x1f, 0)) {
        error_setg(errp, "VFIO dummy ISA/LPC bridge must have address 1f.0");
    }
}

static void vfio_pci_igd_lpc_bridge_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->desc = "VFIO dummy ISA/LPC bridge for IGD assignment";
    dc->hotpluggable = false;
    k->realize = vfio_pci_igd_lpc_bridge_realize;
    k->class_id = PCI_CLASS_BRIDGE_ISA;
}

static const TypeInfo vfio_pci_igd_lpc_bridge_info = {
    .name = "vfio-pci-igd-lpc-bridge",
    .parent = TYPE_PCI_DEVICE,
    .class_init = vfio_pci_igd_lpc_bridge_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void vfio_pci_igd_register_types(void)
{
    type_register_static(&vfio_pci_igd_lpc_bridge_info);
}

type_init(vfio_pci_igd_register_types)

static int vfio_pci_igd_lpc_init(VFIOPCIDevice *vdev,
                                 struct vfio_region_info *info)
{
    PCIDevice *lpc_bridge;
    int ret;

    lpc_bridge = pci_find_device(pci_device_root_bus(&vdev->pdev),
                                 0, PCI_DEVFN(0x1f, 0));
    if (!lpc_bridge) {
        lpc_bridge = pci_create_simple(pci_device_root_bus(&vdev->pdev),
                                 PCI_DEVFN(0x1f, 0), "vfio-pci-igd-lpc-bridge");
    }

    ret = vfio_pci_igd_copy(vdev, lpc_bridge, info, igd_lpc_bridge_infos,
                            ARRAY_SIZE(igd_lpc_bridge_infos));
    if (!ret) {
        trace_vfio_pci_igd_lpc_bridge_enabled(vdev->vbasedev.name);
    }

    return ret;
}

static bool vfio_pci_igd_setup_lpc_bridge(VFIOPCIDevice *vdev, Error **errp)
{
    g_autofree struct vfio_region_info *host = NULL;
    g_autofree struct vfio_region_info *lpc = NULL;
    PCIDevice *lpc_bridge;
    int ret;

    /*
     * Copying IDs or creating new devices are not supported on hotplug
     */
    if (vdev->pdev.qdev.hotplugged) {
        error_setg(errp, "IGD LPC is not supported on hotplugged device");
        return false;
    }

    /*
     * We need to create an LPC/ISA bridge at PCI bus address 00:1f.0 that we
     * can stuff host values into, so if there's already one there and it's not
     * one we can hack on, this quirk is no-go.  Sorry Q35.
     */
    lpc_bridge = pci_find_device(pci_device_root_bus(&vdev->pdev),
                                 0, PCI_DEVFN(0x1f, 0));
    if (lpc_bridge && !object_dynamic_cast(OBJECT(lpc_bridge),
                                           "vfio-pci-igd-lpc-bridge")) {
        error_setg(errp,
                   "Cannot create LPC bridge due to existing device at 1f.0");
        return false;
    }

    /*
     * Check whether we have all the vfio device specific regions to
     * support LPC quirk (added in Linux v4.6).
     */
    ret = vfio_get_dev_region_info(&vdev->vbasedev,
                        VFIO_REGION_TYPE_PCI_VENDOR_TYPE | PCI_VENDOR_ID_INTEL,
                        VFIO_REGION_SUBTYPE_INTEL_IGD_LPC_CFG, &lpc);
    if (ret) {
        error_setg(errp, "IGD LPC bridge access is not supported by kernel");
        return false;
    }

    ret = vfio_get_dev_region_info(&vdev->vbasedev,
                        VFIO_REGION_TYPE_PCI_VENDOR_TYPE | PCI_VENDOR_ID_INTEL,
                        VFIO_REGION_SUBTYPE_INTEL_IGD_HOST_CFG, &host);
    if (ret) {
        error_setg(errp, "IGD host bridge access is not supported by kernel");
        return false;
    }

    /* Create/modify LPC bridge */
    ret = vfio_pci_igd_lpc_init(vdev, lpc);
    if (ret) {
        error_setg(errp, "Failed to create/modify LPC bridge for IGD");
        return false;
    }

    /* Stuff some host values into the VM PCI host bridge */
    ret = vfio_pci_igd_host_init(vdev, host);
    if (ret) {
        error_setg(errp, "Failed to modify host bridge for IGD");
        return false;
    }

    return true;
}

#define IGD_GGC_MMIO_OFFSET     0x108040
#define IGD_BDSM_MMIO_OFFSET    0x1080C0

void vfio_probe_igd_bar0_quirk(VFIOPCIDevice *vdev, int nr)
{
    VFIOQuirk *ggc_quirk, *bdsm_quirk;
    VFIOConfigMirrorQuirk *ggc_mirror, *bdsm_mirror;
    int gen;

    /*
     * This must be an Intel VGA device at address 00:02.0 for us to even
     * consider enabling legacy mode. Some driver have dependencies on the PCI
     * bus address.
     */
    if (!vfio_pci_is(vdev, PCI_VENDOR_ID_INTEL, PCI_ANY_ID) ||
        !vfio_is_vga(vdev) || nr != 0 ||
        &vdev->pdev != pci_find_device(pci_device_root_bus(&vdev->pdev),
                                       0, PCI_DEVFN(0x2, 0))) {
        return;
    }

    /*
     * Only on IGD devices of gen 11 and above, the BDSM register is mirrored
     * into MMIO space and read from MMIO space by the Windows driver.
     */
    gen = igd_gen(vdev);
    if (gen < 6) {
        return;
    }

    ggc_quirk = vfio_quirk_alloc(1);
    ggc_mirror = ggc_quirk->data = g_malloc0(sizeof(*ggc_mirror));
    ggc_mirror->mem = ggc_quirk->mem;
    ggc_mirror->vdev = vdev;
    ggc_mirror->bar = nr;
    ggc_mirror->offset = IGD_GGC_MMIO_OFFSET;
    ggc_mirror->config_offset = IGD_GMCH;

    memory_region_init_io(ggc_mirror->mem, OBJECT(vdev),
                          &vfio_generic_mirror_quirk, ggc_mirror,
                          "vfio-igd-ggc-quirk", 2);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        ggc_mirror->offset, ggc_mirror->mem,
                                        1);

    QLIST_INSERT_HEAD(&vdev->bars[nr].quirks, ggc_quirk, next);

    bdsm_quirk = vfio_quirk_alloc(1);
    bdsm_mirror = bdsm_quirk->data = g_malloc0(sizeof(*bdsm_mirror));
    bdsm_mirror->mem = bdsm_quirk->mem;
    bdsm_mirror->vdev = vdev;
    bdsm_mirror->bar = nr;
    bdsm_mirror->offset = IGD_BDSM_MMIO_OFFSET;
    bdsm_mirror->config_offset = (gen < 11) ? IGD_BDSM : IGD_BDSM_GEN11;

    memory_region_init_io(bdsm_mirror->mem, OBJECT(vdev),
                          &vfio_generic_mirror_quirk, bdsm_mirror,
                          "vfio-igd-bdsm-quirk", (gen < 11) ? 4 : 8);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        bdsm_mirror->offset, bdsm_mirror->mem,
                                        1);

    QLIST_INSERT_HEAD(&vdev->bars[nr].quirks, bdsm_quirk, next);
}

void vfio_probe_igd_bar4_quirk(VFIOPCIDevice *vdev, int nr)
{
    g_autofree struct vfio_region_info *rom = NULL;
    int ret, gen;
    uint64_t gms_size;
    uint64_t *bdsm_size;
    uint32_t gmch;
    Error *err = NULL;

    /*
     * This must be an Intel VGA device at address 00:02.0 for us to even
     * consider enabling legacy mode.  The vBIOS has dependencies on the
     * PCI bus address.
     */
    if (!vfio_pci_is(vdev, PCI_VENDOR_ID_INTEL, PCI_ANY_ID) ||
        !vfio_is_vga(vdev) || nr != 4 ||
        &vdev->pdev != pci_find_device(pci_device_root_bus(&vdev->pdev),
                                       0, PCI_DEVFN(0x2, 0))) {
        return;
    }

    /*
     * IGD is not a standard, they like to change their specs often.  We
     * only attempt to support back to SandBridge and we hope that newer
     * devices maintain compatibility with generation 8.
     */
    gen = igd_gen(vdev);
    if (gen == -1) {
        error_report("IGD device %s is unsupported in legacy mode, "
                     "try SandyBridge or newer", vdev->vbasedev.name);
        return;
    }

    /*
     * Most of what we're doing here is to enable the ROM to run, so if
     * there's no ROM, there's no point in setting up this quirk.
     * NB. We only seem to get BIOS ROMs, so a UEFI VM would need CSM support.
     */
    ret = vfio_get_region_info(&vdev->vbasedev,
                               VFIO_PCI_ROM_REGION_INDEX, &rom);
    if ((ret || !rom->size) && !vdev->pdev.romfile) {
        error_report("IGD device %s has no ROM, legacy mode disabled",
                     vdev->vbasedev.name);
        return;
    }

    /*
     * Ignore the hotplug corner case, mark the ROM failed, we can't
     * create the devices we need for legacy mode in the hotplug scenario.
     */
    if (vdev->pdev.qdev.hotplugged) {
        error_report("IGD device %s hotplugged, ROM disabled, "
                     "legacy mode disabled", vdev->vbasedev.name);
        vdev->rom_read_failed = true;
        return;
    }

    gmch = vfio_pci_read_config(&vdev->pdev, IGD_GMCH, 4);

    /*
     * If IGD VGA Disable is clear (expected) and VGA is not already enabled,
     * try to enable it.  Probably shouldn't be using legacy mode without VGA,
     * but also no point in us enabling VGA if disabled in hardware.
     */
    if (!(gmch & 0x2) && !vdev->vga && !vfio_populate_vga(vdev, &err)) {
        error_reportf_err(err, VFIO_MSG_PREFIX, vdev->vbasedev.name);
        error_report("IGD device %s failed to enable VGA access, "
                     "legacy mode disabled", vdev->vbasedev.name);
        return;
    }

    /* Setup OpRegion access */
    if (!vfio_pci_igd_setup_opregion(vdev, &err)) {
        error_append_hint(&err, "IGD legacy mode disabled\n");
        error_report_err(err);
        return;
    }

    /* Setup LPC bridge / Host bridge PCI IDs */
    if (!vfio_pci_igd_setup_lpc_bridge(vdev, &err)) {
        error_append_hint(&err, "IGD legacy mode disabled\n");
        error_report_err(err);
        return;
    }

    /*
     * Allow user to override dsm size using x-igd-gms option, in multiples of
     * 32MiB. This option should only be used when the desired size cannot be
     * set from DVMT Pre-Allocated option in host BIOS.
     */
    if (vdev->igd_gms) {
        if (gen < 8) {
            if (vdev->igd_gms <= 0x10) {
                gmch &= ~(IGD_GMCH_GEN6_GMS_MASK << IGD_GMCH_GEN6_GMS_SHIFT);
                gmch |= vdev->igd_gms << IGD_GMCH_GEN6_GMS_SHIFT;
            } else {
                error_report(QERR_INVALID_PARAMETER_VALUE,
                             "x-igd-gms", "0~0x10");
            }
        } else {
            if (vdev->igd_gms <= 0x40) {
                gmch &= ~(IGD_GMCH_GEN8_GMS_MASK << IGD_GMCH_GEN8_GMS_SHIFT);
                gmch |= vdev->igd_gms << IGD_GMCH_GEN8_GMS_SHIFT;
            } else {
                error_report(QERR_INVALID_PARAMETER_VALUE,
                             "x-igd-gms", "0~0x40");
            }
        }
    }

    gms_size = igd_stolen_memory_size(gen, gmch);

    /*
     * Request reserved memory for stolen memory via fw_cfg.  VM firmware
     * must allocate a 1MB aligned reserved memory region below 4GB with
     * the requested size (in bytes) for use by the Intel PCI class VGA
     * device at VM address 00:02.0.  The base address of this reserved
     * memory region must be written to the device BDSM register at PCI
     * config offset 0x5C.
     */
    bdsm_size = g_malloc(sizeof(*bdsm_size));
    *bdsm_size = cpu_to_le64(gms_size);
    fw_cfg_add_file(fw_cfg_find(), "etc/igd-bdsm-size",
                    bdsm_size, sizeof(*bdsm_size));

    /* GMCH is read-only, emulated */
    pci_set_long(vdev->pdev.config + IGD_GMCH, gmch);
    pci_set_long(vdev->pdev.wmask + IGD_GMCH, 0);
    pci_set_long(vdev->emulated_config_bits + IGD_GMCH, ~0);

    /* BDSM is read-write, emulated.  The BIOS needs to be able to write it */
    if (gen < 11) {
        pci_set_long(vdev->pdev.config + IGD_BDSM, 0);
        pci_set_long(vdev->pdev.wmask + IGD_BDSM, ~0);
        pci_set_long(vdev->emulated_config_bits + IGD_BDSM, ~0);
    } else {
        pci_set_quad(vdev->pdev.config + IGD_BDSM_GEN11, 0);
        pci_set_quad(vdev->pdev.wmask + IGD_BDSM_GEN11, ~0);
        pci_set_quad(vdev->emulated_config_bits + IGD_BDSM_GEN11, ~0);
    }

    trace_vfio_pci_igd_bdsm_enabled(vdev->vbasedev.name, (gms_size / MiB));
}
