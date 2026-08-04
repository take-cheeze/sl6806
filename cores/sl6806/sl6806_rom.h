/*
 * sl6806_rom.h - Boot ROM ABI for the SL6806 mask ROM.
 *
 * These entry points live in the chip's mask ROM at 0x00000000 and are
 * available whenever the chip is in USB bootloader mode. All addresses are
 * verified: they come from the smartlink_flash payload headers
 * (3rd/smartlink_flash/payload/sl6806_sys.h) and are known-good because the
 * stock ROM-reading payload calls them successfully on real hardware.
 *
 * NOTE ON VALIDITY: these are SL6806 addresses only. The SL6801 ROM has the
 * same functions at completely different addresses. The chip is identified by
 * its USB serial string: SL6806 bootloader reports "20220320000001".
 *
 * NOTE ON MODE: a sketch built for PAYLOAD mode runs underneath this ROM and
 * may call anything here. A sketch built for FIRMWARE mode replaces the
 * application and runs with the ROM no longer resident in the same state -
 * do not call these from firmware builds.
 */
#ifndef SL6806_ROM_H
#define SL6806_ROM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thumb entry points: the +1 sets the Thumb bit required by BLX. */
#define SL6806_ROM_FN(addr, ret, name, args) \
    static ret (* const name) args = (ret (*) args)((addr) + 1)

SL6806_ROM_FN(0x0640, void,   rom_printf,  (const char *fmt, ...));
SL6806_ROM_FN(0x0B82, char *, rom_strcpy,  (char *, const char *));
SL6806_ROM_FN(0x0B96, char *, rom_strncpy, (char *, const char *, size_t));
SL6806_ROM_FN(0x0BBC, char *, rom_strchr,  (const char *, int));
SL6806_ROM_FN(0x0BD8, char *, rom_strrchr, (const char *, int));
SL6806_ROM_FN(0x0BF2, size_t, rom_strlen,  (const char *));
SL6806_ROM_FN(0x0C02, size_t, rom_strnlen, (const char *, size_t));
SL6806_ROM_FN(0x0C16, int,    rom_strcmp,  (const void *, const void *));
SL6806_ROM_FN(0x0C30, int,    rom_strncmp, (const void *, const void *, size_t));
SL6806_ROM_FN(0x0C4E, char *, rom_strcat,  (char *, const char *));
SL6806_ROM_FN(0x0C64, char *, rom_strncat, (char *, const char *, size_t));
SL6806_ROM_FN(0x0C8E, void *, rom_memcpy,  (void *, const void *, size_t));
SL6806_ROM_FN(0x0D0E, void *, rom_memset,  (void *, int, size_t));
SL6806_ROM_FN(0x0D82, void *, rom_memmove, (void *, const void *, size_t));
SL6806_ROM_FN(0x0DB8, int,    rom_memcmp,  (const void *, const void *, size_t));
SL6806_ROM_FN(0x0DD4, void *, rom_malloc,  (size_t));
SL6806_ROM_FN(0x0DD8, void,   rom_free,    (void *));

/*
 * USB user-function table. Registering one hooks the ROM's USB/SCSI service
 * loop, which is how the stock read_mem2 payload works.
 *
 * Only two slots have established meaning:
 *   scsi_cb - called with the 16-byte CDB of a vendor SCSI command. This is
 *             verified: the stock payload handles its own command here.
 *   cb2     - the stock payload installs a do-nothing function here and the
 *             ROM remains healthy, so the ROM calls it. [I] We use it as the
 *             periodic hook that drives loop(). If your sketch never advances,
 *             cb2 is not periodic on your ROM revision - build with
 *             -DSL6806_RUN_MODE=SL6806_RUN_TAKEOVER instead.
 * The remaining slots are unknown; leave them NULL.
 */
typedef struct {
    int (*cb0)(void);
    int (*cb1)(void);
    int (*cb2)(void);           /* [I] periodic / idle hook */
    int (*cb3)(void);
    int (*cb4)(void);
    int (*cb5)(void);
    int (*scsi_cb)(uint8_t *cdb);   /* [V] vendor SCSI handler */
    const uint8_t *scsi_id;
    int (*cb8)(void);
    uint32_t unknown[4];        /* [V] SL6806 table is 4 words longer than SL6801 */
} sl6806_usb_userfn_t;

SL6806_ROM_FN(0x5B70, void, rom_usb_set_userfn, (const sl6806_usb_userfn_t *));

/* Return codes for scsi_cb. */
enum {
    SL6806_USB_ERROR  = -1,
    SL6806_USB_DATA   = 0,
    SL6806_USB_NODATA = 1
};

/*
 * USB transfer descriptor. Field offsets are verified from the trampolines in
 * smtlink_dump.c, which read them on real hardware.
 */
#define SL6806_USB_DESC        0x00800DF8u

#define SL6806_USB_RET_LEN     (*(volatile uint16_t *)(SL6806_USB_DESC + 0x06))
#define SL6806_USB_RET_BUF     (*(uint8_t **)(SL6806_USB_DESC + 0x08))
#define SL6806_USB_DATA_LEN    (*(volatile uint32_t *)(SL6806_USB_DESC + 0x14))
#define SL6806_USB_CDB         ((volatile uint8_t *)(SL6806_USB_DESC + 0x1B))
#define SL6806_USB_RET_BUF_SIZE 64

/* Vendor SCSI opcode the host tool sends: cdb[0] = 0xC0 | flash_index. */
#define SL6806_SCSI_VENDOR_OP  0xC0

#ifdef __cplusplus
}
#endif

#endif /* SL6806_ROM_H */
