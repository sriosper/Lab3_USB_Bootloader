#ifndef BL_FLASH_H
#define BL_FLASH_H

#include <stdint.h>

#define BL_APP_START     0x08010000UL                  /* first byte of the app   */
#define BL_APP_END       0x08100000UL                  /* one past the last byte  */
#define BL_APP_MAX_SIZE  (BL_APP_END - BL_APP_START)   /* 960 KB = 983,040 bytes  */

// from ST source
typedef enum {
    BL_FLASH_OK = 0,
    BL_FLASH_ERR_ERASE,      /* HAL erase failed, or a page did not read back as 0xFF */
    BL_FLASH_ERR_PROGRAM,    /* HAL program failed                                    */
    BL_FLASH_ERR_VERIFY,     /* a double word read back different from what we wrote  */
    BL_FLASH_ERR_OVERFLOW,   /* image larger than 960 KB, or address outside the app  */
    BL_FLASH_ERR_STATE,      /* called out of order (write before erase, empty image) */
    BL_FLASH_ERR_BANKSWAP    /* banks are swapped; refusing to erase                  */
} bl_flash_status_t;

bl_flash_status_t bl_flash_erase_app(void);
bl_flash_status_t bl_flash_write(const uint8_t *data, uint32_t len);
bl_flash_status_t bl_flash_finish(void);
void              bl_flash_abort(void);

/* Number of image bytes accepted since the last erase (for Member 2's DONE check). */
uint32_t          bl_flash_bytes_written(void);

/* On-board test (bl_flash_test.c). ERASES THE APP - re-flash it afterwards. */
bl_flash_status_t bl_flash_selftest(void);

#endif /* BL_FLASH_H */
