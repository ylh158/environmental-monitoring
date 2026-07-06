/*
 * RC522 RFID — GPIO bit-bang SPI Mode 0
 * PF7(MOSI) PE3(MISO) PE4(SCK) PE2(CS) PF6(RST) PE5(IRQ)
 * DRIVER VERSION: FIX_V3.0 【IRQ标志位修正：Rx=0x02 Idle=0x04】
 * MSH commands:
 *   rc522_selftest  — diagnostic pin/SPI check
 *   rc522_scan      — scan for a card and print UID (with debug)
 *   rc522_write_cmd — write RC522 register hex
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>

// 版本标识，所有日志都会打印，用来区分新版代码
#define RC522_DRV_VER "FIX_V12_IRQ"

/* 调试输出开关 — 设为 0 关闭详细调试，只保留关键信息 */
#define RC522_DBG_ENABLE   0
#if RC522_DBG_ENABLE
#define RC522_DBG(...)     rt_kprintf(__VA_ARGS__)
#else
#define RC522_DBG(...)
#endif

/* ===== Pin mapping ===== */
#define RC522_MOSI     GET_PIN(F, 7)
#define RC522_MISO     GET_PIN(E, 3)
#define RC522_SCLK     GET_PIN(E, 4)
#define RC522_CS       GET_PIN(E, 2)
#define RC522_RST      GET_PIN(F, 6)
#define RC522_IRQ      GET_PIN(E, 5)

/* ===== RC522 Register Map ===== */
#define RC522_REG_COMMAND        0x01
#define RC522_REG_COM_I_EN       0x02
#define RC522_REG_DIV_I_EN       0x03
#define RC522_REG_COM_IRQ        0x04
#define RC522_REG_DIV_IRQ        0x05
#define RC522_REG_ERROR          0x06
#define RC522_REG_FIFO_DATA      0x09
#define RC522_REG_FIFO_LEVEL     0x0A
#define RC522_REG_STATUS1        0x07  // Status1Reg: CRCReady, CRCErr, MFCrypto, HiAlert, etc.
#define RC522_REG_BIT_FRAMING    0x0D
#define RC522_REG_MODE           0x11
#define RC522_REG_TX_MODE        0x12
#define RC522_REG_RX_MODE        0x13
#define RC522_REG_TX_CTRL        0x14
#define RC522_REG_TX_AUTO        0x15
#define RC522_REG_RX_THRESHOLD   0x18
#define RC522_REG_MOD_WIDTH      0x24
#define RC522_REG_RFCFG          0x25
#define RC522_REG_RXGAIN         0x26
#define RC522_REG_T_MODE         0x2A
#define RC522_REG_T_PRESCALER    0x2B
#define RC522_REG_T_RELOAD_H     0x2C
#define RC522_REG_T_RELOAD_L     0x2D
#define RC522_REG_VERSION        0x37
#define RC522_REG_SIGNAL_STRENGTH 0x34  // RSSI - Received Signal Strength Indicator

/* ===== Commands ===== */
#define RC522_CMD_IDLE           0x00
#define RC522_CMD_TRANSCEIVE     0x0C
#define RC522_CMD_SOFTRESET      0x0F

/* ===== IRQ flags (ComIrqReg 0x04) — MFRC522 datasheet bit assignments =====
 * Bit 0: TimerIRq  0x01
 * Bit 1: ErrIRq    0x02
 * Bit 2: LoAlert   0x04
 * Bit 3: HiAlert   0x08
 * Bit 4: IdleIRq   0x10
 * Bit 5: RxIRq     0x20  ← receiver finished
 * Bit 6: TxIRq     0x40  ← transmitter finished
 * Bit 7: Set1      0x80
 */
#define RC522_IRQ_RX             0x20  // Bit5: RxIRq — receiver finished
#define RC522_IRQ_IDLE           0x10  // Bit4: IdleIRq — command completed

/* ===== Card commands ===== */
#define PICC_CMD_REQA            0x26
#define PICC_CMD_WUPA            0x52
#define PICC_CMD_ANTICOLL1       0x93

/* ============================================================
   SPI Low-level — soft SPI 优化延时
   ============================================================ */
static void spi_delay(void)
{
    // 优化延时，适配射频通信时序
    for (volatile int i = 0; i < 200; i++) __NOP();
}

static inline void cs_low(void) {
    rt_pin_write(RC522_CS, PIN_LOW);
}

static inline void cs_high(void) {
    rt_pin_write(RC522_CS, PIN_HIGH);
}

/* ===== Bit-bang SPI Mode 0 (CPOL=0, CPHA=0) ===== */
static uint8_t rc522_spi_byte(uint8_t tx)
{
    uint8_t rx = 0;
    
    for (int i = 0; i < 8; i++)
    {
        /* Step 1: Set MOSI data while SCK is LOW */
        if (tx & 0x80)
            rt_pin_write(RC522_MOSI, PIN_HIGH);
        else
            rt_pin_write(RC522_MOSI, PIN_LOW);
        tx <<= 1;
        
        /* Step 2: Wait for MOSI to stabilize (tSU(DI) >= 10ns) */
        spi_delay();
        spi_delay();  /* double delay for reliable MOSI setup */
        
        /* Step 3: Raise SCK (rising edge - RC522 samples MOSI here) */
        rt_pin_write(RC522_SCLK, PIN_HIGH);
        
        /* Step 4: Sample MISO on SCK high level (tA(DO) <= 35ns) */
        spi_delay();
        rx <<= 1;
        if (rt_pin_read(RC522_MISO))
            rx |= 1;
        
        /* Step 5: Lower SCK (falling edge - data can change now) */
        rt_pin_write(RC522_SCLK, PIN_LOW);
        
        /* Step 6: Delay before next bit */
        spi_delay();
    }
    return rx;
}

/* ===== Register access 移除force_gpio，纯RT-Thread pin接口 ===== */
uint8_t rc522_read(uint8_t addr)
{
    uint8_t rx;
    cs_low();
    rc522_spi_byte(((addr << 1) & 0x7E) | 0x80);
    rx = rc522_spi_byte(0x00);
    cs_high();
    return rx;
}

void rc522_write(uint8_t addr, uint8_t val)
{
    cs_low();
    rc522_spi_byte((addr << 1) & 0x7E);
    rc522_spi_byte(val);
    cs_high();
}

/* ============================================================
   Core: Transceive via FIFO — 修复BitFraming，新版带版本打印
   ============================================================ */
static int rc522_transceive(uint8_t *tx, uint8_t tx_len,
                            uint8_t *rx, uint8_t *rx_len,
                            uint8_t bits_last_byte)
{
    uint8_t irq;
    int timeout;
    uint8_t err;
    uint8_t fifo_level;

    RC522_DBG("[rc522 %s dbg] tx_len=%d, bits=%d, tx:", RC522_DRV_VER, tx_len, bits_last_byte);
    for (int _i = 0; _i < tx_len; _i++)
        RC522_DBG(" %02X", tx_buf[_i]);
    RC522_DBG("\n");

    /* Step 1: Stop any current command */
    rc522_write(RC522_REG_COMMAND, RC522_CMD_IDLE);
    
    /* Step 2: Clear all interrupts */
    rc522_write(RC522_REG_COM_IRQ, 0x7F);
    rc522_write(RC522_REG_DIV_IRQ, 0x7F);
    
    /* Step 3: Clear FIFO buffer */
    rc522_write(RC522_REG_FIFO_LEVEL, 0x80);  // FlushFifo=1
    
    /* Step 4: Set interrupt enable */
    rc522_write(RC522_REG_COM_I_EN, RC522_IRQ_RX | RC522_IRQ_IDLE);
    
    /* Step 5: Set bit framing for last byte — MUST be set BEFORE writing to FIFO! */
    uint8_t bit_framing_val = bits_last_byte & 0x07;
    rc522_write(RC522_REG_BIT_FRAMING, bit_framing_val);

    /* Step 6: Write data to FIFO */
    for (int i = 0; i < tx_len; i++) {
        rc522_write(RC522_REG_FIFO_DATA, tx[i]);
    }

    /* Step 7: Configure CRC for this transaction */
    if (bits_last_byte != 0x00) {
        /* Short frame (e.g. 7-bit REQA) → NO CRC — CRC can't be calculated on partial bytes */
        rc522_write(RC522_REG_MODE, 0x31);  // Clear TxCRCEn(bit2) and RxCRCEn(bit3)
    } else {
        /* Full byte frame (e.g. anticollision) → enable CRC */
        rc522_write(RC522_REG_MODE, 0x3D);  // Set TxCRCEn(bit2) and RxCRCEn(bit3)
    }

    /* Step 8: Start TRANSCEIVE command */
    rc522_write(RC522_REG_COMMAND, RC522_CMD_TRANSCEIVE);

    /* Step 8b: ★★★★★ CRITICAL - Set StartSend=1 to begin data transmission ★★★★★
       Without this bit, RC522 will NOT send any data from the FIFO!
       This is the most common reason for RC522 not detecting cards. */
    rc522_write(RC522_REG_BIT_FRAMING, bit_framing_val | 0x80);  // StartSend=1

    /* Debug: Check command register and antenna status immediately */
#if RC522_DBG_ENABLE
    {
        uint8_t cmd = rc522_read(RC522_REG_COMMAND);
        uint8_t fl = rc522_read(RC522_REG_FIFO_LEVEL) & 0x7F;
        uint8_t tx_ctrl = rc522_read(RC522_REG_TX_CTRL);
        uint8_t rfcfg = rc522_read(RC522_REG_RFCFG);
        uint8_t mode = rc522_read(RC522_REG_MODE);
        RC522_DBG("[rc522 %s dbg] CmdReg=0x%02X FIFO=%d TxCtrl=0x%02X RFCfg=0x%02X ModeReg=0x%02X [CRC=%s]\n",
                   RC522_DRV_VER, cmd, fl, tx_ctrl, rfcfg, mode,
                   (mode & 0x04) ? "ON" : "OFF");
    }
#endif

    /* Step 9: Wait for command completion with extended timeout */
    timeout = 1000;  // Extended to 1 second for better reliability
    while (timeout--) {
        irq = rc522_read(RC522_REG_COM_IRQ);
        
        /* Check if command completed (Idle or Rx interrupt) */
        if (irq & (RC522_IRQ_IDLE | RC522_IRQ_RX)) {
            RC522_DBG("[rc522 %s dbg] IRQ=0x%02X at %d [TxIRq=%d RxIRq=%d IdleIRq=%d LoAlert=%d]\n",
                       RC522_DRV_VER, irq, timeout,
                       (irq & 0x40) ? 1 : 0,   // Bit6 = TxIRq
                       (irq & 0x20) ? 1 : 0,   // Bit5 = RxIRq
                       (irq & 0x10) ? 1 : 0,   // Bit4 = IdleIRq
                       (irq & 0x04) ? 1 : 0);   // Bit2 = LoAlert
            break;
        }
        
        /* Check for errors */
        err = rc522_read(RC522_REG_ERROR);
        if (err & 0x1B) {
            RC522_DBG("[rc522 %s dbg] Error detected: 0x%02X\n", RC522_DRV_VER, err);
            break;
        }
        
        rt_thread_mdelay(1);
    }
    
    /* Clear StartSend bit after command completion */
    rc522_write(RC522_REG_BIT_FRAMING, bit_framing_val & 0x7F);  // StartSend=0

    if (timeout <= 0) {
        RC522_DBG("[rc522 %s dbg] TIMEOUT after 1000ms!\n", RC522_DRV_VER);
        /* Force stop the command */
        rc522_write(RC522_REG_COMMAND, RC522_CMD_IDLE);
        return -1;
    }

    /* Step 9: Check error register */
    err = rc522_read(RC522_REG_ERROR);
    RC522_DBG("[rc522 %s dbg] ErrorReg=0x%02X\n", RC522_DRV_VER, err);
    if (err & 0x1B) {
        RC522_DBG("[rc522 %s dbg] Protocol/Parity/CRC error!\n", RC522_DRV_VER);
        return -1;
    }

    /* Step 10: Read received data from FIFO — only if RxIRq was set */
    if (rx && rx_len) {
        if (irq & RC522_IRQ_RX) {
            fifo_level = rc522_read(RC522_REG_FIFO_LEVEL) & 0x7F;
            RC522_DBG("[rc522 %s dbg] FIFO level=%d\n", RC522_DRV_VER, fifo_level);
            
            if (fifo_level > 0) {
                if (fifo_level > *rx_len) fifo_level = *rx_len;
                for (int i = 0; i < fifo_level; i++) {
                    rx[i] = rc522_read(RC522_REG_FIFO_DATA);
                }
                *rx_len = fifo_level;
                RC522_DBG("[rc522 %s dbg] rx:", RC522_DRV_VER);
                for (int i = 0; i < fifo_level; i++) {
                    RC522_DBG(" %02X", rx[i]);
                }
                RC522_DBG("\n");
            } else {
                *rx_len = 0;
                RC522_DBG("[rc522 %s dbg] No data in FIFO\n", RC522_DRV_VER);
            }
        } else {
            *rx_len = 0;
            RC522_DBG("[rc522 %s dbg] No RxIRq — no card response\n", RC522_DRV_VER);
        }
    }
    
    return 0;
}

/* ============================================================
   RC522 Initialization 增强射频发射配置
   ============================================================ */
void rc522_full_init(void)
{
    uint8_t ver;
    int retry = 5;
    rt_kprintf("[rc522 %s] Soft reset chip\n", RC522_DRV_VER);
    rc522_write(RC522_REG_COMMAND, RC522_CMD_SOFTRESET);
    rt_thread_mdelay(100); // 复位等待延长
    while (retry--) {
        ver = rc522_read(RC522_REG_VERSION);
        if (ver != 0xFF && ver != 0x00) break;
        rt_thread_mdelay(50);
    }
    
    /* Clear all interrupts */
    rc522_write(RC522_REG_COM_IRQ, 0x7F);
    rc522_write(RC522_REG_DIV_IRQ, 0x7F);
    
    /* Clear FIFO */
    rc522_write(RC522_REG_FIFO_LEVEL, 0x80);
    
    /* Timer configuration for 106kbps MIFARE — aligned with STM32 working reference */
    rc522_write(RC522_REG_T_MODE, 0x8D);      // TAuto=1, TPrescaler_H=1, auto restart
    rc522_write(RC522_REG_T_PRESCALER, 0x3E); // TPrescaler=0x3E (62)
    rc522_write(RC522_REG_T_RELOAD_H, 0x00);  // TReload_H=0x00
    rc522_write(RC522_REG_T_RELOAD_L, 30);    // TReload_L=30 (~25ms timeout)
    
    /* TX and RX configuration */
    rc522_write(RC522_REG_TX_MODE, 0x00);     // TX: ISO14443A, 106kbps, no CRC
    rc522_write(RC522_REG_RX_MODE, 0x00);     // RX: ISO14443A, 106kbps, no CRC
    rc522_write(RC522_REG_TX_AUTO, 0x40);     // Force100ASK=1 for MIFARE
    rc522_write(RC522_REG_MODE, 0x3D);        // MSBFirst=0, 106kbps, CRCPreset=01 (0x6363)
    
    /* RF configuration - BEFORE enabling antenna */
    rc522_write(RC522_REG_RX_THRESHOLD, 0x84);// MinLevel=4, CollLevel=8 (higher threshold)
    rc522_write(RC522_REG_MOD_WIDTH, 0x26);   // ModWidth=0x26 (MIFARE 106kbps ~3.5μs)
    rc522_write(RC522_REG_RFCFG,  0x7F);      // RxGain=48dB (max)
    rc522_write(RC522_REG_RXGAIN, 0x7F);      // Max RX gain (GsNReg)
    
    /* Demod and MIFARE registers — explicit defaults */
    rc522_write(0x19, 0x00);  // DemodReg
    rc522_write(0x1C, 0x00);  // MfRxReg
    rc522_write(0x1D, 0x00);  // MfTxReg
    
    /* Turn on antenna (TxControlReg bits 0/1 = Tx1RFEn/Tx2RFEn) */
    {
        uint8_t tx_ctrl = rc522_read(RC522_REG_TX_CTRL);
        if (!(tx_ctrl & 0x03)) {
            rc522_write(RC522_REG_TX_CTRL, tx_ctrl | 0x03);
        }
    }
    rt_thread_mdelay(10);  // Wait for antenna to stabilize
    
    /* Verify antenna is enabled */
    uint8_t tx_ctrl = rc522_read(RC522_REG_TX_CTRL);
    rt_kprintf("[rc522 %s] TxControlReg=0x%02X (bit0/1 should be set for antenna)\n", RC522_DRV_VER, tx_ctrl);
    
    /* 验证射频场状态 - Status1Reg (0x07) */
    uint8_t status1 = rc522_read(RC522_REG_STATUS1);
    rt_kprintf("[rc522 %s] Status1Reg=0x%02X (Bit7=CRCReady, Bit6=CRCErr, Bit3=HiAlert)\n", 
               RC522_DRV_VER, status1);
    
    rt_thread_mdelay(10);
    
    rt_kprintf("[rc522 %s] init done, Chip Version=0x%02X\n", RC522_DRV_VER, ver);
}

/* ============================================================
   Card scanning: REQA/WUPA 传入0x07 7bit
   ============================================================ */
int rc522_read_card(uint8_t *uid, uint8_t *uid_len)
{
    uint8_t tx[8], rx[16], rx_len;
    int rc;
    int retry = 3;
    
    while (retry--) {
        /* Step 1: Send REQA (Request Type A) - 7 bits */
        tx[0] = PICC_CMD_REQA;
        rx_len = sizeof(rx);
        rc = rc522_transceive(tx, 1, rx, &rx_len, 0x07);
        
        rt_kprintf("[rfid] REQA: rc=%d, rx_len=%d\n", rc, rx_len);
        
        if (rc != 0 || rx_len < 2) {
            /* REQA failed, try WUPA (Wake-Up Type A) */
            rt_thread_mdelay(10);
            tx[0] = PICC_CMD_WUPA;
            rx_len = sizeof(rx);
            rc = rc522_transceive(tx, 1, rx, &rx_len, 0x07);
            
            rt_kprintf("[rfid] WUPA: rc=%d, rx_len=%d\n", rc, rx_len);
            
            if (rc != 0 || rx_len < 2) {
                rt_thread_mdelay(50);
                continue;
            }
        }
        
        rt_kprintf("[rfid] ATQA: %02X %02X\n", rx[0], rx[1]);
        
        /* Step 2: Anti-collision - get UID */
        tx[0] = PICC_CMD_ANTICOLL1;
        tx[1] = 0x20;  // NVB (Number of Valid Bits): 20h = 32 bits
        rx_len = sizeof(rx);
        rc = rc522_transceive(tx, 2, rx, &rx_len, 0x00);
        
        rt_kprintf("[rfid] ANTI: rc=%d, rx_len=%d\n", rc, rx_len);
        
        if (rc != 0 || rx_len < 5) {
            rt_thread_mdelay(50);
            continue;
        }
        
        /* Step 3: Verify BCC (Block Check Character) */
        uint8_t bcc_calc = rx[0] ^ rx[1] ^ rx[2] ^ rx[3];
        if (bcc_calc != rx[4]) {
            rt_kprintf("[rfid] BCC mismatch: calc=0x%02X, recv=0x%02X\n", bcc_calc, rx[4]);
            continue;
        }
        
        /* Step 4: Copy UID */
        uid[0] = rx[0];
        uid[1] = rx[1];
        uid[2] = rx[2];
        uid[3] = rx[3];
        *uid_len = 4;
        
        /* Step 5: Validate UID */
        int all_zero = 1, all_ff = 1;
        for (int i = 0; i < 4; i++) {
            if (uid[i] != 0x00) all_zero = 0;
            if (uid[i] != 0xFF) all_ff = 0;
        }
        if (all_zero || all_ff) {
            rt_kprintf("[rfid] Invalid UID (all 0x00 or 0xFF)\n");
            continue;
        }
        
        rt_kprintf("[rfid] ✅ Card found! UID: %02X %02X %02X %02X\n",
                   uid[0], uid[1], uid[2], uid[3]);
        return 0;
    }
    
    rt_kprintf("[rfid] ❌ No card detected after %d retries\n", 3);
    return -1;
}

void rc522_reinit_pins(void)
{
    /* 使用 RT-Thread 标准 pin 接口初始化，确保与 rt_pin_write/read 一致 */
    rt_pin_mode(RC522_RST,  PIN_MODE_OUTPUT);
    rt_pin_mode(RC522_MOSI, PIN_MODE_OUTPUT);
    rt_pin_mode(RC522_SCLK, PIN_MODE_OUTPUT);
    rt_pin_mode(RC522_CS,   PIN_MODE_OUTPUT);
    rt_pin_mode(RC522_MISO, PIN_MODE_INPUT);
    rt_pin_mode(RC522_IRQ,  PIN_MODE_INPUT_PULLUP);

    /* Set initial states */
    rt_pin_write(RC522_RST,  PIN_HIGH);
    rt_pin_write(RC522_MOSI, PIN_LOW);
    rt_pin_write(RC522_SCLK, PIN_LOW);
    rt_pin_write(RC522_CS,   PIN_HIGH);  /* deselect */

    /* Verify pin states */
    rt_kprintf("[rc522 %s] Pin init: SCK=%d MOSI=%d CS=%d RST=%d MISO=%d\n",
               RC522_DRV_VER,
               rt_pin_read(RC522_SCLK),
               rt_pin_read(RC522_MOSI),
               rt_pin_read(RC522_CS),
               rt_pin_read(RC522_RST),
               rt_pin_read(RC522_MISO));
}

#ifdef FINSH_USING_MSH
#include <finsh.h>

/* SPI communication test */
static int rc522_spi_test(int argc, char **argv)
{
    rt_kprintf("\n=== RC522 SPI Communication Test ===\n");
    
    /* Test 1: Read Version Register multiple times */
    rt_kprintf("Test 1: Reading VersionReg (0x37) 5 times:\n");
    for (int i = 0; i < 5; i++) {
        uint8_t ver = rc522_read(RC522_REG_VERSION);
        rt_kprintf("  [%d] 0x%02X %s\n", i+1, ver, 
                   (ver == 0x92) ? "✅" : (ver == 0xFF) ? "❌ FLOAT" : (ver == 0x00) ? "❌ GND" : "⚠️");
        rt_thread_mdelay(10);
    }
    
    /* Test 2: Write and read back TxControlReg */
    rt_kprintf("\nTest 2: Write/Read TxControlReg (0x14):\n");
    uint8_t test_vals[] = {0xC0, 0x00, 0xFF};
    for (int i = 0; i < 3; i++) {
        rc522_write(RC522_REG_TX_CTRL, test_vals[i]);
        rt_thread_mdelay(5);
        uint8_t rd = rc522_read(RC522_REG_TX_CTRL);
        rt_kprintf("  Write 0x%02X -> Read 0x%02X %s\n", 
                   test_vals[i], rd, (rd == test_vals[i]) ? "✅" : "❌");
    }
    
    /* Test 3: Check all pins */
    rt_kprintf("\nTest 3: Pin states:\n");
    rt_kprintf("  SCK=%d  MOSI=%d  CS=%d  RST=%d  MISO=%d  IRQ=%d\n",
               rt_pin_read(RC522_SCLK),
               rt_pin_read(RC522_MOSI),
               rt_pin_read(RC522_CS),
               rt_pin_read(RC522_RST),
               rt_pin_read(RC522_MISO),
               rt_pin_read(RC522_IRQ));
    
    rt_kprintf("\n=== End SPI Test ===\n\n");
    return 0;
}
MSH_CMD_EXPORT(rc522_spi_test, Test RC522 SPI communication);

static int rc522_selftest(int argc, char **argv)
{
    uint8_t ver;
    rt_kprintf("\n=== RC522 Self-Test | DRIVER: %s ===\n", RC522_DRV_VER);
    rt_kprintf("Pin mapping:\n");
    rt_kprintf("  RST = PF6(86)  CS = PE2(66)  IRQ = PE5(69)\n");
    rt_kprintf("  MOSI= PF7(87)  MISO=PE3(67)  SCK = PE4(68)\n");
    rt_kprintf("Pin states:\n");
    rt_kprintf("  RST=%d CS=%d IRQ=%d SCK=%d MOSI=%d MISO=%d\n",
               rt_pin_read(RC522_RST), rt_pin_read(RC522_CS),
               rt_pin_read(RC522_IRQ), rt_pin_read(RC522_SCLK),
               rt_pin_read(RC522_MOSI), rt_pin_read(RC522_MISO));
    rc522_reinit_pins();
    rt_pin_write(RC522_RST, PIN_LOW);
    rt_thread_mdelay(10);
    rt_pin_write(RC522_RST, PIN_HIGH);
    rt_thread_mdelay(100);
    rt_kprintf("\nChip VersionReg (0x37):\n");
    for (int i = 0; i < 3; i++) {
        ver = rc522_read(RC522_REG_VERSION);
        rt_kprintf("  attempt %d: 0x%02X\n", i + 1, ver);
        rt_thread_mdelay(10);
    }
    rc522_full_init();
    rt_kprintf("\nDiagnosis:\n");
    
    /* Support multiple RC522 variants */
    if (ver == 0x92 || ver == 0x82 || ver == 0x91 || ver == 0x12) {
        rt_kprintf("  ✅ Genuine/Compatible MFRC522 (Version 0x%02X)\n", ver);
        rt_kprintf("  ✅ SPI communication OK\n");
    } else if (ver == 0xFF) {
        rt_kprintf("  ❌ Read 0xFF: Check wiring (MISO floating)\n");
    } else if (ver == 0x00) {
        rt_kprintf("  ❌ Read 0x00: Check MISO connection to GND\n");
    } else {
        rt_kprintf("  ⚠️  Unknown version: 0x%02X (may still work)\n", ver);
        rt_kprintf("  💡 Try 'rc522_scan' to test card detection\n");
    }
    rt_kprintf("=== End Self-Test | DRIVER: %s ===\n\n", RC522_DRV_VER);
    return 0;
}
MSH_CMD_EXPORT(rc522_selftest, RC522 diagnostic self-test);

static int rc522_scan(int argc, char **argv)
{
    uint8_t tx[8], rx[16], rx_len;
    int rc;
    int attempts = (argc > 1) ? atoi(argv[1]) : 5;
    rt_kprintf(">>> rc522_scan RUN | DRIVER: %s <<<\n", RC522_DRV_VER);
    rt_kprintf("[rc522 %s] scanning for cards (%d attempts)...\n", RC522_DRV_VER, attempts);
    for (int i = 0; i < attempts; i++) {
        tx[0] = PICC_CMD_REQA;
        rx_len = sizeof(rx);
        rc = rc522_transceive(tx, 1, rx, &rx_len, 0x07);
        rt_kprintf("REQA attempt %d: rc=%d, rx_len=%d\n", i+1, rc, rx_len);
        if (rc == 0 && rx_len >= 2) {
            rt_kprintf("✅ REQA success! ATQA: %02X %02X\n", rx[0], rx[1]);
            tx[0] = PICC_CMD_ANTICOLL1;
            tx[1] = 0x20;
            rx_len = sizeof(rx);
            rc = rc522_transceive(tx, 2, rx, &rx_len, 0x00);
            rt_kprintf("ANTI attempt %d: rc=%d, rx_len=%d\n", i+1, rc, rx_len);
            if (rc == 0 && rx_len >= 5) {
                rt_kprintf("✅ Card found! UID: %02X %02X %02X %02X\n",
                           rx[0], rx[1], rx[2], rx[3]);
                return 0;
            } else {
                rt_kprintf("❌ Anticollision failed\n");
            }
        } else {
            rt_kprintf("❌ No REQA response\n");
        }
        rt_thread_mdelay(300);
    }
    rt_kprintf("[rc522 %s] ❌ No card detected\n", RC522_DRV_VER);
    return -1;
}
MSH_CMD_EXPORT(rc522_scan, Scan for RFID cards);

static int rc522_write_cmd(int argc, char **argv)
{
    if (argc != 3)
    {
        rt_kprintf("Usage: rc522_write_cmd reg_addr hex_value\n");
        rt_kprintf("Example: rc522_write_cmd 0x14 0xC0\n");
        return -1;
    }
    uint8_t addr = strtol(argv[1], RT_NULL, 16);
    uint8_t val  = strtol(argv[2], RT_NULL, 16);
    rc522_write(addr, val);
    rt_kprintf("[rc522 %s] Write Reg 0x%02X = 0x%02X\n", RC522_DRV_VER, addr, val);
    uint8_t rd = rc522_read(addr);
    rt_kprintf("[rc522 %s] Readback Reg 0x%02X = 0x%02X\n", RC522_DRV_VER, addr, rd);
    return 0;
}
MSH_CMD_EXPORT(rc522_write_cmd, Write RC522 register hex);

/* 【新增】天线场强测试 - 通过测量背景噪声判断射频场是否激活 */
static int rc522_field_test(int argc, char **argv)
{
    rt_kprintf("\n=== RC522 RF Field Test ===\n");
    
    /* Step 1: Check TxControlReg */
    uint8_t tx_ctrl = rc522_read(RC522_REG_TX_CTRL);
    rt_kprintf("TxControlReg = 0x%02X\n", tx_ctrl);
    if ((tx_ctrl & 0xC0) == 0xC0) {
        rt_kprintf("  ✅ Antenna drivers enabled (Tx1=1, Tx2=1)\n");
    } else {
        rt_kprintf("  ❌ Antenna NOT enabled! Need 0xC0\n");
        return -1;
    }
    
    /* 【新增】Step 1.5: Force re-enable antenna with delay */
    rt_kprintf("\nForcing antenna re-enable...\n");
    rc522_write(RC522_REG_TX_CTRL, 0x00);  // Disable first
    rt_thread_mdelay(5);
    rc522_write(RC522_REG_TX_CTRL, 0xC0);  // Re-enable
    rt_thread_mdelay(20);  // Wait for RF field to stabilize
    
    uint8_t tx_ctrl2 = rc522_read(RC522_REG_TX_CTRL);
    rt_kprintf("After re-enable: TxControlReg = 0x%02X %s\n", 
               tx_ctrl2, (tx_ctrl2 == 0xC0) ? "✅" : "❌");
    
    /* Step 2: Measure background noise with antenna ON */
    rt_kprintf("\nMeasuring background noise (10 samples)...\n");
    uint16_t noise_sum = 0;
    for (int i = 0; i < 10; i++) {
        /* Clear FIFO and start idle measurement */
        rc522_write(RC522_REG_FIFO_LEVEL, 0x80);  // FlushFIFO
        rc522_write(RC522_REG_COMMAND, RC522_CMD_IDLE);
        rt_thread_mdelay(5);
        
        /* Read RX signal strength indicator (RSSI at 0x34) */
        uint8_t signal = rc522_read(RC522_REG_SIGNAL_STRENGTH);
        noise_sum += signal;
        rt_kprintf("  [%d] Signal Strength = 0x%02X (%d)\n", i+1, signal, signal);
        rt_thread_mdelay(10);
    }
    
    uint16_t avg_noise = noise_sum / 10;
    rt_kprintf("\nAverage noise level: %d (0x%02X)\n", avg_noise, avg_noise);
    
    if (avg_noise > 20) {
        rt_kprintf("  ✅ RF field is ACTIVE (noise detected)\n");
        rt_kprintf("  💡 Try placing a card near the antenna now!\n");
    } else if (avg_noise > 5) {
        rt_kprintf("  ⚠️  RF field is WEAK (low noise)\n");
        rt_kprintf("  🔧 Possible issues:\n");
        rt_kprintf("     - Antenna coil impedance mismatch\n");
        rt_kprintf("     - Matching capacitors incorrect value\n");
        rt_kprintf("     - Card distance too far (>5cm)\n");
    } else {
        rt_kprintf("  ❌ RF field is INACTIVE (no noise)\n");
        rt_kprintf("  🔧 HARDWARE CHECK REQUIRED:\n");
        rt_kprintf("     1. Measure antenna coil resistance (<10Ω expected)\n");
        rt_kprintf("     2. Check matching capacitors (22-33pF near antenna pins)\n");
        rt_kprintf("     3. Verify VCC voltage (3.3V ±5%%)\n");
        rt_kprintf("     4. Replace RC522 module if possible\n");
    }
    
    rt_kprintf("\n=== End Field Test ===\n\n");
    return 0;
}
MSH_CMD_EXPORT(rc522_field_test, Test RC522 RF field strength);

/* 【新增】手动重新初始化 RC522（更换模块后使用） */
static int rc522_reinit(int argc, char **argv)
{
    rt_kprintf("\n=== RC522 Manual Re-initialization ===\n");
    
    /* Step 1: Hardware reset */
    rt_kprintf("Step 1: Hardware reset...\n");
    rt_pin_write(RC522_RST, PIN_LOW);
    rt_thread_mdelay(10);
    rt_pin_write(RC522_RST, PIN_HIGH);
    rt_thread_mdelay(200);
    
    /* Step 2: Full initialization */
    rt_kprintf("Step 2: Full initialization...\n");
    rc522_full_init();
    
    /* Step 3: Verify */
    rt_kprintf("\nStep 3: Verification...\n");
    uint8_t ver = rc522_read(RC522_REG_VERSION);
    uint8_t tx_ctrl = rc522_read(RC522_REG_TX_CTRL);
    
    /* Support multiple RC522 variants: 0x92 (standard), 0x82 (clone), 0x91, etc. */
    rt_bool_t ver_ok = (ver == 0x92 || ver == 0x82 || ver == 0x91 || ver == 0x12);
    rt_bool_t tx_ok = ((tx_ctrl & 0xC0) == 0xC0);
    
    rt_kprintf("  VersionReg = 0x%02X %s", ver, ver_ok ? "✅" : "❌");
    if (!ver_ok) {
        rt_kprintf(" (Expected: 0x92/0x82/0x91/0x12)");
    }
    rt_kprintf("\n");
    
    rt_kprintf("  TxControlReg = 0x%02X %s\n", tx_ctrl, tx_ok ? "✅" : "❌");
    
    if (ver_ok && tx_ok) {
        rt_kprintf("\n✅ Re-initialization SUCCESS!\n");
        rt_kprintf("💡 Now try 'rc522_scan' to detect cards.\n");
        return 0;
    } else {
        rt_kprintf("\n❌ Re-initialization FAILED!\n");
        rt_kprintf("🔧 Check hardware connections and power supply.\n");
        return -1;
    }
}
MSH_CMD_EXPORT(rc522_reinit, Manually reinitialize RC522 after module replacement);

/* ============================================================
   深度诊断：逐步骤测试 TRANSCEIVE 管线
   ============================================================ */
static int rc522_debug(int argc, char **argv)
{
    uint8_t val, err, irq;
    int timeout;

    rt_kprintf("\n========== RC522 DEEP DIAGNOSTIC ==========\n");
    rt_kprintf("Driver: %s\n", RC522_DRV_VER);

    /* ───── Part 1: Register dump ───── */
    rt_kprintf("\n--- Part 1: Full Register Dump ---\n");

    struct { uint8_t addr; const char *name; } regs[] = {
        {0x01, "CommandReg"},    {0x02, "ComIEnReg"},   {0x03, "DivIEnReg"},
        {0x04, "ComIrqReg"},     {0x05, "DivIrqReg"},   {0x06, "ErrorReg"},
        {0x07, "Status1Reg"},    {0x08, "Status2Reg"},  {0x09, "FIFODataReg"},
        {0x0A, "FIFOLevelReg"},  {0x0B, "WaterLevelReg"},{0x0C, "ControlReg"},
        {0x0D, "BitFramingReg"}, {0x0E, "CollReg"},     {0x0F, "Reserved"},
        {0x11, "ModeReg"},       {0x12, "TxModeReg"},   {0x13, "RxModeReg"},
        {0x14, "TxControlReg"},  {0x15, "TxAutoReg"},   {0x18, "RxThresholdReg"},
        {0x19, "DemodReg"},      {0x1A, "Reserved"},    {0x1B, "MfTxReg"},
        {0x1C, "MfRxReg"},       {0x1D, "Reserved"},    {0x24, "ModWidthReg"},
        {0x25, "RFCfgReg"},      {0x26, "GsNReg"},      {0x2A, "TModeReg"},
        {0x2B, "TPrescalerReg"}, {0x2C, "TReloadRegH"}, {0x2D, "TReloadRegL"},
        {0x34, "RSSIReg"},       {0x37, "VersionReg"},
    };
    for (int i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        val = rc522_read(regs[i].addr);
        rt_kprintf("  0x%02X (%s) = 0x%02X\n", regs[i].addr, regs[i].name, val);
    }

    /* ───── Part 2: FIFO write/readback test ───── */
    rt_kprintf("\n--- Part 2: FIFO Write/Readback Test ---\n");
    rc522_write(RC522_REG_FIFO_LEVEL, 0x80);  // Flush
    rc522_write(RC522_REG_FIFO_DATA, 0xA5);   // Write known pattern
    rc522_write(RC522_REG_FIFO_DATA, 0x5A);   // Write second pattern
    uint8_t fl = rc522_read(RC522_REG_FIFO_LEVEL) & 0x7F;
    rt_kprintf("  FIFO level after writing 2 bytes: %d %s\n",
               fl, (fl == 2) ? "✅" : "❌");

    /* Read back: read from FIFO */
    rc522_write(RC522_REG_FIFO_LEVEL, 0x80);  // Flush
    rc522_write(RC522_REG_FIFO_DATA, 0xA5);
    rc522_write(RC522_REG_FIFO_DATA, 0x5A);
    rc522_write(RC522_REG_FIFO_DATA, 0x00);  // Dummy read
    uint8_t rx1 = rc522_read(RC522_REG_FIFO_DATA);
    uint8_t rx2 = rc522_read(RC522_REG_FIFO_DATA);
    rt_kprintf("  Readback: 0x%02X 0x%02X %s\n",
               rx1, rx2,
               (rx1 == 0xA5 && rx2 == 0x5A) ? "✅" : "❌");

    /* ───── Part 3: Test TRANSCEIVE without card ───── */
    rt_kprintf("\n--- Part 3: TRANSCEIVE Test (no card needed) ---\n");

    /* Write REQA to FIFO */
    rc522_write(RC522_REG_COMMAND, RC522_CMD_IDLE);
    rc522_write(RC522_REG_COM_IRQ, 0x7F);
    rc522_write(RC522_REG_DIV_IRQ, 0x7F);
    rc522_write(RC522_REG_FIFO_LEVEL, 0x80);
    rc522_write(RC522_REG_COM_I_EN, 0x7F);  // Enable ALL interrupts
    rc522_write(RC522_REG_BIT_FRAMING, 0x07);
    rc522_write(RC522_REG_FIFO_DATA, 0x26);

    /* Read state BEFORE Transceive */
    rt_kprintf("  BEFORE TRANSCEIVE:\n");
    rt_kprintf("    FIFOLevel = %d (expected 1)\n", rc522_read(RC522_REG_FIFO_LEVEL) & 0x7F);
    rt_kprintf("    ComIrqReg = 0x%02X (expected 0x00 after clear)\n", rc522_read(RC522_REG_COM_IRQ));
    rt_kprintf("    ErrorReg  = 0x%02X (expected 0x00)\n", rc522_read(RC522_REG_ERROR));

    /* Start Transceive */
    rt_kprintf("\n  Starting TRANSCEIVE (0x0C to CommandReg)...\n");
    rc522_write(RC522_REG_COMMAND, RC522_CMD_TRANSCEIVE);

    /* Read CommandReg to verify */
    val = rc522_read(RC522_REG_COMMAND);
    rt_kprintf("    CommandReg after write = 0x%02X (expected 0x0C) %s\n",
               val, (val == 0x0C) ? "✅" : "❌");

    /* Immediately read ComIrqReg */
    irq = rc522_read(RC522_REG_COM_IRQ);
    rt_kprintf("    ComIrqReg immediate    = 0x%02X %s\n", irq,
               (irq != 0x00) ? "(IRQ fired!)" : "(no IRQ yet)");
    
    /* Wait for idle with fast polling */
    rt_kprintf("\n  Polling for completion (50 iterations, 1ms each):\n");
    for (timeout = 0; timeout < 50; timeout++) {
        irq = rc522_read(RC522_REG_COM_IRQ);
        err = rc522_read(RC522_REG_ERROR);
        val = rc522_read(RC522_REG_COMMAND);
        
        if (irq & 0x40) rt_kprintf("    [%d] TxIRq! ", timeout);   // Bit6 = TxIRq
        if (irq & 0x20) rt_kprintf("    [%d] RxIRq! ", timeout);   // Bit5 = RxIRq
        if (irq & 0x10) rt_kprintf("    [%d] IdleIRq! ", timeout); // Bit4 = IdleIRq
        
        if (irq & (0x20 | 0x10)) {  // RxIRq or IdleIRq
            rt_kprintf("Cmd=0x%02X Err=0x%02X\n", val, err);
            break;
        }
        
        if (irq) {
            rt_kprintf("Cmd=0x%02X Err=0x%02X IRQ=0x%02X\n", val, err, irq);
        }
        
        rt_thread_mdelay(1);
    }
    
    if (timeout >= 50) {
        rt_kprintf("  ❌ TIMEOUT after 50ms! CommandReg=0x%02X\n",
                   rc522_read(RC522_REG_COMMAND));
    }

    /* ───── Part 4: Check FIFO after TRANSCEIVE ───── */
    rt_kprintf("\n--- Part 4: Post-TRANSCEIVE State ---\n");
    fl = rc522_read(RC522_REG_FIFO_LEVEL) & 0x7F;
    uint8_t fifo_data = rc522_read(RC522_REG_FIFO_DATA);
    rt_kprintf("  FIFOLevel = %d (0 = data transmitted)\n", fl);
    rt_kprintf("  FIFOData  = 0x%02X\n", fifo_data);
    rt_kprintf("  Status2Reg = 0x%02X\n", rc522_read(0x08));

    /* ───── Summary ───── */
    rt_kprintf("\n========== DIAGNOSTIC SUMMARY ==========\n");
    
    /* Check if TxIRq occurred (bit 6 of ComIrqReg) is harder since we cleared it */
    /* Instead check by re-reading the register state doesn't help */
    rt_kprintf("Key observations:\n");
    rt_kprintf("  VersionReg = 0x%02X - Chip is %s\n",
               rc522_read(RC522_REG_VERSION),
               (rc522_read(RC522_REG_VERSION) == 0x92) ? "ALIVE ✅" : "SUSPICIOUS ❌");
    
    /* Re-enable everything */
    rc522_full_init();
    rt_kprintf("  Diagnostics complete - RC522 re-initialized\n");
    rt_kprintf("============================================\n\n");
    return 0;
}
MSH_CMD_EXPORT(rc522_debug, RC522 deep diagnostic (register dump + FIFO test + TRANSCEIVE step-by-step));

#endif  /* FINSH_USING_MSH */

static int rc522_hw_init(void)
{
    rt_kprintf("[rc522 %s] hardware init start【新版修复7bit+SPI时序优化】\n", RC522_DRV_VER);
    rc522_reinit_pins();
    rt_pin_write(RC522_RST, PIN_LOW);
    rt_thread_mdelay(10);
    rt_pin_write(RC522_RST, PIN_HIGH);
    rt_thread_mdelay(200);
    rc522_full_init();
    rt_kprintf("[rc522 %s] ready. Use 'rc522_selftest' / 'rc522_scan' / 'rc522_write_cmd'.\n", RC522_DRV_VER);
    return 0;
}
INIT_ENV_EXPORT(rc522_hw_init);