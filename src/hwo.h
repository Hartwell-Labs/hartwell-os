/* Shared interface between the runtime (hwo.c) and the framebuffer UI (fbui.c). */
#ifndef HWO_H
#define HWO_H

/* "HWO-XXXX-YYYY" for this machine, derived from its DMI UUID. */
void hwo_device_id(char out[32]);

/* 1 if /etc/hartwell/license holds a key that matches this device. */
int hwo_licensed(char *key_out, unsigned long key_n);

/* Key format/checksum check for this device, without touching the licence file. */
int hwo_key_valid(const char *key);

/* Persist a validated key. Returns 0 on success. */
int hwo_save_key(const char *key);
int hwo_state_persistent(void);   /* licence is on the HWO_CFG partition */
int hwo_state_present(void);      /* a state partition is mounted at /state */

/* Read a whole file into buf (trimmed); returns -1 if unreadable. */
int hwo_read(const char *path, char *buf, unsigned long n);

/* Framebuffer UI. Returns only if there is no usable framebuffer. */
int fbui_run(void);

#endif
