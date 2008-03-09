/*
   atarisio V1.02
   a kernel module for handling the Atari 8bit SIO protocol

   Copyright (C) 2002-2007 Matthias Reichl <hias@horus.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
#include <linux/config.h>
#else
#include <linux/autoconf.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)

#if defined(CONFIG_MODVERSIONS) && !defined(MODVERSIONS)
#define MODVERSIONS
#endif

#ifdef MODVERSIONS
#include <linux/modversions.h>
#endif

#endif

#include <linux/fs.h>
#include <linux/serial.h>

#include <asm/atomic.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/ioctls.h>

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/smp_lock.h>

#include <linux/poll.h>
#include <linux/serial_reg.h>
#include <linux/miscdevice.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/slab.h>

#include "atarisio.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,23)
#define irqreturn_t void
#define IRQ_RETVAL(foo)
#endif

/* debug levels: */
#define DEBUG_STANDARD 1
#define DEBUG_NOISY 2
#define DEBUG_VERY_NOISY 3

/*
#define ATARISIO_DEBUG_TIMING
*/

/*
 * if ATARISIO_EARLY_NOTIFICATION is defined, poll will indicate a
 * pending command frame at the very start of the command frame,
 * not when it's completely received
 */
/*
#define ATARISIO_EARLY_NOTIFICATION
*/

#define PRINTK_NODEV(x...) printk(NAME ": " x)

#define PRINTK(x...) do { \
		printk(NAME "%d: ", dev->id); \
		printk(x); \
	} while(0)

#define IRQ_PRINTK(level, x...) \
	if (debug_irq >= level) { \
		PRINTK(x); \
	}

#define DBG_PRINTK(level, x...) \
	if (debug >= level) { \
		PRINTK(x); \
	}

#ifdef ATARISIO_DEBUG_TIMING
#define PRINT_TIMESTAMP(msg) \
	do { \
		struct timeval tv; \
		do_gettimeofday(&tv); \
		printk(NAME "%d: %s (%lu:%lu)\n", dev->id msg, tv.tv_sec, tv.tv_usec);\
	} while (0)
#else
#define PRINT_TIMESTAMP(msg) do { } while(0)
#endif


/*
#define ATARISIO_DEBUG_TIMEOUTS
#define ATARISIO_DEBUG_TIMEOUT_WAIT (HZ/2)
*/

#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif
#ifdef MODULE_AUTHOR
MODULE_AUTHOR("Matthias Reichl <hias@horus.com>");
#endif
#ifdef MODULE_DESCRIPTION
MODULE_DESCRIPTION("Serial Atari 8bit SIO driver");
#endif

/* 
 * currently we use major 10, minor 240, which is a reserved local
 * number within the miscdevice block
 */

#define ATARISIO_DEFAULT_MINOR 240

/* maximum number of devices */
#define ATARISIO_MAXDEV 4

#define NAME "atarisio"

/*
 * module parameters:
 * io:  the base address of the 16550
 * irq: interrupt number
 */
static int minor = ATARISIO_DEFAULT_MINOR;
static char* port[ATARISIO_MAXDEV] = {0, };
static int io[ATARISIO_MAXDEV] = {0, };
static int irq[ATARISIO_MAXDEV] = {0, };
static int debug = 0;
static int debug_irq = 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,10)
MODULE_PARM(minor,"i");
MODULE_PARM(port,"1-" __MODULE_STRING(ATARISIO_MAXDEV) "s");
MODULE_PARM(io,"1-" __MODULE_STRING(ATARISIO_MAXDEV) "i");
MODULE_PARM(irq,"1-" __MODULE_STRING(ATARISIO_MAXDEV) "i");
MODULE_PARM(debug,"i");
MODULE_PARM(debug_irq,"i");
#else
module_param(minor, int, S_IRUGO);
module_param_array(port, charp, 0, S_IRUGO);
module_param_array(io, int, 0, S_IRUGO);
module_param_array(irq, int, 0, S_IRUGO);
module_param(debug, int, S_IRUGO | S_IWUSR);
module_param(debug_irq, int, S_IRUGO | S_IWUSR);
#endif

#ifdef MODULE_PARM_DESC
MODULE_PARM_DESC(port,"serial port (eg /dev/ttyS0, default: use supplied io/irq values)");
MODULE_PARM_DESC(io,"io address of 16550 UART (eg 0x3f8)");
MODULE_PARM_DESC(irq,"irq of 16550 UART (eg 4)");
MODULE_PARM_DESC(minor,"minor device number (default: 240)");
MODULE_PARM_DESC(debug,"debug level (default: 0)");
MODULE_PARM_DESC(debug_irq,"interrupt debug level (default: 0)");
#endif

/*
 * constants of the SIO protocol
 */

#define MAX_COMMAND_FRAME_RETRIES 14
#define COMMAND_FRAME_ACK_CHAR 0x41
#define COMMAND_FRAME_NAK_CHAR 0x4e

#define OPERATION_COMPLETE_CHAR 0x43
#define OPERATION_ERROR_CHAR 0x45
#define DATA_FRAME_ACK_CHAR 0x41
#define DATA_FRAME_NAK_CHAR 0x4e


/*
 * delays, according to the SIO specs, in uSecs
 */

#define DELAY_T0 900
#define DELAY_T1 850
#define DELAY_T2_MIN 10
#define DELAY_T2_MAX 20000
#define DELAY_T3_MIN 1000
#define DELAY_T3_MAX 1600
#define DELAY_T4_MIN 450
#define DELAY_T4_MAX 16000

/* the QMEG OS needs at least 300usec delay between ACK and  complete */
/* #define DELAY_T5_MIN 250 */
#define DELAY_T5_MIN 300
#define DELAY_T5_MIN_SLOW 1000

/* this one is in mSecs, one jiffy (10 mSec on i386) is minimum,
 * try higher values for slower machines. The default of 50 mSecs
 * should be sufficient for most PCs
 */
#define DELAY_RECEIVE_HEADROOM 50
#define DELAY_SEND_HEADROOM 50

#define MODE_1050_2_PC 0
#define MODE_SIOSERVER 1

/*
 * IO buffers for the intterupt handlers
 */

#define IOBUF_LENGTH 8200
/*#define IOBUF_LENGTH 300*/

#define MAX_SIO_DATA_LENGTH (IOBUF_LENGTH-2)
/* circular buffers can hold IOBUF_LENGTH-1 chars, reserve one further
 * char for the checksum
 */

/* device state information */
struct atarisio_dev {
	int busy; /* =0; */
	int id;
	char* devname;
	int io;
	int irq;
	char* port; /* = 0 */
	/* lock for UART hardware registers */
	spinlock_t uart_lock; /* = SPIN_LOCK_UNLOCKED; */

	volatile int current_mode; /* = MODE_1050_2_PC; */

	/* timestamp at the very beginning of the IRQ */
	struct timeval irq_timestamp;
	
	/* value of modem status register at the last interrupt call */
	unsigned char last_msr; /* =0; */

	int enable_timestamp_recording; /* = 0; */
	SIO_timestamps timestamps;

	/* receive buffer */
	volatile struct rx_buf_struct {
		unsigned char buf[IOBUF_LENGTH];
		unsigned int head;
		unsigned int tail;
		int wakeup_len;
	} rx_buf;

	spinlock_t rx_lock; /* = SPIN_LOCK_UNLOCKED;*/

	/* transmit buffer */
	volatile struct tx_buf_struct {
		unsigned char buf[IOBUF_LENGTH];
		unsigned int head;
		unsigned int tail;
	} tx_buf;

	spinlock_t tx_lock; /* = SPIN_LOCK_UNLOCKED;*/

	unsigned int current_cmdframe_serial_number;

	/* command frame buffer */
	volatile struct cmdframe_buf_struct {
		unsigned char buf[5];
		unsigned int is_valid;
		unsigned int serial_number;
		unsigned int receiving;
		unsigned int pos;
		unsigned int error_counter;
		unsigned long long start_reception_time; /* in usec */
		unsigned long long end_reception_time; /* in usec */
		unsigned int missed_count;
	} cmdframe_buf;

	spinlock_t cmdframe_lock; /* = SPIN_LOCK_UNLOCKED; */

	/*
	 * wait queues
	 */

	wait_queue_head_t rx_queue;
	wait_queue_head_t tx_queue;
	wait_queue_head_t cmdframe_queue;

#ifdef ATARISIO_EARLY_NOTIFICATION
	wait_queue_head_t early_cmdframe_queue;
#endif

	/*
	 * configuration of the serial port
	 */

	volatile struct serial_config_struct {
		unsigned int baudrate;
		unsigned char IER;
		unsigned char MCR;
		unsigned char LCR;
		unsigned char FCR;
		unsigned long baud_base;
	} serial_config;

	spinlock_t serial_config_lock; /* = SPIN_LOCK_UNLOCKED; */


	/* 
	 * configuration for SIOSERVER mode
	 */

	unsigned char sioserver_command_line; /* = UART_MSR_RI; */
	unsigned char sioserver_command_line_delta; /* = UART_MSR_TERI; */

	/* 
	 * configuration for 1050-2-PC mode
	 */

	/*
	 * The command line has to be set to LOW during the
	 * transmission of a command frame and HI otherwise
	 */
	unsigned char command_line_mask; /* = ~UART_MCR_RTS; */
	unsigned char command_line_low; /* = UART_MCR_RTS; */
	unsigned char command_line_high; /* = 0; */

	unsigned int default_baudrate; /* = ATARISIO_STANDARD_BAUDRATE; */
	int do_autobaud; /* = 0; */
	int add_highspeedpause; /* = 0; */
	unsigned int tape_baudrate;

	unsigned char standard_lcr; /* = UART_LCR_WLEN8; */
	unsigned char slow_lcr; /* = UART_LCR_WLEN8 | UART_LCR_STOP | UART_LCR_PARITY | UART_LCR_SPAR; */

	/* serial port state */
	struct serial_struct orig_serial_struct;
	int need_reenable; /* = 0; */

	struct miscdevice* miscdev;
};

static struct atarisio_dev* atarisio_devices[ATARISIO_MAXDEV];

/*
 * helper functions to access the 16550
 */
static inline void serial_out(struct atarisio_dev* dev, unsigned int offset, u8 value)
{
	if (offset >=8) {
		DBG_PRINTK(DEBUG_STANDARD, "illegal offset in serial_out\n");
	} else {
		outb(value, dev->io+offset);
	}
}

static inline u8 serial_in(struct atarisio_dev* dev, unsigned int offset)
{
	if (offset >=8) {
		DBG_PRINTK(DEBUG_STANDARD, "illegal offset in serial_in\n");
		return 0;
	} else {
		return inb(dev->io+offset);
	}
}

static inline unsigned long long timeval_to_usec(struct timeval* tv)
{
	return (unsigned long long)tv->tv_sec*1000000 + tv->tv_usec;
}

/*
 * timing / timestamp functions
 */
static inline unsigned long long get_timestamp(void)
{
	struct timeval tv;
	do_gettimeofday(&tv);
	return timeval_to_usec(&tv);
}

static inline void timestamp_entering_ioctl(struct atarisio_dev* dev)
{
	if (dev->enable_timestamp_recording) {
		dev->timestamps.system_entering = get_timestamp();
		dev->timestamps.transmission_start = dev->timestamps.system_entering;
		dev->timestamps.transmission_end = dev->timestamps.system_entering;
		dev->timestamps.transmission_wakeup = dev->timestamps.system_entering;
		dev->timestamps.uart_finished = dev->timestamps.system_entering;
		dev->timestamps.system_leaving = dev->timestamps.system_entering;
	}
}

static inline void timestamp_transmission_start(struct atarisio_dev* dev)
{
	if (dev->enable_timestamp_recording) {
		dev->timestamps.transmission_start = get_timestamp();
	}
}

static inline void timestamp_transmission_end(struct atarisio_dev* dev)
{
	if (dev->enable_timestamp_recording) {
		dev->timestamps.transmission_end = get_timestamp();
	}
}

static inline void timestamp_transmission_wakeup(struct atarisio_dev* dev)
{
	if (dev->enable_timestamp_recording) {
		dev->timestamps.transmission_wakeup = get_timestamp();
	}
}

static inline void timestamp_uart_finished(struct atarisio_dev* dev)
{
	if (dev->enable_timestamp_recording) {
		dev->timestamps.uart_finished = get_timestamp();
	}
}

static inline void timestamp_leaving_ioctl(struct atarisio_dev* dev)
{
	if (dev->enable_timestamp_recording) {
		dev->timestamps.system_leaving = get_timestamp();
	}
}

static inline void reset_rx_buf(struct atarisio_dev* dev)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->rx_lock, flags);

	if (dev->rx_buf.head!=dev->rx_buf.tail) {
		DBG_PRINTK(DEBUG_STANDARD, "reset_rx_buf: flushing %d characters\n",
			(dev->rx_buf.head+IOBUF_LENGTH - dev->rx_buf.tail) % IOBUF_LENGTH);
	}

	dev->rx_buf.head=dev->rx_buf.tail=0;
	dev->rx_buf.wakeup_len=-1;

	spin_unlock_irqrestore(&dev->rx_lock, flags);
}

static inline void reset_tx_buf(struct atarisio_dev* dev)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->tx_lock, flags);

	if (dev->tx_buf.head!=dev->tx_buf.tail) {
		DBG_PRINTK(DEBUG_STANDARD, "reset_tx_buf: flushing %d characters\n",
			(dev->tx_buf.head+IOBUF_LENGTH - dev->tx_buf.tail) % IOBUF_LENGTH);
	}

	dev->tx_buf.head=dev->tx_buf.tail=0;

	spin_unlock_irqrestore(&dev->tx_lock, flags);
}

static inline void reset_cmdframe_buf(struct atarisio_dev* dev, int do_lock)
{
	unsigned long flags = 0;
	if (do_lock) {
		spin_lock_irqsave(&dev->cmdframe_lock, flags);
	}

	dev->cmdframe_buf.is_valid = 0;
	dev->cmdframe_buf.serial_number=0;
	dev->cmdframe_buf.receiving = 0;
	dev->cmdframe_buf.pos = 0;
	dev->cmdframe_buf.error_counter=0;
	dev->cmdframe_buf.start_reception_time=0;
	dev->cmdframe_buf.end_reception_time=0;
	dev->cmdframe_buf.missed_count=0;

	if (do_lock) {
		spin_unlock_irqrestore(&dev->cmdframe_lock, flags);
	}
}

/*
 * indicate the start of a command frame
 */
static inline void set_command_line(struct atarisio_dev* dev)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->uart_lock, flags);

	dev->serial_config.MCR = (dev->serial_config.MCR & dev->command_line_mask) | dev->command_line_low;
	serial_out(dev, UART_MCR, dev->serial_config.MCR);

	spin_unlock_irqrestore(&dev->uart_lock, flags);
}

/*
 * indicate the end of a command frame
 */
static inline void clear_command_line(struct atarisio_dev* dev, int lock_uart)
{
	unsigned long flags = 0;

	if (lock_uart) {
		spin_lock_irqsave(&dev->uart_lock, flags);
	}

	dev->serial_config.MCR = (dev->serial_config.MCR & dev->command_line_mask) | dev->command_line_high;
	serial_out(dev, UART_MCR, dev->serial_config.MCR);

	if (lock_uart) {
		spin_unlock_irqrestore(&dev->uart_lock, flags);
	}
}

static void set_lcr(struct atarisio_dev* dev, unsigned char lcr, int do_locks)
{
	unsigned long flags=0;

	if (do_locks) {
		spin_lock_irqsave(&dev->uart_lock, flags);
		spin_lock(&dev->serial_config_lock);
	}

	dev->serial_config.LCR = lcr;
	serial_out(dev, UART_LCR, dev->serial_config.LCR);

	if (do_locks) {
		spin_unlock(&dev->serial_config_lock);
		spin_unlock_irqrestore(&dev->uart_lock, flags);
	}
}

static int set_baudrate(struct atarisio_dev* dev, unsigned int baudrate, int do_locks)
{
	unsigned long flags=0;
	unsigned int divisor;

	if (do_locks) {
		spin_lock_irqsave(&dev->uart_lock, flags);
		spin_lock(&dev->serial_config_lock);
	}

	dev->serial_config.baudrate = baudrate;
	divisor = dev->serial_config.baud_base / baudrate;

	serial_out(dev, UART_LCR, dev->serial_config.LCR | UART_LCR_DLAB);
	serial_out(dev, UART_DLL, divisor & 0xff);
	serial_out(dev, UART_DLM, divisor >> 8);
	serial_out(dev, UART_LCR, dev->serial_config.LCR);

	if (do_locks) {
		spin_unlock(&dev->serial_config_lock);
		spin_unlock_irqrestore(&dev->uart_lock, flags);
	}

	return 0;
}


static inline unsigned char calculate_checksum(const unsigned char* circ_buf, int start, int len, int total_len)
{
	unsigned int chksum=0;
	int i;
	for (i=0;i<len;i++) {
		chksum += circ_buf[(start+i) % total_len];
		if (chksum >= 0x100) {
			chksum = (chksum & 0xff) + 1;
		}
	}
	return chksum;
}

static inline void reset_fifos(struct atarisio_dev* dev)
{
	IRQ_PRINTK(DEBUG_NOISY, "resetting 16550 fifos\n");

	serial_out(dev, UART_FCR, 0);
	serial_out(dev, UART_FCR, dev->serial_config.FCR | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
}

static inline void receive_chars(struct atarisio_dev* dev)
{
	unsigned char c,lsr;
	int do_wakeup=0;
	int first = 1;

	spin_lock(&dev->rx_lock);
	spin_lock(&dev->cmdframe_lock);
	
	while ( (lsr=serial_in(dev, UART_LSR)) & UART_LSR_DR ) {
		if (first && (dev->serial_config.LCR != dev->standard_lcr) ) {
			IRQ_PRINTK(DEBUG_STANDARD, "wrong LCR in receive chars!\n");
			spin_lock(&dev->serial_config_lock);
			set_lcr(dev, dev->standard_lcr, 0);
			spin_unlock(&dev->serial_config_lock);
		}
		first = 0;
		if (lsr & UART_LSR_OE) {
			IRQ_PRINTK(DEBUG_STANDARD, "overrun error\n");
		}
		c = serial_in(dev, UART_RX);

		if (lsr & UART_LSR_FE) {
			IRQ_PRINTK(DEBUG_NOISY, "got framing error\n");
			if (dev->cmdframe_buf.receiving) {
				dev->cmdframe_buf.error_counter++;
			}
		} else {
			if (dev->cmdframe_buf.receiving) {
				if (dev->cmdframe_buf.pos<5) {
					dev->cmdframe_buf.buf[dev->cmdframe_buf.pos] = c;
					IRQ_PRINTK(DEBUG_VERY_NOISY, "received cmdframe character 0x%02x\n",c);
				} else {
					IRQ_PRINTK(DEBUG_VERY_NOISY, "sinking cmdframe character 0x%02x\n",c);
					dev->cmdframe_buf.error_counter++;
				}
				dev->cmdframe_buf.pos++;
			} else {
				if ( (dev->rx_buf.head+1) % IOBUF_LENGTH != dev->rx_buf.tail ) {
					dev->rx_buf.buf[dev->rx_buf.head] = c;
					dev->rx_buf.head = (dev->rx_buf.head+1) % IOBUF_LENGTH;
					if ( (dev->rx_buf.head + IOBUF_LENGTH - dev->rx_buf.tail) % IOBUF_LENGTH == (unsigned int) dev->rx_buf.wakeup_len) {
						do_wakeup=1;
					}
					IRQ_PRINTK(DEBUG_VERY_NOISY, "received character 0x%02x\n",c);
				} else {
					IRQ_PRINTK(DEBUG_VERY_NOISY, "sinking character 0x%02x\n",c);
				}
			}
		}
	}
	spin_unlock(&dev->cmdframe_lock);
	spin_unlock(&dev->rx_lock);
	if ( do_wakeup ) {
		wake_up(&dev->rx_queue);
	}
}

static inline void send_chars(struct atarisio_dev* dev)
{
	int do_wakeup=0;
	int count = 16; /* FIFO can hold 16 chars at maximum */
	unsigned char lsr;

	lsr = serial_in(dev, UART_LSR);
	if (lsr & UART_LSR_OE) {
		IRQ_PRINTK(DEBUG_STANDARD, "overrun error\n");
	}
	if (lsr & UART_LSR_THRE) {
		spin_lock(&dev->tx_lock);
		while ( (count > 0) && (dev->tx_buf.head != dev->tx_buf.tail) ) {
			IRQ_PRINTK(DEBUG_VERY_NOISY, "transmit char 0x%02x\n",dev->tx_buf.buf[dev->tx_buf.head]);

			serial_out(dev, UART_TX, dev->tx_buf.buf[dev->tx_buf.tail]);
			dev->tx_buf.tail = (dev->tx_buf.tail+1) % IOBUF_LENGTH;
			count--;
		};
		if ( dev->tx_buf.head == dev->tx_buf.tail ) {
			/* end of TX-buffer reached, disable further TX-interrupts */
			spin_lock(&dev->serial_config_lock);

			dev->serial_config.IER &= ~UART_IER_THRI;
			serial_out(dev, UART_IER, dev->serial_config.IER);

			spin_unlock(&dev->serial_config_lock);

			timestamp_transmission_end(dev);

			do_wakeup=1;
		}
		spin_unlock(&dev->tx_lock);
		if (do_wakeup) {
			wake_up(&dev->tx_queue);
		}
	}
}

static inline void try_switchbaud(struct atarisio_dev* dev)
{
	unsigned int baud;

	spin_lock(&dev->serial_config_lock);

        baud= dev->serial_config.baudrate;

	if (dev->do_autobaud) {
		if (baud == ATARISIO_STANDARD_BAUDRATE) {
			baud = ATARISIO_HIGHSPEED_BAUDRATE;
		/*
		} else if (baud == 57600) {
			baud = 38400;
		*/
		} else {
			baud = ATARISIO_STANDARD_BAUDRATE;
		}

		IRQ_PRINTK(DEBUG_STANDARD, "switching to %d baud\n",baud);
	}

	set_baudrate(dev, baud, 0);

	spin_unlock(&dev->serial_config_lock);
}

static inline void check_modem_lines_before_receive(struct atarisio_dev* dev, unsigned char new_msr)
{
	if (dev->current_mode == MODE_SIOSERVER) {
		if ( (new_msr & dev->sioserver_command_line) != (dev->last_msr & dev->sioserver_command_line)) {
			IRQ_PRINTK(DEBUG_VERY_NOISY, "msr changed from 0x%02x to 0x%02x [%ld]\n",dev->last_msr, new_msr, jiffies);
			if (new_msr & dev->sioserver_command_line) {
				PRINT_TIMESTAMP("start of command frame");
				/* start of a new command frame */
				spin_lock(&dev->cmdframe_lock);
				if (dev->cmdframe_buf.is_valid) {
					IRQ_PRINTK(DEBUG_STANDARD, "invalidating command frame (detected new frame) %d\n", 
						dev->cmdframe_buf.missed_count);
					dev->cmdframe_buf.missed_count++;
				}
				if (dev->cmdframe_buf.receiving) {
					IRQ_PRINTK(DEBUG_STANDARD, "restarted reception of command frame (detected new frame)\n");
				}
				dev->cmdframe_buf.receiving = 1;
				dev->cmdframe_buf.start_reception_time = timeval_to_usec(&dev->irq_timestamp);
				dev->cmdframe_buf.pos = 0;
				dev->cmdframe_buf.is_valid = 0;
				dev->cmdframe_buf.error_counter = 0;
				dev->cmdframe_buf.serial_number++;

				spin_unlock(&dev->cmdframe_lock);

#ifdef ATARISIO_EARLY_NOTIFICATION
				wake_up(&dev->early_cmdframe_queue);
#endif
			}
		}
	}
}

#define DEBUG_PRINT_CMDFRAME_BUF(dev) \
	"cmdframe_buf: %02x %02x %02x %02x %02x err: %d pos=%d\n",\
	dev->cmdframe_buf.buf[0],\
	dev->cmdframe_buf.buf[1],\
	dev->cmdframe_buf.buf[2],\
	dev->cmdframe_buf.buf[3],\
	dev->cmdframe_buf.buf[4],\
	dev->cmdframe_buf.error_counter,\
	dev->cmdframe_buf.pos

static inline void check_modem_lines_after_receive(struct atarisio_dev* dev, unsigned char new_msr)
{
	int do_wakeup = 0;
	int OK = 1;
	unsigned char checksum;

	if (dev->current_mode == MODE_SIOSERVER) {
		/*
		 * check if the UART indicated a change on the command line from
		 * high to low. If yes, a complete (5 byte) command frame should
		 * have been received.
		 */
		if ( (new_msr & dev->sioserver_command_line_delta) && (!(new_msr & dev->sioserver_command_line)) ) {
			spin_lock(&dev->rx_lock);
			spin_lock(&dev->cmdframe_lock);


			if (dev->rx_buf.tail != dev->rx_buf.head) {
				IRQ_PRINTK(DEBUG_STANDARD, "flushing rx buffer (%d/%d)\n", dev->rx_buf.head, dev->rx_buf.tail);
			}
			if (dev->tx_buf.tail != dev->tx_buf.head) {
				IRQ_PRINTK(DEBUG_STANDARD, "flushing tx buffer (%d/%d)\n", dev->tx_buf.head, dev->tx_buf.tail);
			}

			dev->rx_buf.tail = dev->rx_buf.head; /* flush input buffer */
			dev->tx_buf.tail = dev->tx_buf.head; /* flush output buffer */

			if (dev->cmdframe_buf.pos != 5) {
				if (dev->cmdframe_buf.pos <=3 || dev->cmdframe_buf.pos >=7) {
					dev->cmdframe_buf.error_counter+=5;
				} else {
					dev->cmdframe_buf.error_counter++;
				}
				IRQ_PRINTK(DEBUG_NOISY, "got %d chars for this command frame\n",
					dev->cmdframe_buf.pos);
				OK = 0;
			}

			if (! dev->cmdframe_buf.receiving) {
			 	IRQ_PRINTK(DEBUG_NOISY, "indicated end of command frame, but I'm not currently receiving\n");
				/* the UART might have locked up, try resetting the FIFOs */
				reset_fifos(dev);
				OK = 0;
			}

			if (dev->cmdframe_buf.error_counter>0) {
				OK = 0;
			}

			if (OK) {
				IRQ_PRINTK(DEBUG_NOISY, DEBUG_PRINT_CMDFRAME_BUF(dev));

				checksum = calculate_checksum((unsigned char*)dev->cmdframe_buf.buf, 0, 4, 5);

				if (dev->cmdframe_buf.buf[4] == checksum) {
					PRINT_TIMESTAMP("found command frame");
					IRQ_PRINTK(DEBUG_NOISY, "found command frame [%ld]\n", jiffies);
					do_wakeup = 1;

					dev->cmdframe_buf.is_valid = 1;
					dev->cmdframe_buf.end_reception_time = timeval_to_usec(&dev->irq_timestamp);
					dev->cmdframe_buf.error_counter = 0;

				} else {
					IRQ_PRINTK(DEBUG_STANDARD, "command frame checksum error [%ld]\n",jiffies);
					IRQ_PRINTK(DEBUG_NOISY, DEBUG_PRINT_CMDFRAME_BUF(dev));

					dev->cmdframe_buf.is_valid = 0;
					dev->cmdframe_buf.error_counter++;
					dev->cmdframe_buf.pos=0;
				}
			} else {
				IRQ_PRINTK(DEBUG_STANDARD, "errors in command frame reception\n");
				IRQ_PRINTK(DEBUG_NOISY, DEBUG_PRINT_CMDFRAME_BUF(dev));
			}

			if (dev->cmdframe_buf.error_counter >= 3) {
				IRQ_PRINTK(DEBUG_NOISY, "error counter exceeded limit [%d]\n",dev->cmdframe_buf.error_counter);
				try_switchbaud(dev);

				/* some 16550 chips get confused if we switch the baudrate too
				 * fast. clearing the FIFOs solves this problem, but don't ask
				 * me why...
				 */
				reset_fifos(dev);

			}
			dev->cmdframe_buf.receiving = 0;

			spin_unlock(&dev->cmdframe_lock);
			spin_unlock(&dev->rx_lock);

			if (do_wakeup) {
				PRINT_TIMESTAMP("waking up cmdframe_queue");
				wake_up(&dev->cmdframe_queue);
			}
		}
	}
}

/*
 * the interrupt handler
 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
static irqreturn_t atarisio_interrupt(int irq, void* dev_id, struct pt_regs* regs)
#else
static irqreturn_t atarisio_interrupt(int irq, void* dev_id)
#endif
{
	struct atarisio_dev* dev = dev_id;
	unsigned char iir,msr;
	unsigned int handled = 0;

	do_gettimeofday(&dev->irq_timestamp);

	spin_lock(&dev->uart_lock);

	while (! ((iir=serial_in(dev, UART_IIR)) & UART_IIR_NO_INT)) {
		handled = 1;

		msr = serial_in(dev, UART_MSR);
		IRQ_PRINTK(DEBUG_VERY_NOISY, "atarisio_interrupt: IIR = 0x%02x MSR = 0x%02x\n", iir, msr);

		check_modem_lines_before_receive(dev, msr);

		receive_chars(dev);

		check_modem_lines_after_receive(dev, msr);

		send_chars(dev);

		dev->last_msr = msr;
	}

	spin_unlock(&dev->uart_lock);

	return IRQ_RETVAL(handled);
}

// check if a new command frame was received, before sending
// a command ACK/NAK
static inline int check_new_command_frame(struct atarisio_dev* dev)
{
	if (dev->current_mode != MODE_SIOSERVER) {
		return 0;
	}
	if (dev->cmdframe_buf.serial_number != dev->current_cmdframe_serial_number) {
		DBG_PRINTK(DEBUG_STANDARD, "new command frame has arrived...\n");
		return -EATARISIO_COMMAND_TIMEOUT;
	} else {
		return 0;
	}
}

static inline int check_command_frame_time(struct atarisio_dev* dev, int do_lock)
{
	unsigned long flags = 0;
	unsigned long long current_time;
	int ret = 0;
	unsigned int do_delay = 0;

	if (dev->current_mode != MODE_SIOSERVER) {
		return 0;
	}
	if (do_lock) {
		spin_lock_irqsave(&dev->cmdframe_lock, flags);
	}
	current_time = get_timestamp();

	if (current_time > dev->cmdframe_buf.end_reception_time+10000 ) {
		DBG_PRINTK(DEBUG_STANDARD, "command frame is too old (%lu usecs)\n",
				(unsigned long) (current_time - dev->cmdframe_buf.end_reception_time));
		ret = -EATARISIO_COMMAND_TIMEOUT;
	} else {
		DBG_PRINTK(DEBUG_NOISY, "command frame age is OK (%lu usecs)\n",
				(unsigned long) (current_time - dev->cmdframe_buf.end_reception_time));
		if (do_lock && (current_time - dev->cmdframe_buf.end_reception_time < DELAY_T2_MIN)) {
			do_delay = DELAY_T2_MIN-(current_time - dev->cmdframe_buf.end_reception_time);
		}
	}
	if (do_lock) {
		spin_unlock_irqrestore(&dev->cmdframe_lock, flags);
	}
	if (do_delay>0) {
		udelay(do_delay);
	}
	return ret;
}

static inline void initiate_send(struct atarisio_dev* dev)
{
	unsigned long flags;
	DBG_PRINTK(DEBUG_VERY_NOISY, "initiate_send: tx-head = %d tx-tail = %d\n", dev->tx_buf.head, dev->tx_buf.tail);

	timestamp_transmission_start(dev);

	if ( (dev->tx_buf.head != dev->tx_buf.tail)
	     && !(dev->serial_config.IER & UART_IER_THRI) ) {

		DBG_PRINTK(DEBUG_VERY_NOISY, "enabling TX-interrupt\n");

		spin_lock_irqsave(&dev->uart_lock, flags);
		spin_lock(&dev->serial_config_lock);

		dev->serial_config.IER |= UART_IER_THRI;
		serial_out(dev, UART_IER, dev->serial_config.IER);

		spin_unlock(&dev->serial_config_lock);
		spin_unlock_irqrestore(&dev->uart_lock, flags);
	}
}

static inline int wait_send(struct atarisio_dev* dev, unsigned int len)
{
	wait_queue_t wait;
	signed long timeout_jiffies;
	signed long expire;
	unsigned long max_jiffies;
	long timeout;

	timeout = 10*len*1000 / dev->serial_config.baudrate;
	timeout += DELAY_SEND_HEADROOM;

	timeout_jiffies = timeout*HZ/1000;
	if (timeout_jiffies <= 0) {
		timeout_jiffies = 1;
	}

	expire = timeout_jiffies;
	max_jiffies = jiffies + timeout_jiffies;

	/*
	 * first wait for the interrupt notification, telling us
	 * that the transmit buffer is empty
	 */
	init_waitqueue_entry(&wait, current);

	add_wait_queue(&dev->tx_queue, &wait);
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		if (dev->tx_buf.head == dev->tx_buf.tail) {
			break;
		}
		expire = schedule_timeout(expire);
		if (expire==0) {
			break;
		}
		if (signal_pending(current)) {
			current->state=TASK_RUNNING;
			remove_wait_queue(&dev->tx_queue, &wait);
			return -EINTR;
		}
	}
	current->state=TASK_RUNNING;
	remove_wait_queue(&dev->tx_queue, &wait);

	if ( dev->tx_buf.head != dev->tx_buf.tail ) {
		DBG_PRINTK(DEBUG_STANDARD, "timeout expired in wait_send\n");
		return -EATARISIO_COMMAND_TIMEOUT;
	}

	timestamp_transmission_wakeup(dev);

	/*
	 * now wait for the 16550 FIFO and transmitter to empty
	 */
	while ((jiffies < max_jiffies) && !(serial_in(dev, UART_LSR) & UART_LSR_TEMT)) {
	}

	timestamp_uart_finished(dev);

	if (jiffies >= max_jiffies) {
		DBG_PRINTK(DEBUG_STANDARD, "timeout expired in wait_send / TEMT\n");
		return -EATARISIO_COMMAND_TIMEOUT;
	} else {
		return 0;
	}
}

static inline int wait_receive(struct atarisio_dev* dev, int len, int additional_timeout)
{
	wait_queue_t wait;

	int no_received_chars=0;
	long timeout;
	signed long timeout_jiffies;
	signed long expire;
	signed long jiffies_start = jiffies;

	timeout = 10*len*1000 / dev->serial_config.baudrate;
	timeout += additional_timeout + DELAY_RECEIVE_HEADROOM;

	timeout_jiffies = timeout*HZ/1000;
	if (timeout_jiffies <= 0) {
		timeout_jiffies = 1;
	}

	expire = timeout_jiffies;

	init_waitqueue_entry(&wait, current);

	dev->rx_buf.wakeup_len = len;
	add_wait_queue(&dev->rx_queue, &wait);
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		if ( (no_received_chars=(dev->rx_buf.head+IOBUF_LENGTH - dev->rx_buf.tail) % IOBUF_LENGTH) >=len) {
			break;
		}
		expire = schedule_timeout(expire);
		if (expire==0) {
			break;
		}
		if (signal_pending(current)) {
			current->state=TASK_RUNNING;
			remove_wait_queue(&dev->rx_queue, &wait);
			return -EINTR;
		}
	}
	current->state=TASK_RUNNING;
	remove_wait_queue(&dev->rx_queue, &wait);

#ifdef ATARISIO_DEBUG_TIMEOUTS
	no_received_chars=(dev->rx_buf.head+IOBUF_LENGTH - dev->rx_buf.tail) % IOBUF_LENGTH;
	if (no_received_chars != len) {
		check_new_command_frame(dev);
		DBG_PRINTK(DEBUG_STANDARD, "timeout in wait_receive [wanted %d got %d time %ld], waiting again\n",
				len, no_received_chars, jiffies-jiffies_start);
		dev->rx_buf.wakeup_len = len;
		expire = ATARISIO_DEBUG_TIMEOUT_WAIT;
		add_wait_queue(&dev->rx_queue, &wait);
		while(1) {
			current->state = TASK_INTERRUPTIBLE;
			if ((no_received_chars=(dev->rx_buf.head+IOBUF_LENGTH - dev->rx_buf.tail) % IOBUF_LENGTH) >=len) {
				break;
			}
			expire = schedule_timeout(expire);
			if (expire==0) {
				break;
			}
			if (signal_pending(current)) {
				current->state=TASK_RUNNING;
				remove_wait_queue(&dev->rx_queue, &wait);
				return -EINTR;
			}
		}
		current->state=TASK_RUNNING;
		remove_wait_queue(&dev->rx_queue, &wait);
	}
#endif
	dev->rx_buf.wakeup_len=-1;
	no_received_chars=(dev->rx_buf.head+IOBUF_LENGTH - dev->rx_buf.tail) % IOBUF_LENGTH;
	if (no_received_chars < len) {
		DBG_PRINTK(DEBUG_STANDARD, "timeout in wait_receive [wanted %d got %d time %ld]\n",
				len, no_received_chars, jiffies - jiffies_start);
	}
	if (no_received_chars > len) {
		DBG_PRINTK(DEBUG_NOISY, "received more bytes than expected [expected %d got %d]\n",
				len, no_received_chars);
	}
	return no_received_chars;
}

static inline int send_command_frame(struct atarisio_dev* dev, SIO_parameters* params)
{
	int retry = 0;
	int last_err = 0;
	unsigned long flags;
	int i;
	unsigned char c;
	int w;

	unsigned char cmd_frame[5];

	cmd_frame[0] = params->device_id;
	cmd_frame[1] = params->command;
	cmd_frame[2] = params->aux1;
	cmd_frame[3] = params->aux2;
	cmd_frame[4] = calculate_checksum(cmd_frame, 0, 4, 5);

	while (retry < MAX_COMMAND_FRAME_RETRIES) {
		DBG_PRINTK(DEBUG_NOISY, "initiating command frame [%ld]\n",jiffies);

		dev->rx_buf.tail = dev->rx_buf.head; /* clear rx-buffer */

		set_command_line(dev);
		udelay(DELAY_T0);

		spin_lock_irqsave(&dev->tx_lock, flags);
		if (dev->tx_buf.head != dev->tx_buf.tail) {
			DBG_PRINTK(DEBUG_NOISY, "clearing tx bufffer\n");
			dev->tx_buf.head = dev->tx_buf.tail;
		}
		for (i=0;i<5;i++) {
			dev->tx_buf.buf[dev->tx_buf.head+i] = cmd_frame[i];
		}
		dev->tx_buf.head = (dev->tx_buf.head+5) % IOBUF_LENGTH;

		initiate_send(dev);
		spin_unlock_irqrestore(&dev->tx_lock, flags);
		if ((w=wait_send(dev, 5))) {
			if (w == -EATARISIO_COMMAND_TIMEOUT) {
				last_err = w;
				goto again;
			} else {
				DBG_PRINTK(DEBUG_STANDARD, "sending command frame returned %d\n",w);
				return w;
			}
		}

		udelay(DELAY_T1);
		clear_command_line(dev, 1);
		udelay(DELAY_T2_MIN);

		PRINT_TIMESTAMP("begin wait for command ACK");
		w=wait_receive(dev, 1, DELAY_T2_MAX/1000);
		PRINT_TIMESTAMP("end wait for command ACK");

		if (w < 1) {
			if (w < 0) {
				DBG_PRINTK(DEBUG_STANDARD, "wait_receive returned %d\n",w);
				return w;
			} else {
				DBG_PRINTK(DEBUG_STANDARD, "waiting for command frame ACK timed out [%ld]\n", jiffies);

				last_err = -EATARISIO_COMMAND_TIMEOUT;
				goto again;
			}
		}
		c = dev->rx_buf.buf[dev->rx_buf.tail];
		dev->rx_buf.tail = (dev->rx_buf.tail+1) % IOBUF_LENGTH;
		if (c==COMMAND_FRAME_ACK_CHAR) {
			break;
		} else if (c==COMMAND_FRAME_NAK_CHAR) {
			DBG_PRINTK(DEBUG_STANDARD, "got command frame NAK\n");
			last_err = -EATARISIO_COMMAND_NAK;
		} else {
			DBG_PRINTK(DEBUG_STANDARD, "illegal response to command frame: 0x%02x\n",c);
		}
		if (retry+1 < MAX_COMMAND_FRAME_RETRIES) {
			DBG_PRINTK(DEBUG_NOISY, "retrying command frame\n");
		}

again:
		clear_command_line(dev, 1);
		{
			signed long t = HZ / 5;
			current->state=TASK_INTERRUPTIBLE;
			while (t) {
				t = schedule_timeout(t);
				if (signal_pending(current)) {
					current->state=TASK_RUNNING;
					return -EINTR;
				}
			}
			current->state=TASK_RUNNING;
		}
		retry++;
	}
		
	if (retry == MAX_COMMAND_FRAME_RETRIES) {
		DBG_PRINTK(DEBUG_STANDARD, "request timed out - no ACK to command frame\n");

		if (last_err) {
			return last_err;
		} else {
			return -EATARTSIO_UNKNOWN_ERROR;
		}
	} else {
		DBG_PRINTK(DEBUG_NOISY, "got ACK for command frame - ready to proceed\n");
		return 0;
	}
}

static int setup_send_frame(struct atarisio_dev* dev, unsigned int data_length, unsigned char* user_buffer, int add_checksum)
{
	unsigned long flags;
	unsigned int len, remain;
	unsigned char checksum;

	if ((data_length == 0) || (data_length >= MAX_SIO_DATA_LENGTH)) {
		return -EINVAL;
	}

	spin_lock_irqsave(&dev->tx_lock, flags);
	dev->tx_buf.head = dev->tx_buf.tail;

	len = IOBUF_LENGTH - dev->tx_buf.head;
	if (len > data_length) {
		len = data_length;
	}
	if ( copy_from_user((unsigned char*) (&(dev->tx_buf.buf[dev->tx_buf.head])), 
			user_buffer,
			len) ) {
		return -EFAULT;
	}
	remain = data_length - len;
	if (remain>0) {
		if ( copy_from_user((unsigned char*) dev->tx_buf.buf,
				user_buffer+len,
				remain) ) {
			return -EFAULT;
		}
	}

	if (add_checksum) {
		checksum = calculate_checksum((unsigned char*)(dev->tx_buf.buf),
				dev->tx_buf.head, data_length, IOBUF_LENGTH);

		dev->tx_buf.buf[(dev->tx_buf.head+data_length) % IOBUF_LENGTH] = checksum;
		data_length++;
	}

	dev->tx_buf.head = (dev->tx_buf.head + data_length) % IOBUF_LENGTH;
	spin_unlock_irqrestore(&dev->tx_lock, flags);

	return 0;
}

static int setup_send_data_frame(struct atarisio_dev* dev, unsigned int data_length, unsigned char* user_buffer)
{
	return setup_send_frame(dev, data_length, user_buffer, 1);
}

static int setup_send_raw_frame(struct atarisio_dev* dev, unsigned int data_length, unsigned char* user_buffer)
{
	return setup_send_frame(dev, data_length, user_buffer, 0);
}

static int copy_received_data_to_user(struct atarisio_dev* dev, unsigned int data_length, unsigned char* user_buffer)
{
	unsigned int len, remain;
	if ((data_length == 0) || (data_length >= MAX_SIO_DATA_LENGTH)) {
		return -EINVAL;
	}
	/* if we received any bytes, copy the to the user buffer, even
	 * if we ran into a timeout and got less than we wanted
	 */
	len = IOBUF_LENGTH - dev->rx_buf.tail;
	if (len > data_length) {
		len = data_length;
	}
	if ( copy_to_user(user_buffer,
			 (unsigned char*) (&(dev->rx_buf.buf[dev->rx_buf.tail])),
			 len) ) {
		return -EFAULT;
	}
	remain = data_length - len;
	if (remain>0) {
		if ( copy_to_user(user_buffer+len,
				 (unsigned char*) dev->rx_buf.buf,
				 remain) ) {
			return -EFAULT;
		}
	}
	return 0;
}

static int perform_sio_receive_data(struct atarisio_dev* dev, SIO_parameters * sio_params)
{
	int received_len, copy_len;
        int ret=0,w;
	unsigned char c, checksum;
	/*
	 * read data from disk
	 */
	if ((w=wait_receive(dev, 1, sio_params->timeout*1000)) < 1) {
		if (w < 0) {
			DBG_PRINTK(DEBUG_STANDARD, "wait_receive returned %d\n",w);
			return w;
		} else {
			DBG_PRINTK(DEBUG_STANDARD, "command complete timed out\n");
			return -EATARISIO_COMMAND_TIMEOUT;
		}
	}
	ret = 0;
	c = dev->rx_buf.buf[dev->rx_buf.tail];
	dev->rx_buf.tail = (dev->rx_buf.tail+1) % IOBUF_LENGTH;

	if (c != OPERATION_COMPLETE_CHAR) {
		if (c == COMMAND_FRAME_ACK_CHAR) {
			DBG_PRINTK(DEBUG_STANDARD, "got command ACK instead of command complete\n");
		} else {
			if (c != OPERATION_ERROR_CHAR) {
				DBG_PRINTK(DEBUG_STANDARD, "wanted command complete, got: 0x%02x\n",c);
			} else {
				DBG_PRINTK(DEBUG_STANDARD, "got sio command error\n");
			}
			ret = -EATARISIO_COMMAND_COMPLETE_ERROR;
		}
	}

	if (sio_params->data_length) {
		/* receive data block and checksum*/
		received_len = wait_receive(dev, sio_params->data_length+1, 0);

		if (received_len < 0) {
			DBG_PRINTK(DEBUG_STANDARD, "receiving data block returned %d\n",received_len);
			return received_len;
		}

		copy_len = sio_params->data_length;
		if (received_len < copy_len) {
			copy_len = received_len;
		}
		if (copy_len > 0) {
			if ( (w=copy_received_data_to_user(dev, copy_len, sio_params->data_buffer)) ) {
				return w;
			}
		}

	        if ((unsigned int) received_len < sio_params->data_length+1) {
			DBG_PRINTK(DEBUG_STANDARD, "receive data frame timed out [wanted %d got %d]\n",
					sio_params->data_length+1, received_len);
			return -EATARISIO_COMMAND_TIMEOUT;
		}
		checksum = calculate_checksum((unsigned char*)(dev->rx_buf.buf),
			       	dev->rx_buf.tail, sio_params->data_length, IOBUF_LENGTH);

		if (dev->rx_buf.buf[(dev->rx_buf.tail+sio_params->data_length) % IOBUF_LENGTH ]
			!= checksum) {
			DBG_PRINTK(DEBUG_STANDARD, "checksum error : 0x%02x != 0x%02x\n",
				checksum,
				dev->rx_buf.buf[(dev->rx_buf.tail+sio_params->data_length) % IOBUF_LENGTH]);
			if (! ret) {
				ret = -EATARISIO_CHECKSUM_ERROR;
			}
		}


		dev->rx_buf.tail = (dev->rx_buf.tail + sio_params->data_length + 1 ) % IOBUF_LENGTH;

	}

	if (dev->rx_buf.head != dev->rx_buf.tail) {
		DBG_PRINTK(DEBUG_NOISY, "detected excess characters\n");
	}
	return ret;
}

static int perform_sio_send_data(struct atarisio_dev* dev, SIO_parameters * sio_params)
{
        int ret=0,w;
	unsigned char c;

	/*
	 * read data from disk
	 */
	udelay(DELAY_T3_MIN);


	if (sio_params->data_length) {
		if ( (ret=setup_send_data_frame(dev, sio_params->data_length, sio_params->data_buffer)) ) {
			return ret;
		}

		initiate_send(dev);

		if ((w=wait_send(dev, sio_params->data_length+1))) {
			DBG_PRINTK(DEBUG_STANDARD, "send data frame returned %d\n",w);
			return w;
		}

		/*
		 * wait for data frame ack
		 */

		if ((w=wait_receive(dev, 1, DELAY_T4_MAX/1000)) < 1) {
			if (w < 0) {
				DBG_PRINTK(DEBUG_STANDARD, "receive data frame ACK returned %d\n",w);
				return w;
			} else {
				DBG_PRINTK(DEBUG_STANDARD, "receive data frame ACK timed out\n");
				return -EATARISIO_COMMAND_TIMEOUT;
			}
		}
		c = dev->rx_buf.buf[dev->rx_buf.tail];
		dev->rx_buf.tail = (dev->rx_buf.tail + 1) % IOBUF_LENGTH;

		if (c != DATA_FRAME_ACK_CHAR) {
			if (c != DATA_FRAME_NAK_CHAR) {
				DBG_PRINTK(DEBUG_STANDARD, "wanted data ACK or ERROR, got: 0x%02x\n",c);
			}
			return -EATARISIO_DATA_NAK;
		}
	}

	/* 
	 * wait for command complete
	 */
	if ((w=wait_receive(dev, 1, sio_params->timeout*1000)) < 1) {
		if (w < 0) {
			DBG_PRINTK(DEBUG_STANDARD, "receive command complete returned %d\n",w);
			return w;
		} else {
			DBG_PRINTK(DEBUG_STANDARD, "receive command complete timed out\n");
			return -EATARISIO_COMMAND_TIMEOUT;
		}
	}
	c = dev->rx_buf.buf[dev->rx_buf.tail];
	dev->rx_buf.tail = (dev->rx_buf.tail + 1) % IOBUF_LENGTH;

	if (c != OPERATION_COMPLETE_CHAR) {
		if (c == COMMAND_FRAME_ACK_CHAR) {
			DBG_PRINTK(DEBUG_STANDARD, "got command ACK instead of command complete\n");
		} else {
			if (c != OPERATION_ERROR_CHAR) {
				DBG_PRINTK(DEBUG_STANDARD, "wanted command complete, got: 0x%02x\n",c);
			} else {
				DBG_PRINTK(DEBUG_STANDARD, "got sio command error\n");
			}
			ret = -EATARISIO_COMMAND_COMPLETE_ERROR;
		}
	}
	return ret;
}

static inline int perform_sio(struct atarisio_dev* dev, SIO_parameters * user_sio_params)
{
	int len;
	int ret=0;
	SIO_parameters sio_params;

	len = sizeof(sio_params);
	if (copy_from_user(&sio_params, user_sio_params, sizeof(sio_params))) {
		DBG_PRINTK(DEBUG_STANDARD, "copy_from_user failed for SIO parameters\n");
		return -EFAULT;
	}

	if (sio_params.data_length >= MAX_SIO_DATA_LENGTH) {
		return -EATARISIO_ERROR_BLOCK_TOO_LONG;
	}

	DBG_PRINTK(DEBUG_VERY_NOISY, "performing SIO:\n");
	DBG_PRINTK(DEBUG_VERY_NOISY, "unit=%d cmd=%d dir=%d timeout=%d aux1=%d aux2=%d datalen=%d\n",
		sio_params.device_id, sio_params.command, sio_params.direction,
		sio_params.timeout, sio_params.aux1, sio_params.aux2, sio_params.data_length);

	if ((ret = send_command_frame(dev, &sio_params))) {
		return ret;
	}

	if (sio_params.direction == 0) {
		ret = perform_sio_receive_data(dev, &sio_params);
	} else {
		ret = perform_sio_send_data(dev, &sio_params);
	}
	return ret;
}

static void set_1050_2_pc_mode(struct atarisio_dev* dev, int do_locks)
{
	unsigned long flags=0;
	int have_to_wait;

	if (do_locks) {
		spin_lock_irqsave(&dev->uart_lock, flags);
		spin_lock(&dev->serial_config_lock);
	}

	have_to_wait = !(dev->serial_config.MCR & UART_MCR_DTR);

	dev->current_mode = MODE_1050_2_PC;
	dev->command_line_mask = ~(UART_MCR_RTS | UART_MCR_DTR);
	dev->command_line_low = UART_MCR_DTR | UART_MCR_RTS;
	dev->command_line_high = UART_MCR_DTR;
	clear_command_line(dev, 0);

	set_lcr(dev, dev->standard_lcr, 0);
	dev->do_autobaud = 0;
	set_baudrate(dev, dev->default_baudrate, 0);
	dev->serial_config.IER &= ~UART_IER_MSI;
	serial_out(dev, UART_IER, dev->serial_config.IER);

	if (do_locks) {
		spin_unlock(&dev->serial_config_lock);
		spin_unlock_irqrestore(&dev->uart_lock, flags);
		if (have_to_wait) {
			schedule_timeout(HZ/2); /* wait 0.5 sec. for voltage supply to stabilize */
		}
	}

	reset_cmdframe_buf(dev, 1);
}

static void set_prosystem_mode(struct atarisio_dev* dev, int do_locks)
{
	unsigned long flags = 0;
	int have_to_wait;

	if (do_locks) {
		spin_lock_irqsave(&dev->uart_lock, flags);
		spin_lock(&dev->serial_config_lock);
	}

	have_to_wait = !(dev->serial_config.MCR & UART_MCR_RTS);

	dev->current_mode = MODE_1050_2_PC;
	dev->command_line_mask = ~(UART_MCR_RTS | UART_MCR_DTR);
	dev->command_line_low = UART_MCR_RTS | UART_MCR_DTR;
	dev->command_line_high = UART_MCR_RTS;
	clear_command_line(dev, 0);
	set_lcr(dev, dev->standard_lcr, 0);
	dev->do_autobaud = 0;
	set_baudrate(dev, dev->default_baudrate, 0);
	dev->serial_config.IER &= ~UART_IER_MSI;
	serial_out(dev, UART_IER, dev->serial_config.IER);

	if (do_locks) {
		spin_unlock(&dev->serial_config_lock);
		spin_unlock_irqrestore(&dev->uart_lock, flags);
		if (have_to_wait) {
			schedule_timeout(HZ/2); /* wait 0.5 sec. for voltage supply to stabilize */
		}
	}

	reset_cmdframe_buf(dev, 1);
}

/* clear DTR and RTS to make autoswitching Atarimax interface work */
static void clear_control_lines(struct atarisio_dev* dev)
{
	dev->serial_config.MCR &= ~(UART_MCR_DTR | UART_MCR_RTS);
	serial_out(dev, UART_MCR, dev->serial_config.MCR);
}

static void set_sioserver_mode(struct atarisio_dev* dev, int do_locks)
{
	unsigned long flags = 0;

	if (do_locks) {
		spin_lock_irqsave(&dev->uart_lock, flags);
		spin_lock(&dev->serial_config_lock);
	}

	dev->current_mode = MODE_SIOSERVER;
	dev->sioserver_command_line = UART_MSR_RI;
	dev->sioserver_command_line_delta = UART_MSR_TERI;

	set_lcr(dev, dev->standard_lcr, 0);
	set_baudrate(dev, dev->default_baudrate, 0);
	clear_control_lines(dev);
	dev->serial_config.IER |= UART_IER_MSI;
	serial_out(dev, UART_IER, dev->serial_config.IER);

	if (do_locks) {
		spin_unlock(&dev->serial_config_lock);
		spin_unlock_irqrestore(&dev->uart_lock, flags);
	}

	reset_cmdframe_buf(dev, 1);
}

static void set_sioserver_mode_dsr(struct atarisio_dev* dev, int do_locks)
{
	unsigned long flags = 0;

	if (do_locks) {
		spin_lock_irqsave(&dev->uart_lock, flags);
		spin_lock(&dev->serial_config_lock);
	}

	dev->current_mode = MODE_SIOSERVER;
	dev->sioserver_command_line = UART_MSR_DSR;
	dev->sioserver_command_line_delta = UART_MSR_DDSR;

	set_lcr(dev, dev->standard_lcr, 0);
	set_baudrate(dev, dev->default_baudrate, 0);
	clear_control_lines(dev);
	dev->serial_config.IER |= UART_IER_MSI;
	serial_out(dev, UART_IER, dev->serial_config.IER);

	if (do_locks) {
		spin_unlock(&dev->serial_config_lock);
		spin_unlock_irqrestore(&dev->uart_lock, flags);
	}

	reset_cmdframe_buf(dev, 1);
}

static void set_sioserver_mode_cts(struct atarisio_dev* dev, int do_locks)
{
	unsigned long flags = 0;

	if (do_locks) {
		spin_lock_irqsave(&dev->uart_lock, flags);
		spin_lock(&dev->serial_config_lock);
	}

	dev->current_mode = MODE_SIOSERVER;
	dev->sioserver_command_line = UART_MSR_CTS;
	dev->sioserver_command_line_delta = UART_MSR_DCTS;

	set_lcr(dev, dev->standard_lcr, 0);
	set_baudrate(dev, dev->default_baudrate, 0);
	clear_control_lines(dev);
	dev->serial_config.IER |= UART_IER_MSI;
	serial_out(dev, UART_IER, dev->serial_config.IER);

	if (do_locks) {
		spin_unlock(&dev->serial_config_lock);
		spin_unlock_irqrestore(&dev->uart_lock, flags);
	}

	reset_cmdframe_buf(dev, 1);
}


static int get_command_frame(struct atarisio_dev* dev, unsigned long arg)
{
	wait_queue_t wait;
	SIO_command_frame frame;
	signed long expire = HZ;
	signed long end_time = jiffies + HZ;
	unsigned long flags;

again:
	expire = end_time - jiffies;
	if (expire <= 0) {
		return -EATARISIO_COMMAND_TIMEOUT;
	}

	init_waitqueue_entry(&wait, current);

	add_wait_queue(&dev->cmdframe_queue, &wait);
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		if ( dev->cmdframe_buf.is_valid ) {
			PRINT_TIMESTAMP("found valid command frame");
			DBG_PRINTK(DEBUG_NOISY, "found valid command frame\n");
			break;
		}
		expire = schedule_timeout(expire);
		if (expire==0) {
			break;
		}
		if (signal_pending(current)) {
			current->state=TASK_RUNNING;
			remove_wait_queue(&dev->cmdframe_queue, &wait);
			DBG_PRINTK(DEBUG_STANDARD, "got signal while waiting for command frame\n");
			return -EINTR;
		}
	}
	current->state=TASK_RUNNING;
	remove_wait_queue(&dev->cmdframe_queue, &wait);

	if (!dev->cmdframe_buf.is_valid ) {
		DBG_PRINTK(DEBUG_STANDARD, "waiting for command frame timed out\n");
		return -EATARISIO_COMMAND_TIMEOUT;
	}
	spin_lock_irqsave(&dev->cmdframe_lock, flags);
/*
	if (!cmdframe_buf.is_valid) {
		DBG_PRINTK(DEBUG_STANDARD, "command frame invalidated shortly after notification\n");
		spin_unlock_irqrestore(&cmdframe_lock, flags);
		goto again;
	}
*/

	if (check_command_frame_time(dev, 0)) {
		dev->cmdframe_buf.is_valid = 0;
		spin_unlock_irqrestore(&dev->cmdframe_lock, flags);
		goto again;
	}

	frame.device_id   = dev->cmdframe_buf.buf[0];
	frame.command     = dev->cmdframe_buf.buf[1];
	frame.aux1        = dev->cmdframe_buf.buf[2];
	frame.aux2        = dev->cmdframe_buf.buf[3];
	frame.reception_timestamp = dev->cmdframe_buf.end_reception_time;
	frame.missed_count = dev->cmdframe_buf.missed_count;
	dev->cmdframe_buf.missed_count=0;
	dev->current_cmdframe_serial_number = dev->cmdframe_buf.serial_number;
	dev->cmdframe_buf.is_valid = 0; /* already got that :-) */
	reset_tx_buf(dev);
	reset_rx_buf(dev);
	spin_unlock_irqrestore(&dev->cmdframe_lock, flags);

	if (copy_to_user((SIO_command_frame*) arg, &frame, sizeof(SIO_command_frame)) ) {
		return -EFAULT;
	}
	return 0;
}

static int send_single_character(struct atarisio_dev* dev, unsigned char c)
{
	int w;
	dev->tx_buf.buf[dev->tx_buf.head]=c;
	dev->tx_buf.head = (dev->tx_buf.head + 1) % IOBUF_LENGTH;
	initiate_send(dev);
	w=wait_send(dev, 1);
	return w;
}

static int perform_send_data_frame(struct atarisio_dev* dev, unsigned long arg)
{
	SIO_data_frame frame;
	int ret,w;

	if (copy_from_user(&frame, (SIO_data_frame*) arg, sizeof(SIO_data_frame)) ) {
		return -EFAULT;
	}

	udelay(DELAY_T3_MIN);


	if (frame.data_length) {
		if (dev->add_highspeedpause && (dev->serial_config.baudrate != ATARISIO_STANDARD_BAUDRATE)) {
			set_lcr(dev, dev->slow_lcr, 1);
		}
		if ((ret=setup_send_data_frame(dev, frame.data_length, frame.data_buffer)) ) {
			if (dev->add_highspeedpause && (dev->serial_config.baudrate != ATARISIO_STANDARD_BAUDRATE)) {
				set_lcr(dev, dev->standard_lcr, 1);
			}
			return ret;
		}

		PRINT_TIMESTAMP("perform_send_data_frame: initiate_send");
		initiate_send(dev);

		PRINT_TIMESTAMP("perform_send_data_frame: begin wait_send");
		if ((w=wait_send(dev, frame.data_length+1))) {
			if (dev->add_highspeedpause && (dev->serial_config.baudrate != ATARISIO_STANDARD_BAUDRATE)) {
				set_lcr(dev, dev->standard_lcr, 1);
			}
			DBG_PRINTK(DEBUG_STANDARD, "send data frame returned %d\n",w);
			return w;
		}
		if (dev->add_highspeedpause && (dev->serial_config.baudrate != ATARISIO_STANDARD_BAUDRATE)) {
			set_lcr(dev, dev->standard_lcr, 1);
		}
		PRINT_TIMESTAMP("perform_send_data_frame: end wait_send");
	}
	return 0;
}

static int perform_receive_data_frame(struct atarisio_dev* dev, unsigned long arg)
{
	SIO_data_frame frame;
	int ret=0;
	int received_len, copy_len;
	unsigned char checksum;

	if (copy_from_user(&frame, (SIO_data_frame*) arg, sizeof(SIO_data_frame)) ) {
		return -EFAULT;
	}


	if (frame.data_length) {
		/* receive data block and checksum*/
		received_len = wait_receive(dev, frame.data_length+1, DELAY_T3_MAX/1000);

		if (received_len < 0) {
			DBG_PRINTK(DEBUG_STANDARD, "receive data block returned %d\n",received_len);
			return received_len;
		}
		copy_len = frame.data_length;
		if (received_len < copy_len) {
			copy_len = received_len;
		}
		if (copy_len > 0) {
			if ( (ret=copy_received_data_to_user(dev, copy_len, frame.data_buffer)) ) {
				return ret;
			}
		}

	        if ((unsigned int) received_len < frame.data_length+1) {
			DBG_PRINTK(DEBUG_STANDARD, "receive data frame timed out [wanted %d got %d]\n",
					frame.data_length+1, received_len);
			return -EATARISIO_COMMAND_TIMEOUT;
		}
		checksum = calculate_checksum((unsigned char*)(dev->rx_buf.buf),
			       	dev->rx_buf.tail, frame.data_length, IOBUF_LENGTH);

		if (dev->rx_buf.buf[(dev->rx_buf.tail+frame.data_length) % IOBUF_LENGTH ]
			!= checksum) {
			DBG_PRINTK(DEBUG_STANDARD, "checksum error : 0x%02x != 0x%02x\n",
				checksum,
				dev->rx_buf.buf[(dev->rx_buf.tail+frame.data_length) % IOBUF_LENGTH]);
			if (! ret) {
				ret = -EATARISIO_CHECKSUM_ERROR;
			}
		}


		dev->rx_buf.tail = (dev->rx_buf.tail + frame.data_length + 1 ) % IOBUF_LENGTH;

		udelay(DELAY_T4_MIN);
		if (ret == 0) {
			ret=send_single_character(dev, DATA_FRAME_ACK_CHAR);
		} else {
			send_single_character(dev, DATA_FRAME_NAK_CHAR);
		}
	} else {
		return -EINVAL;
	}

	if (dev->rx_buf.head != dev->rx_buf.tail) {
		DBG_PRINTK(DEBUG_NOISY, "detected excess characters\n");
	}
	return ret;
}

static int perform_send_raw_frame(struct atarisio_dev* dev, unsigned long arg)
{
	SIO_data_frame frame;
	int ret,w;

	if (copy_from_user(&frame, (SIO_data_frame*) arg, sizeof(SIO_data_frame)) ) {
		return -EFAULT;
	}

	if (frame.data_length) {
		if (dev->add_highspeedpause && (dev->serial_config.baudrate != ATARISIO_STANDARD_BAUDRATE)) {
			set_lcr(dev, dev->slow_lcr, 1);
		}
		if ((ret=setup_send_raw_frame(dev, frame.data_length, frame.data_buffer)) ) {
			if (dev->add_highspeedpause && (dev->serial_config.baudrate != ATARISIO_STANDARD_BAUDRATE)) {
				set_lcr(dev, dev->standard_lcr, 1);
			}
			return ret;
		}

		PRINT_TIMESTAMP("perform_send_raw_frame: initiate_send");
		initiate_send(dev);

		PRINT_TIMESTAMP("perform_send_raw_frame: begin wait_send");
		if ((w=wait_send(dev, frame.data_length))) {
			if (dev->add_highspeedpause && (dev->serial_config.baudrate != ATARISIO_STANDARD_BAUDRATE)) {
				set_lcr(dev, dev->standard_lcr, 1);
			}
			DBG_PRINTK(DEBUG_STANDARD, "send raw frame returned %d\n",w);
			return w;
		}
		if (dev->add_highspeedpause && (dev->serial_config.baudrate != ATARISIO_STANDARD_BAUDRATE)) {
			set_lcr(dev, dev->standard_lcr, 1);
		}
		PRINT_TIMESTAMP("perform_send_raw_frame: end wait_send");
	}
	return 0;
}

static int perform_receive_raw_frame(struct atarisio_dev* dev, unsigned long arg)
{
	SIO_data_frame frame;
	int ret=0;
	int received_len, copy_len;

	if (copy_from_user(&frame, (SIO_data_frame*) arg, sizeof(SIO_data_frame)) ) {
		return -EFAULT;
	}

	if (frame.data_length) {
		/* receive data block and checksum*/
		received_len = wait_receive(dev, frame.data_length, DELAY_T3_MAX/1000);

		if (received_len < 0) {
			DBG_PRINTK(DEBUG_STANDARD, "receive data block returned %d\n",received_len);
			return received_len;
		}
		copy_len = frame.data_length;
		if (received_len < copy_len) {
			copy_len = received_len;
		}
		if (copy_len > 0) {
			if ( (ret=copy_received_data_to_user(dev, copy_len, frame.data_buffer)) ) {
				return ret;
			}
		}

	        if ((unsigned int) received_len < frame.data_length) {
			DBG_PRINTK(DEBUG_STANDARD, "receive raw frame timed out [wanted %d got %d]\n",
					frame.data_length+1, received_len);
			return -EATARISIO_COMMAND_TIMEOUT;
		}
		dev->rx_buf.tail = (dev->rx_buf.tail + frame.data_length) % IOBUF_LENGTH;
	} else {
		return -EINVAL;
	}

	if (dev->rx_buf.head != dev->rx_buf.tail) {
		DBG_PRINTK(DEBUG_NOISY, "detected excess characters\n");
	}
	return ret;
}

static int perform_send_tape_block(struct atarisio_dev* dev, unsigned long arg)
{
	SIO_data_frame frame;
	unsigned int current_baudrate;
	unsigned int current_autobaud;
	int ret = 0;
	int w;

	if (copy_from_user(&frame, (SIO_data_frame*) arg, sizeof(SIO_data_frame)) ) {
		return -EFAULT;
	}
	current_baudrate = dev->serial_config.baudrate;
	current_autobaud = dev->do_autobaud;

	/* disable autobauding and set the baudrate */
	dev->do_autobaud = 0;
	ret = set_baudrate(dev, dev->tape_baudrate, 1);

	if (ret) {
		DBG_PRINTK(DEBUG_STANDARD, "setting tape baudrate %d failed\n", dev->tape_baudrate);
		goto exit_failure;
	}
	
	if (frame.data_length) {
		if ((ret=setup_send_raw_frame(dev, frame.data_length, frame.data_buffer)) ) {
			goto exit_failure;
		}

		PRINT_TIMESTAMP("perform_send_tape_block: initiate_send");
		initiate_send(dev);

		PRINT_TIMESTAMP("perform_send_tape_block: begin wait_send");
		if ((w=wait_send(dev, frame.data_length))) {
			DBG_PRINTK(DEBUG_STANDARD, "send tape block returned %d\n",w);
			ret = w;
			goto exit_failure;
		}
		PRINT_TIMESTAMP("perform_send_tape_block: end wait_send");
	} else {
		DBG_PRINTK(DEBUG_STANDARD, "warning: called perform_send_tape_block with empty tape frame\n");
	}

exit_failure:
	/* reset baudrate */
	set_baudrate(dev, current_baudrate, 1);
	dev->do_autobaud = current_autobaud;
	return ret;

}

static void print_status(struct atarisio_dev* dev)
{
	unsigned long flags;

	switch (dev->current_mode) {
	case MODE_1050_2_PC:
		PRINTK("current_mode: 1050-2-PC\n"); break;
	case MODE_SIOSERVER:
		PRINTK("current_mode: sioserver\n"); break;
	default:
		PRINTK("current_mode: unknown - %d\n", dev->current_mode);
		break;
	}

	PRINTK("rx_buf: head=%d tail=%d wakeup_len=%d\n",
		dev->rx_buf.head,
		dev->rx_buf.tail,
		dev->rx_buf.wakeup_len);

	PRINTK("tx_buf: head=%d tail=%d\n",
		dev->tx_buf.head,
		dev->tx_buf.tail);

	PRINTK("cmdframe_buf: valid=%d serial=%d missed=%d\n",
		dev->cmdframe_buf.is_valid, 
		dev->cmdframe_buf.serial_number,
		dev->cmdframe_buf.missed_count);

	PRINTK("cmdframe: receiving=%d pos=%d error_counter=%d\n",
		dev->cmdframe_buf.receiving,
		dev->cmdframe_buf.pos,
		dev->cmdframe_buf.error_counter);

	PRINTK("serial_config: baudrate=%d\n",
		dev->serial_config.baudrate);

	PRINTK("command_line=0x%02x delta=0x%02x\n",
		dev->sioserver_command_line,
		dev->sioserver_command_line_delta);

	PRINTK("default_baudrate=%d do_autobaud=%d\n",
		dev->default_baudrate,
		dev->do_autobaud);

	PRINTK("tape_baudrate=%d\n",
		dev->tape_baudrate);

	spin_lock_irqsave(&dev->uart_lock, flags);

	PRINTK("UART RBR = 0x%02x\n", serial_in(dev, UART_RX));
	PRINTK("UART IER = 0x%02x\n", serial_in(dev, UART_IER));
	PRINTK("UART IIR = 0x%02x\n", serial_in(dev, UART_IIR));
	PRINTK("UART LCR = 0x%02x\n", serial_in(dev, UART_LCR));
	PRINTK("UART MCR = 0x%02x\n", serial_in(dev, UART_MCR));
	PRINTK("UART LSR = 0x%02x\n", serial_in(dev, UART_LSR));
	PRINTK("UART MSR = 0x%02x\n", serial_in(dev, UART_MSR));

	spin_unlock_irqrestore(&dev->uart_lock, flags);
}

static int atarisio_ioctl(struct inode* inode, struct file* filp,
 	unsigned int cmd, unsigned long arg)
{
	int ret=0;

	struct atarisio_dev* dev = filp->private_data;
	if (!dev) {
		PRINTK_NODEV("cannot get device information!\n");
		return -ENOTTY;
	}

	/*
	 * check for illegal commands
	 */
	if (_IOC_TYPE(cmd) != ATARISIO_IOC_MAGIC) return -ENOTTY;
	if (_IOC_NR(cmd) > ATARISIO_IOC_MAXNR) return -ENOTTY;

	if (cmd != ATARISIO_IOC_GET_TIMESTAMPS) {
		timestamp_entering_ioctl(dev);
	}

        DBG_PRINTK(DEBUG_VERY_NOISY, "ioctl 0x%x , 0x%lx\n", cmd, arg);

	switch (cmd) {
	case ATARISIO_IOC_GET_VERSION:
		ret = ATARISIO_VERSION;
		break;
	case ATARISIO_IOC_SET_MODE:
		switch (arg) {
		case ATARISIO_MODE_1050_2_PC:
			set_1050_2_pc_mode(dev, 1);
			break;
		case ATARISIO_MODE_PROSYSTEM:
			set_prosystem_mode(dev, 1);
			break;
		case ATARISIO_MODE_SIOSERVER:
			set_sioserver_mode(dev, 1);
			break;
		case ATARISIO_MODE_SIOSERVER_DSR:
			set_sioserver_mode_dsr(dev, 1);
			break;
		case ATARISIO_MODE_SIOSERVER_CTS:
			set_sioserver_mode_cts(dev, 1);
			break;
		default:
			ret = -EINVAL;
		}
		break;
	case ATARISIO_IOC_SET_BAUDRATE:
		dev->default_baudrate = arg;
		ret = set_baudrate(dev, dev->default_baudrate, 1);
		break;
	case ATARISIO_IOC_SET_AUTOBAUD:
		dev->do_autobaud = arg;
		if (dev->do_autobaud == 0) {
			ret = set_baudrate(dev, dev->default_baudrate, 1);
		}
		break;
	case ATARISIO_IOC_DO_SIO:
		ret = perform_sio(dev, (SIO_parameters*)arg);
		if (ret == 0) {
			DBG_PRINTK(DEBUG_NOISY, "sio successful\n");
		} else {
			DBG_PRINTK(DEBUG_NOISY, "sio failed : %d\n", ret);
		}
		break;
	case ATARISIO_IOC_GET_COMMAND_FRAME:
		PRINT_TIMESTAMP("start getting command frame");
		ret = get_command_frame(dev, arg);
		PRINT_TIMESTAMP("end getting command frame");
		break;
	case ATARISIO_IOC_SEND_COMMAND_ACK:
	case ATARISIO_IOC_SEND_COMMAND_ACK_XF551:
		if ((ret = check_new_command_frame(dev))) {
			break;
		}
		if ((ret = check_command_frame_time(dev, 1))) {
			break;
		}
		PRINT_TIMESTAMP("start sending command ACK");
		ret = send_single_character(dev, COMMAND_FRAME_ACK_CHAR);
		PRINT_TIMESTAMP("end sending command ACK");
		if (cmd == ATARISIO_IOC_SEND_COMMAND_ACK_XF551) {
			set_baudrate(dev, ATARISIO_XF551_BAUDRATE, 1);
		}
		break;
	case ATARISIO_IOC_SEND_COMMAND_NAK:
		if ((ret = check_new_command_frame(dev))) {
			break;
		}
		if ((ret = check_command_frame_time(dev, 1))) {
			break;
		}
		ret = send_single_character(dev, COMMAND_FRAME_NAK_CHAR);
		break;
	case ATARISIO_IOC_SEND_DATA_ACK:
		udelay(DELAY_T4_MIN);
		ret = send_single_character(dev, COMMAND_FRAME_ACK_CHAR);
		break;
	case ATARISIO_IOC_SEND_DATA_NAK:
		udelay(DELAY_T4_MIN);
		ret = send_single_character(dev, COMMAND_FRAME_NAK_CHAR);
		break;
	case ATARISIO_IOC_SEND_COMPLETE:
	case ATARISIO_IOC_SEND_COMPLETE_XF551:
		if ((ret = check_new_command_frame(dev))) {
			break;
		}
		PRINT_TIMESTAMP("start sending complete");
		if (dev->add_highspeedpause) {
			udelay(DELAY_T5_MIN_SLOW);
		} else {
			udelay(DELAY_T5_MIN);
		}
		ret = send_single_character(dev, OPERATION_COMPLETE_CHAR);
		PRINT_TIMESTAMP("end sending complete");
		if (cmd == ATARISIO_IOC_SEND_COMPLETE_XF551) {
			set_baudrate(dev, ATARISIO_STANDARD_BAUDRATE, 1);
		}
		break;
	case ATARISIO_IOC_SEND_ERROR:
		if ((ret = check_new_command_frame(dev))) {
			break;
		}
		if (dev->add_highspeedpause) {
			udelay(DELAY_T5_MIN_SLOW);
		} else {
			udelay(DELAY_T5_MIN);
		}
		ret = send_single_character(dev, OPERATION_ERROR_CHAR);
		break;
	case ATARISIO_IOC_SEND_DATA_FRAME:
	case ATARISIO_IOC_SEND_DATA_FRAME_XF551:
		if ((ret = check_new_command_frame(dev))) {
			break;
		}
		PRINT_TIMESTAMP("begin send data frame");
		ret = perform_send_data_frame(dev, arg);
		PRINT_TIMESTAMP("end send data frame");
		if (cmd == ATARISIO_IOC_SEND_DATA_FRAME_XF551) {
			set_baudrate(dev, ATARISIO_STANDARD_BAUDRATE, 1);
		}
		break;
	case ATARISIO_IOC_RECEIVE_DATA_FRAME:
		if ((ret = check_new_command_frame(dev))) {
			break;
		}
		ret = perform_receive_data_frame(dev, arg);
		break;
	case ATARISIO_IOC_SEND_RAW_FRAME:
		if ((ret = check_new_command_frame(dev))) {
			break;
		}
		PRINT_TIMESTAMP("begin send raw frame");
		ret = perform_send_raw_frame(dev, arg);
		PRINT_TIMESTAMP("end send raw frame");
		break;
	case ATARISIO_IOC_RECEIVE_RAW_FRAME:
		if ((ret = check_new_command_frame(dev))) {
			break;
		}
		ret = perform_receive_raw_frame(dev, arg);
		break;
	case ATARISIO_IOC_GET_BAUDRATE:
		ret = dev->serial_config.baudrate;
		break;
	case ATARISIO_IOC_SET_HIGHSPEEDPAUSE:
		dev->add_highspeedpause = arg;
		break;
	case ATARISIO_IOC_PRINT_STATUS:
		print_status(dev);
		break;
	case ATARISIO_IOC_ENABLE_TIMESTAMPS:
		dev->enable_timestamp_recording = (int) arg;
		break;
	case ATARISIO_IOC_GET_TIMESTAMPS:
		if (!dev->enable_timestamp_recording) {
			ret = -EINVAL;
		} else {
			if (copy_to_user((SIO_timestamps*)arg, (unsigned char*) &dev->timestamps, sizeof(SIO_timestamps))) {
				ret = -EFAULT;
			}
		}
		break;
	case ATARISIO_IOC_SET_TAPE_BAUDRATE:
		dev->tape_baudrate = arg;
		break;
	case ATARISIO_IOC_SEND_TAPE_BLOCK:
		PRINT_TIMESTAMP("begin send tape block");
		ret = perform_send_tape_block(dev, arg);
		PRINT_TIMESTAMP("end send tape block");
		break;
	default:
		ret = -EINVAL;
	}

	if (cmd != ATARISIO_IOC_GET_TIMESTAMPS) {
		timestamp_leaving_ioctl(dev);
	}

	return ret;
}

static unsigned int atarisio_poll(struct file* filp, poll_table* wait)
{
	unsigned int mask=0;

	struct atarisio_dev* dev = filp->private_data;
	if (!dev) {
		PRINTK_NODEV("cannot get device information!\n");
		return -ENOTTY;
	}

#ifdef ATARISIO_EARLY_NOTIFICATION
	unsigned long flags;
	unsigned long long current_time;
#endif

	poll_wait(filp, &dev->cmdframe_queue, wait);

#ifdef ATARISIO_EARLY_NOTIFICATION
	poll_wait(filp, &dev->early_cmdframe_queue, wait);

	spin_lock_irqsave(&dev->cmdframe_lock, flags);

	current_time = get_timestamp();
	if (dev->cmdframe_buf.receiving) {
		if (current_time > dev->cmdframe_buf.start_reception_time + 10000) {
			dev->cmdframe_buf.receiving = 0;
			DBG_PRINTK(DEBUG_STANDARD, "aborted reception of command frame - maximum time exceeded");
		} else {
			mask |= POLLPRI;
		}
	}
	spin_unlock_irqrestore(&dev->cmdframe_lock, flags);
#endif

	if (dev->cmdframe_buf.is_valid) {
		mask |= POLLPRI;
	}

	return mask;
}

/* old IRQ flags deprecated since linux 2.6.22 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
#define FAST_IRQFLAGS (IRQF_DISABLED | IRQF_SHARED)
#define SLOW_IRQFLAGS (IRQF_SHARED)
#else
#define FAST_IRQFLAGS (SA_INTERRUPT | SA_SHIRQ)
#define SLOW_IRQFLAGS (SA_SHIRQ)
#endif



static int atarisio_open(struct inode* inode, struct file* filp)
{
	unsigned long flags;
	struct atarisio_dev* dev = 0;
	int ret = 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
       	MOD_INC_USE_COUNT;
#endif
	if (MINOR(inode->i_rdev) < minor) {
		ret = -ENOTTY;
		goto exit_open;
	}
	if (MINOR(inode->i_rdev) >= minor + ATARISIO_MAXDEV) {
		ret = -ENOTTY;
		goto exit_open;
	}

	dev = atarisio_devices[MINOR(inode->i_rdev) - minor];

	if (!dev) {
		PRINTK_NODEV("cannot get device information!\n");
		ret = -ENOTTY;
		goto exit_open;
	}

	if (dev->busy) {
		ret = -EBUSY;
		goto exit_open;
	}
	dev->busy=1;
	filp->private_data = dev;

	dev->current_mode = MODE_1050_2_PC;

	reset_rx_buf(dev);
	reset_tx_buf(dev);
	reset_cmdframe_buf(dev, 1);

	dev->do_autobaud = 0;
	dev->default_baudrate = ATARISIO_STANDARD_BAUDRATE;
	dev->tape_baudrate = ATARISIO_TAPE_BAUDRATE;
	dev->add_highspeedpause = 0;

	spin_lock_irqsave(&dev->uart_lock, flags);
	spin_lock(&dev->serial_config_lock);

	dev->serial_config.baud_base = 115200;
	dev->serial_config.baudrate = dev->default_baudrate;
	dev->serial_config.IER = UART_IER_RDI;
	dev->serial_config.MCR = UART_MCR_OUT2;
			/* OUT2 is essential for enabling interrupts - when it's required :-) */
	dev->serial_config.LCR = dev->standard_lcr; /* 1 stopbit, no parity */

	dev->serial_config.FCR = UART_FCR_ENABLE_FIFO | UART_FCR_TRIGGER_1;

	serial_out(dev, UART_IER, 0);

	serial_out(dev, UART_LCR, dev->serial_config.LCR);

	set_baudrate(dev, dev->serial_config.baudrate, 0);

	set_1050_2_pc_mode(dev, 0); /* this call sets MCR */

	serial_out(dev, UART_FCR, 0);
	serial_out(dev, UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
	serial_out(dev, UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR_TRIGGER_1);

	/*
	 * clear any interrupts
	 */
        (void)serial_in(dev, UART_LSR);
	(void)serial_in(dev, UART_IIR);
	(void)serial_in(dev, UART_RX);
	(void)serial_in(dev, UART_MSR);

	if (request_irq(dev->irq, atarisio_interrupt, FAST_IRQFLAGS, dev->devname, dev)) {
		if (request_irq(dev->irq, atarisio_interrupt, SLOW_IRQFLAGS, dev->devname, dev)) {
			PRINTK("could not register interrupt %d\n",dev->irq);
			dev->busy=0;
			spin_unlock_irqrestore(&dev->uart_lock, flags);
			ret = -EFAULT;
			goto exit_open;
		} else {
			DBG_PRINTK(DEBUG_STANDARD, "got slow interrupt\n");
		}
	} else {
		DBG_PRINTK(DEBUG_STANDARD, "got fast interrupt\n");
	}

	serial_out(dev, UART_IER, dev->serial_config.IER);

	spin_unlock(&dev->serial_config_lock);
	spin_unlock_irqrestore(&dev->uart_lock, flags);

exit_open:

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
	if (ret != 0) {
        	MOD_DEC_USE_COUNT;
	}
#endif
	return ret;
}

static int atarisio_release(struct inode* inode, struct file* filp)
{
	unsigned long flags;

	struct atarisio_dev* dev = filp->private_data;
	if (!dev) {
		PRINTK_NODEV("cannot get device information!\n");
		return -ENOTTY;
	}

	spin_lock_irqsave(&dev->uart_lock, flags);

	serial_out(dev, UART_MCR,0);
	serial_out(dev, UART_IER,0);

	spin_unlock_irqrestore(&dev->uart_lock, flags);

	free_irq(dev->irq, dev);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
        MOD_DEC_USE_COUNT;
#endif

	dev->busy=0;
	return 0;
}

static struct file_operations atarisio_fops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,0)
	owner:		THIS_MODULE,
#endif
	poll:		atarisio_poll,
	ioctl:		atarisio_ioctl,
	open:		atarisio_open,
	release:	atarisio_release
};

struct atarisio_dev* alloc_atarisio_dev(unsigned int id)
{
	struct atarisio_dev* dev;
	if (id >= ATARISIO_MAXDEV) {
		PRINTK_NODEV("alloc_atarisio_dev called with invalid id %d\n", id);
		return 0;
	}
	if (atarisio_devices[id]) {
		PRINTK_NODEV("device %d already allocated\n", id);
		return 0;
	}

	dev = kmalloc(sizeof(struct atarisio_dev), GFP_KERNEL);
	if (!dev) {
		goto alloc_fail;
	}
	memset(dev, 0, sizeof(struct atarisio_dev));

	dev->devname = kmalloc(20, GFP_KERNEL);
	if (!dev->devname) {
		kfree(dev);
		goto alloc_fail;
	}
	sprintf(dev->devname,"%s%d",NAME, id);

	dev->miscdev = kmalloc(sizeof(struct miscdevice), GFP_KERNEL);
	if (!dev->miscdev) {
		kfree(dev->devname);
		kfree(dev);
		goto alloc_fail;
	}
	memset(dev->miscdev, 0, sizeof(struct miscdevice));

	atarisio_devices[id] = dev;
	dev->busy = 0;
	dev->id = id;
	dev->uart_lock = SPIN_LOCK_UNLOCKED;
	dev->current_mode = MODE_1050_2_PC;
	dev->last_msr = 0;
	dev->enable_timestamp_recording = 0;
	dev->rx_buf.head = 0;
	dev->rx_buf.tail = 0;
	dev->rx_lock = SPIN_LOCK_UNLOCKED;
	dev->tx_lock = SPIN_LOCK_UNLOCKED;
	dev->cmdframe_lock = SPIN_LOCK_UNLOCKED;
	dev->serial_config_lock = SPIN_LOCK_UNLOCKED;
	dev->sioserver_command_line = UART_MSR_RI;
	dev->sioserver_command_line_delta = UART_MSR_TERI;
	dev->command_line_mask =  ~UART_MCR_RTS;
	dev->command_line_low = UART_MCR_RTS;
	dev->command_line_high = 0;
	dev->default_baudrate = ATARISIO_STANDARD_BAUDRATE;
	dev->tape_baudrate = ATARISIO_TAPE_BAUDRATE;
	dev->do_autobaud = 0;
	dev->add_highspeedpause = 0;
	dev->standard_lcr = UART_LCR_WLEN8;
	dev->slow_lcr = UART_LCR_WLEN8 | UART_LCR_STOP | UART_LCR_PARITY | UART_LCR_SPAR;
	dev->need_reenable = 0;

	init_waitqueue_head(&dev->rx_queue);
	init_waitqueue_head(&dev->tx_queue);
	init_waitqueue_head(&dev->cmdframe_queue);

	dev->miscdev->name = dev->devname;
	dev->miscdev->fops = &atarisio_fops;
	dev->miscdev->minor = minor + dev->id;

#ifdef ATARISIO_EARLY_NOTIFICATION
	init_waitqueue_head(&dev->early_cmdframe_queue);
#endif

	return dev;

alloc_fail:
	PRINTK_NODEV("cannot allocate atarisio_dev structure %d\n", id);
	return 0;
}

int free_atarisio_dev(unsigned int id)
{
	struct atarisio_dev* dev;
	if (id >= ATARISIO_MAXDEV) {
		PRINTK_NODEV("free_atarisio_dev called with invalid id %d\n", id);
		return 0;
	}
	dev = atarisio_devices[id];
	if (!dev) {
		PRINTK_NODEV("attempt to free non-allocated atarisio_dev %d\n", id);
		return -EINVAL;
	}
	kfree(dev->miscdev);
	dev->miscdev = 0;

	kfree(dev->devname);
	dev->devname = 0;
	kfree(dev);
	atarisio_devices[id] = 0;
	return 0;
}

static int disable_serial_port(struct atarisio_dev* dev)
{
	mm_segment_t fs;
	struct file* f;
	struct serial_struct ss;

	lock_kernel();

	fs = get_fs();
	set_fs(get_ds());

	f = filp_open(dev->port,O_RDWR | O_NONBLOCK,0);
	if (IS_ERR(f)) {
		DBG_PRINTK(DEBUG_STANDARD, "error opening serial port %s\n", dev->port);
		goto fail;
	}
	if (f->f_op && f->f_op->ioctl) {
		if (f->f_dentry && f->f_dentry->d_inode) {
			if (f->f_op->ioctl(f->f_dentry->d_inode, f, TIOCGSERIAL, (unsigned long) &ss)) {
				DBG_PRINTK(DEBUG_STANDARD, "TIOCGSERIAL failed\n");
				goto fail_close;
			}
			if (f->f_op->ioctl(f->f_dentry->d_inode, f, TIOCGSERIAL, (unsigned long) &dev->orig_serial_struct)) {
				DBG_PRINTK(DEBUG_STANDARD, "TIOCGSERIAL failed\n");
				goto fail_close;
			}
			switch (ss.type) {
			case PORT_16550:
			case PORT_16550A:
				break;
			default:
				PRINTK("illegal port type - only 16550(A) is supported\n");
				goto fail_close;
			}

			DBG_PRINTK(DEBUG_STANDARD, "ss.port = 0x%04x ss.irq = %d ss.type = %d\n",
				ss.port, ss.irq, ss.type);

			if (dev->io == 0) {
				dev->io = ss.port;
			}
			if (dev->irq == 0) {
				dev->irq = ss.irq;
			}

			// disable serial driver by setting the uart type to none
			ss.type = PORT_UNKNOWN;

			if (f->f_op->ioctl(f->f_dentry->d_inode, f, TIOCSSERIAL, (unsigned long) &ss)) {
				DBG_PRINTK(DEBUG_STANDARD, "TIOCSSERIAL failed\n");
				goto fail_close;
			}
			dev->need_reenable = 1;
		} else {
			DBG_PRINTK(DEBUG_STANDARD, "unable to get inode of %s\n", dev->port);
			goto fail_close;
		}
	} else {
		DBG_PRINTK(DEBUG_STANDARD, "device doesn't provide ioctl function!\n");
		goto fail_close;
	}

	if (filp_close(f,NULL)) {
		DBG_PRINTK(DEBUG_STANDARD, "error closing serial port %s\n", dev->port);
		goto fail;
	}
	set_fs(fs);
	unlock_kernel();
	return 0;

fail_close:
	if (filp_close(f,NULL)) {
		DBG_PRINTK(DEBUG_STANDARD, "error closing serial port %s\n", dev->port);
		goto fail;
	}

fail:
	set_fs(fs);
	unlock_kernel();
	return 1;
}

static int reenable_serial_port(struct atarisio_dev* dev)
{
	mm_segment_t fs;
	struct file* f;

	if (!dev->need_reenable) {
		DBG_PRINTK(DEBUG_STANDARD, "unnecessary call to reenable_serial_port\n");
		return 0;
	}

	lock_kernel();
	fs = get_fs();
	set_fs(get_ds());

	f = filp_open(dev->port,O_RDWR,0);
	if (IS_ERR(f)) {
		DBG_PRINTK(DEBUG_STANDARD, "error opening serial port %s\n", dev->port);
		goto fail;
	}
	if (f->f_op && f->f_op->ioctl) {
		if (f->f_dentry && f->f_dentry->d_inode) {
			if (f->f_op->ioctl(f->f_dentry->d_inode, f, TIOCSSERIAL, (unsigned long) &dev->orig_serial_struct)) {
				DBG_PRINTK(DEBUG_STANDARD, "TIOCSSERIAL failed\n");
				goto fail_close;
			}
		} else {
			DBG_PRINTK(DEBUG_STANDARD, "unable to get inode of %s\n", dev->port);
			goto fail_close;
		}
	} else {
		DBG_PRINTK(DEBUG_STANDARD, "device doesn't provide ioctl function!\n");
		goto fail_close;
	}

	if (filp_close(f,NULL)) {
		DBG_PRINTK(DEBUG_STANDARD, "error closing serial port %s\n", dev->port);
		goto fail;
	}
	set_fs(fs);
	unlock_kernel();
	return 0;

fail_close:
	if (filp_close(f,NULL)) {
		DBG_PRINTK(DEBUG_STANDARD, "error closing serial port %s\n", dev->port);
		goto fail;
	}

fail:
	set_fs(fs);
	unlock_kernel();
	return 1;
}

static int check_register_atarisio(struct atarisio_dev* dev)
{
	int ret = 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
	if (check_region(dev->io,8)) {
		PRINTK("cannot access IO-ports 0x%04x-0x%04x\n",dev->io,dev->io+7);
		ret = -EINVAL;
		goto failure;
	}
	request_region(dev->io, 8, NAME);
#else
	if (request_region(dev->io, 8, NAME) == 0) {
		PRINTK("cannot access IO-ports 0x%04x-0x%04x\n",dev->io,dev->io+7);
		ret = -EINVAL;
		goto failure;
	}
#endif
	
	/*
	 * check for 16550:
	 * this is just a very simple test using the scratch register
	 */
	serial_out(dev, UART_SCR,0xaa);
	if (serial_in(dev, UART_SCR) != 0xaa) {
		PRINTK("couldn't detect 16550\n");
		ret = -ENODEV;
		goto failure_release;
	}

	serial_out(dev, UART_SCR,0x55);
	if (serial_in(dev, UART_SCR) != 0x55) {
		PRINTK("couldn't detect 16550\n");
		ret = -ENODEV;
		goto failure_release;
	}

	/*
	 * checks are OK so far, try to register the device
	 */
	if (misc_register(dev->miscdev))
	{
		PRINTK("failed to register device %s (minor %d)\n", dev->miscdev->name, dev->miscdev->minor);
		ret = -ENODEV;
		goto failure_release;
	}

	if (dev->port) {
		PRINTK("minor=%d port=%s io=0x%04x irq=%d\n", dev->miscdev->minor, dev->port, dev->io, dev->irq);
	} else {
		PRINTK("minor=%d io=0x%04x irq=%d\n", dev->miscdev->minor, dev->io, dev->irq);
	}
	return 0;

failure_release:
	release_region(dev->io, 8);

failure:
	if (dev->need_reenable) {
		if (reenable_serial_port(dev)) {
			PRINTK("error re-enabling serial port!\n");
		} else {
			DBG_PRINTK(DEBUG_STANDARD, "successfully re-enabled serial port %s\n", dev->port);
		}
	}
	return ret;
}

static int atarisio_is_initialized = 0;

static void atarisio_cleanup_module(void)
{
	int i;
	struct atarisio_dev* dev;

	for (i=0;i<ATARISIO_MAXDEV;i++) {
		if ((dev=atarisio_devices[i])) {
			if (misc_deregister(dev->miscdev)) {
				PRINTK("cannot unregister device\n");
			}

			release_region(dev->io,8);
			if (dev->need_reenable) {
				if (reenable_serial_port(dev)) {
					PRINTK("error re-enabling serial port!\n");
				} else {
					DBG_PRINTK(DEBUG_STANDARD, "successfully re-enabled serial port %s\n", dev->port);
				}
			}
			free_atarisio_dev(i);
		}
	}
	PRINTK_NODEV("module terminated\n");
	atarisio_is_initialized = 0;
}  

static int atarisio_init_module(void)
{
	int i;
	int numdev = 0;
	int numtried = 0;
	atarisio_is_initialized = 0;

	printk("AtariSIO kernel driver V%d.%02d (c) 2007 Matthias Reichl\n",
		ATARISIO_MAJOR_VERSION, ATARISIO_MINOR_VERSION);

	for (i=0;i<ATARISIO_MAXDEV;i++) {
		if ((port[i] && port[i][0]) || io[i] || irq[i]) {
			struct atarisio_dev* dev = alloc_atarisio_dev(i);
			numtried++;
			if (!dev) {
				atarisio_cleanup_module();
				return -ENOMEM;
			}

			dev->port = 0;
			dev->io = io[i];
			dev->irq = irq[i];
			if (port[i] && port[i][0]) {
				dev->port = port[i];
				if (disable_serial_port(dev)) {
					PRINTK("unable to disable serial port %s\n", dev->port);
					free_atarisio_dev(i);
					continue;
				} else {
					DBG_PRINTK(DEBUG_STANDARD, "successfully disabled serial port %s\n", dev->port);
				}
			}

			if (check_register_atarisio(dev)) {
				free_atarisio_dev(i);
				continue;
			}
			numdev++;
		}
	}
	if (!numtried) {
		PRINTK_NODEV("please supply port or io and irq parameters\n");
		return -EINVAL;
	}
	if (!numdev) {
		PRINTK_NODEV("failed to register any devices\n");
		return -ENODEV;
	}
	atarisio_is_initialized = 1;
	return 0;
}

static void atarisio_exit_module(void)
{
	if (!atarisio_is_initialized) {
		PRINTK_NODEV("internal error - atarisio is not initialized!\n");
		return;
	}

	atarisio_cleanup_module();
}

module_init(atarisio_init_module);
module_exit(atarisio_exit_module);
