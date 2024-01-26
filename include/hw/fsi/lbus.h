/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2024 IBM Corp.
 *
 * IBM Local bus and connected device structures.
 */
#ifndef FSI_LBUS_H
#define FSI_LBUS_H

#include "hw/qdev-core.h"
#include "qemu/units.h"
#include "exec/memory.h"

#define TYPE_FSI_LBUS_DEVICE "fsi.lbus.device"
OBJECT_DECLARE_SIMPLE_TYPE(FSILBusDevice, FSI_LBUS_DEVICE)

typedef struct FSILBusDevice {
    DeviceState parent;

    MemoryRegion iomem;
} FSILBusDevice;

#define TYPE_FSI_LBUS "fsi.lbus"
OBJECT_DECLARE_SIMPLE_TYPE(FSILBus, FSI_LBUS)

typedef struct FSILBus {
    BusState bus;

    MemoryRegion mr;
} FSILBus;

#endif /* FSI_LBUS_H */
