/*
 
 * STM32L496ZG flash: 1 MB, dual bank, 2 KB pages, 256 pages per bank.
 *   Bank 1: 0x08000000 - 0x0807FFFF  (pages 0..255)
 *   Bank 2: 0x08080000 - 0x080FFFFF  (pages 0..255)
 * Bootloader = bank 1 pages 0..31 (64 KB). Application = bank 1 pages 32..255
 * plus all of bank 2 = 224 + 256 = 480 pages (960 KB).
 */
#include "bl_flash.h"
#include "stm32l4xx_hal.h"
#include <string.h>

/* Lets the host-side unit test map flash addresses onto a simulated array. */
#ifndef BL_FLASH_READ32
#define BL_FLASH_READ32(addr) (*(volatile const uint32_t *)(addr))
#endif

/* Fixed for the STM32L496ZG (1 MB). Note: the HAL's FLASH_BANK_SIZE is read
   from a register at run time, so it can't be used in a static assert. */
#define DW_SIZE           8U
#define BL_PAGE_SIZE      2048UL
#define PAGES_PER_BANK    256UL
#define APP_FIRST_PAGE    ((BL_APP_START - 0x08000000UL) / BL_PAGE_SIZE)    /* 32 */

_Static_assert((BL_APP_START % BL_PAGE_SIZE) == 0, "app must start on a page boundary");
_Static_assert(APP_FIRST_PAGE == 32, "expected 64 KB bootloader = 32 pages");
_Static_assert((PAGES_PER_BANK - APP_FIRST_PAGE) + PAGES_PER_BANK == 480, "expected 480 app pages");

static uint8_t  s_buf[DW_SIZE];   /* bytes waiting for a full double word     */
static uint32_t s_buf_len;        /* how many bytes are in s_buf              */
static uint32_t s_next_addr;      /* flash address where s_buf will be written */
static uint32_t s_total;          /* image bytes accepted since erase         */
static uint64_t s_first_dw;       /* held back until bl_flash_finish()        */
static uint8_t  s_have_first;
static uint8_t  s_ready;          /* 1 = erased and accepting data            */


//holds 6 variables that sets everything to he start before a new update. 
// the variables remember progress during the update like if the buffer is full, where the next write goes and how many bytes arrive. 
// This fnc just tracks and sets variables. They do not get used outside the c file. 
// This function clears the progress tracking variables so each update starts from where the program index starts

static void reset_state(void)
{
    s_buf_len    = 0;
    s_next_addr  = BL_APP_START;
    s_total      = 0;
    s_first_dw   = 0;
    s_have_first = 0;
    s_ready      = 0;
}



// /this function writes the 8-byte created from flush_buffer(). 
// This is the function that writes to the flash, so it has all the safety checks. 
// It checks if hte address is safe to write if the address is larger then the start, or less then the end- DW size 
static bl_flash_status_t program_dw(uint32_t addr, uint64_t value)
{
    /* Hard guard: never touch the bootloader, never run off the end. */
    if (addr < BL_APP_START || addr > BL_APP_END - DW_SIZE || (addr & (DW_SIZE - 1U)) != 0U) {
        return BL_FLASH_ERR_OVERFLOW;
    }
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, value) != HAL_OK) {
        return BL_FLASH_ERR_PROGRAM;
    }
    if (BL_FLASH_READ32(addr)     != (uint32_t)value ||
        BL_FLASH_READ32(addr + 4) != (uint32_t)(value >> 32)) {
        return BL_FLASH_ERR_VERIFY;
    }
    return BL_FLASH_OK;
}

// is called when buffer is full. 
// It turns the byte into one number, writes to flash then moves to the next address and empties the buffer the ST does this format

static bl_flash_status_t flush_buffer(void)
{
    uint64_t value;
    memcpy(&value, s_buf, DW_SIZE);           /* little-endian: s_buf[0] -> lowest address */

    bl_flash_status_t st = BL_FLASH_OK;
    if (s_next_addr == BL_APP_START) {
        s_first_dw   = value;                 /* written last, in bl_flash_finish() */
        s_have_first = 1;
    } else {
        st = program_dw(s_next_addr, value);
    }
    s_next_addr += DW_SIZE;
    s_buf_len    = 0;
    return st;
}

//erases groups of pages into one bank by filling in STs settings and calling STs built in functions

static bl_flash_status_t erase_pages(uint32_t bank, uint32_t first_page, uint32_t count)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0;

    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks     = bank;
    erase.Page      = first_page;
    erase.NbPages   = count;

    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
        return BL_FLASH_ERR_ERASE;
    }
    return BL_FLASH_OK;
}


//prepares the flash for new app by unlocking it, wiping out all the pages and checking if the pages are wiped. 
// It uses the 2 helper functions to reset the flash and erase each bank. 
// Additionally, it checks if the pages are swapped, checks if banks are cleared and passes next function to be called by member to to flash new data.
bl_flash_status_t bl_flash_erase_app(void)
{
    reset_state();

#ifdef SYSCFG_MEMRMP_FB_MODE
    /* If the banks are swapped, "bank 1" is not at 0x08000000 and the page
       numbers below would hit the running bootloader. Refuse. */
    if (READ_BIT(SYSCFG->MEMRMP, SYSCFG_MEMRMP_FB_MODE) != 0U) {
        return BL_FLASH_ERR_BANKSWAP;
    }
#endif

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);   /* stale flags make the first op fail */

    /* Call 1: bank 1, pages 32..255 (224 pages, 0x08010000 - 0x0807FFFF) */
    bl_flash_status_t st = erase_pages(FLASH_BANK_1, APP_FIRST_PAGE, PAGES_PER_BANK - APP_FIRST_PAGE);
    /* Call 2: bank 2, pages 0..255 (256 pages, 0x08080000 - 0x080FFFFF) */
    if (st == BL_FLASH_OK) {
        st = erase_pages(FLASH_BANK_2, 0, PAGES_PER_BANK);
    }

    /* Blank check: every word of the application region must read 0xFFFFFFFF. */
    for (uint32_t a = BL_APP_START; st == BL_FLASH_OK && a < BL_APP_END; a += 4U) {
        if (BL_FLASH_READ32(a) != 0xFFFFFFFFUL) {
            st = BL_FLASH_ERR_ERASE;
        }
    }

    if (st != BL_FLASH_OK) {
        HAL_FLASH_Lock();
        return st;
    }

    s_ready = 1;          /* stay unlocked until finish/abort */
    return BL_FLASH_OK;
}


//takes in bytes from data input and separates them into the 8 byte buffer. 
// When the buffer fills, it writes it to flash.
//  You loop 8 bytes at a time from the data. You need to check if 8 bytes are there to take first. 
// THrough main loop, which runs once per loop countind down to 0. 
// Each loop, you deregferance each byte, put it in the next empty buffer slot and move onto the next slot. 
// You increase each byte until you have 0 spots left and you move over to next 8 and start writing it to flash

bl_flash_status_t bl_flash_write(const uint8_t *data, uint32_t len)
{
    if (!s_ready) {
        return BL_FLASH_ERR_STATE;
    }
    if (len > BL_APP_MAX_SIZE - s_total) {
        return BL_FLASH_ERR_OVERFLOW;
    }
    while (len--) {
        s_buf[s_buf_len++] = *data++;
        s_total++;
        if (s_buf_len == DW_SIZE) {
            bl_flash_status_t st = flush_buffer();
            if (st != BL_FLASH_OK) {
                return st;
            }
        }
    }
    return BL_FLASH_OK;
}



// the last function in a successful update and upload. 
// It writes in any left over bits if the last 8 byte number isn't filled with 0s. 
// It reports how many bytes were filled in as 0s and then it locks the flash if complte without any errors 

bl_flash_status_t bl_flash_finish(void)
{
    bl_flash_status_t st = BL_FLASH_OK;

    if (!s_ready || s_total == 0U) {
        st = BL_FLASH_ERR_STATE;
    }

    // Leftover 1-7 bytesget  pad with 0xFF so the last double word is complete 8 bytes
    if (st == BL_FLASH_OK && s_buf_len > 0U) {
        memset(&s_buf[s_buf_len], 0xFF, DW_SIZE - s_buf_len);
        s_buf_len = DW_SIZE;
        st = flush_buffer();
    }

    /* Last step: the vector table. Only now does the app become launchable. */
    if (st == BL_FLASH_OK && s_have_first) {
        st = program_dw(BL_APP_START, s_first_dw);
    }

    HAL_FLASH_Lock();
    s_ready = 0;
    return st;
}


//is an emergency exit if anything during transfer goes wrong

void bl_flash_abort(void)
{
    /* The first double word was never written, so the app stays unlaunchable. */
    HAL_FLASH_Lock();
    s_ready = 0;
}



//returns how many bytes of the app have arrived so far. 

uint32_t bl_flash_bytes_written(void)
{
    return s_total;
}
