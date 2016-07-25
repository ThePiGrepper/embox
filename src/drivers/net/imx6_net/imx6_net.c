/**
 * @file imx6_net.c
 * @brief iMX6 MAC-NET driver
 * @author Denis Deryugin <deryugin.denis@gmail.com>
 * @version 0.1
 * @date 2016-05-11
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <hal/reg.h>

#include <kernel/irq.h>
#include <kernel/printk.h>

#include <net/inetdevice.h>
#include <net/l0/net_entry.h>
#include <net/l2/ethernet.h>
#include <net/netdevice.h>
#include <net/skbuff.h>

#include <util/log.h>

#include <embox/unit.h>

#include <framework/mod/options.h>

#include "imx6_net.h"

struct fec_priv {
	uint32_t base_addr;
	struct imx6_buf_desc *rbd_base;
	int rbd_index;
	struct imx6_buf_desc *tbd_base;
	int tbd_index;

	int _cur_rx;
	int _dirty_tx;
};

static struct fec_priv fec_priv;

#define DEBUG 1
#if DEBUG
#include <kernel/printk.h>
/* Debugging routines */
static inline void show_packet(uint8_t *raw, int size, char *title) {
	int i;

	printk("\nPACKET(%d) %s:", size, title);
	for (i = 0; i < size; i++) {
		if (!(i % 16)) {
			printk("\n");
		}
		printk(" %02hhX", *(raw + i));
	}
	printk("\n.\n");
}
#else
#define show_packet(raw, size,title)
#endif

static void _reg_dump(void) {
	log_debug("ENET_EIR  %10x", REG32_LOAD(ENET_EIR ));
	log_debug("ENET_EIMR %10x", REG32_LOAD(ENET_EIMR));
	log_debug("ENET_RDAR %10x", REG32_LOAD(ENET_RDAR));
	log_debug("ENET_TDAR %10x", REG32_LOAD(ENET_TDAR));
	log_debug("ENET_ECR  %10x", REG32_LOAD(ENET_ECR ));
	log_debug("ENET_MSCR %10x", REG32_LOAD(ENET_MSCR));
	log_debug("ENET_RCR  %10x", REG32_LOAD(ENET_RCR ));
	log_debug("ENET_TCR  %10x", REG32_LOAD(ENET_TCR ));
	log_debug("MAC_LOW   %10x", REG32_LOAD(MAC_LOW  ));
	log_debug("MAC_HI    %10x", REG32_LOAD(MAC_HI   ));
	log_debug("ENET_IAUR %10x", REG32_LOAD(ENET_IAUR));
	log_debug("ENET_IALR %10x", REG32_LOAD(ENET_IALR));
	log_debug("ENET_GAUR %10x", REG32_LOAD(ENET_GAUR));
	log_debug("ENET_GALR %10x", REG32_LOAD(ENET_GALR));
	log_debug("ENET_TFWR %10x", REG32_LOAD(ENET_TFWR));
	log_debug("ENET_RDSR %10x", REG32_LOAD(ENET_RDSR));
	log_debug("ENET_TDSR %10x", REG32_LOAD(ENET_TDSR));
	log_debug("ENET_MRBR %10x", REG32_LOAD(ENET_MRBR));
}

static void emac_set_macaddr(unsigned char _macaddr[6]) {
	uint32_t mac_hi, mac_lo;
	const uint8_t *tmp = _macaddr;

	REG32_STORE(ENET_IAUR, 0);
	REG32_STORE(ENET_IALR, 0);
	REG32_STORE(ENET_GAUR, 0);
	REG32_STORE(ENET_GALR, 0);

	log_debug("addr = %x:%x:%x:%x:%x:%x", tmp[0],tmp[1],tmp[2],tmp[3],tmp[4],tmp[5]);

	mac_hi  = (_macaddr[5] << 16) |
	          (_macaddr[4] << 24);
	mac_lo  = (_macaddr[3] <<  0) |
	          (_macaddr[2] <<  8) |
	          (_macaddr[1] << 16) |
	          (_macaddr[0] << 24);

	REG32_STORE(MAC_LOW, mac_lo);
	REG32_STORE(MAC_HI, mac_hi | 0x8808);
}

static struct imx6_buf_desc _tx_desc_ring[TX_BUF_FRAMES] __attribute__ ((aligned(0x10)));
static struct imx6_buf_desc _rx_desc_ring[RX_BUF_FRAMES] __attribute__ ((aligned(0x10)));

static int _cur_rx = 0;
static int _dirty_tx = 0;

static uint8_t _tx_buf[TX_BUF_FRAMES][2048] __attribute__ ((aligned(0x10)));
static uint8_t _rx_buf[RX_BUF_FRAMES][2048] __attribute__ ((aligned(0x10)));

extern void dcache_inval(const void *p, size_t size);
extern void dcache_flush(const void *p, size_t size);

static void fec_tbd_init(struct fec_priv *fec)
{
	unsigned size = TX_BUF_FRAMES * sizeof(struct imx6_buf_desc);

	memset(fec->tbd_base, 0, size);

	fec->tbd_base[TX_BUF_FRAMES - 1].flags1 = FLAG_W;

	dcache_flush(fec->tbd_base, size);

	fec->tbd_index = 0;
	REG32_STORE(ENET_TDSR, ((uint32_t) fec->tbd_base));
}

static void fec_rbd_init(struct fec_priv *fec, int count, int dsize) {
	uint32_t size;
	uint8_t *data;
	int i;

	/*
	 * Reload the RX descriptors with default values and wipe
	 * the RX buffers.
	 */
	size = count * sizeof(struct imx6_buf_desc);
	for (i = 0; i < count; i++) {
		data = (uint8_t *)fec->rbd_base[i].data_pointer;
		memset(data, 0, dsize);
		dcache_flush(data, dsize);

		fec->rbd_base[i].flags1 = FLAG_R;
		fec->rbd_base[i].len = 0;
	}

	/* Mark the last RBD to close the ring. */
	fec->rbd_base[count - 1].flags1 = FLAG_W | FLAG_R;
	fec->rbd_index = 0;

	dcache_flush((void *)fec->rbd_base, size);

	REG32_STORE(ENET_RDSR, (uint32_t)fec->rbd_base);
}

static int imx6_net_xmit(struct net_device *dev, struct sk_buff *skb) {
	uint8_t *data;
	struct imx6_buf_desc *desc;
	int res;
	ipl_t sp;
	struct fec_priv *priv;
	int cur_tx_desc;

	assert(dev);
	assert(skb);


	res = 0;

	data = (uint8_t*) skb_data_cast_in(skb->data);

	if (!data) {
		log_error("No skb data!\n");
		res = -1;
		goto out;

	}
	show_packet(data, skb->len, "tx");

	priv = dev->priv;
	assert((uint32_t)_tx_desc_ring == REG32_LOAD(ENET_TDSR));

	sp = ipl_save();
	{
		cur_tx_desc = priv->tbd_index;
		log_debug("Transmitting packet %2d", cur_tx_desc);
		if (skb->len < 0x40) {
			skb->len = 0x40;
		}

		memcpy(&_tx_buf[cur_tx_desc][0], data, skb->len);
		dcache_flush(&_tx_buf[cur_tx_desc][0], skb->len);
		//dcache_flush(data, skb->len);

		desc = &_tx_desc_ring[cur_tx_desc];
		dcache_inval(desc, sizeof(struct imx6_buf_desc));
		if (desc->flags1 & FLAG_R) {
			log_error("tx desc still busy");
			goto out1;

		}
		desc->data_pointer = (uint32_t)&_tx_buf[cur_tx_desc][0];
		//desc->data_pointer = (uint32_t)data;
		desc->len          = skb->len;
		desc->flags1       |= FLAG_L | FLAG_TC | FLAG_R;
		dcache_flush(desc, sizeof(struct imx6_buf_desc));
		log_debug("desc = %x", desc);
		log_debug("->data_pointer = %x", desc->data_pointer);
		log_debug("->len = %x", desc->len);
		log_debug("->flags1 = %x", desc->flags1);

		REG32_LOAD(desc + sizeof(*desc) - 4);

		REG32_STORE(ENET_TDAR, 1 << 24);
		__asm__("nop");

		int timeout = 0xFF;
		while(--timeout) {
			if (!(REG32_LOAD(ENET_TDAR))) {
				break;
			}
			log_debug("ENET_TDAR not zero");
		}

		log_debug("TX timeout %d", timeout);
		if (timeout == 0) {
			log_debug("TX timeout ENET_TDAR is not zero...");
		}

		timeout = 0xFFFFFF;
		while(--timeout) {
			dcache_inval(desc, sizeof(struct imx6_buf_desc));
			if (!(desc->flags1 & FLAG_R)) {
				log_debug("************\n->data_pointer = %x", desc->data_pointer);
				log_debug("->len = %x", desc->len);
				log_debug("->flags1 = %x", desc->flags1);
				break;
			}
		}
		if (timeout == 0) {
			log_debug("TX timeout bit READY still set...");
		}
		log_debug("TX timeout %d", timeout);

		priv->tbd_index = (priv->tbd_index + 1) % TX_BUF_FRAMES;
	}
out1:
	ipl_restore(sp);

out:
	skb_free(skb);

	return res;
}

static void _init_buffers(void) {
	int i;

	for (i = 0; i < RX_BUF_FRAMES; i++) {
		_rx_desc_ring[i].data_pointer = (uint32_t)&_rx_buf[i][0];
	}
#if 0
	for (i = 0; i < TX_BUF_FRAMES; i++) {
		_tx_desc_ring[i].data_pointer = (uint32_t)& _tx_buf[i][0];
	}
#endif
}

static void _reset(struct net_device *dev) {
	int cnt = 0;

	log_debug("ENET dump uboot...\n");
	_reg_dump();

	REG32_STORE(ENET_ECR, RESET);
	while(REG32_LOAD(ENET_ECR) & RESET){
		if (cnt ++ > 100000) {
			log_error("enet can't be reset");
			break;
		}
	}
	REG32_STORE(ENET_ECR, 0xF0000100);

	_init_buffers();
	fec_rbd_init(dev->priv, RX_BUF_FRAMES, 0x600);

	fec_tbd_init(dev->priv);


	/*
	 * Set interrupt mask register
	 */
	/* enable all interrupts */
	/* Transmit Frame Interrupt
	 * Transmit Buffer Interrupt
	 * Receive Frame Interrupt
	 * Receive Buffer Interrupt
	 */
	REG32_STORE(ENET_EIMR, 0x00000000);
	/*
	 * Clear FEC-Lite interrupt event register(IEVENT)
	 */
	REG32_STORE(ENET_EIR, 0xffc00000);

	/* set mii speed */
	REG32_STORE(ENET_MSCR, 0x1a);

	/* Full-Duplex Enable */
	 REG32_STORE(ENET_TCR, (1 << 2));

	 /* Transmit FIFO Write 64 bytes */
	REG32_STORE(ENET_TFWR, 0x100);

	//uint32_t t = 0x08000124;
	/* MAX_FL frame length*/
	/* Enables 10-Mbit/s mode of the RMII or RGMII ?*/
	/* MII or RMII mode, as indicated by the RMII_MODE field. */
	REG32_STORE(ENET_RCR, 0x5ee0104);

	/* Maximum Receive Buffer Size Register
	 * Receive buffer size in bytes. This value, concatenated with the four
	 * least-significant bits of this register (which are always zero),
	 * is the effective maximum receive buffer size.
	 */
	REG32_STORE(ENET_MRBR, 0x5f0);

	REG32_STORE(MAC_LOW, 0x001213dd);
	REG32_STORE(MAC_HI, 0x54a38808);

	REG32_STORE(ENET_ECR, REG32_LOAD(ENET_ECR) | ETHEREN); /* Note: should be last ENET-related init step */

	REG32_STORE(ENET_RDAR, (1 << 24));
	log_debug("ENET dump embox...\n");
	_reg_dump();
}

static int imx6_net_open(struct net_device *dev) {

	_reset(dev);

	return 0;
}

static int imx6_net_set_macaddr(struct net_device *dev, const void *addr) {
	assert(dev);
	assert(addr);

	emac_set_macaddr((unsigned char *)addr);

	return 0;
}

static irq_return_t imx6_irq_handler(unsigned int irq_num, void *dev_id) {
	uint32_t state;
	struct imx6_buf_desc *desc;

	state = REG32_LOAD(ENET_EIR);

	log_debug("Interrupt mask %#010x", state);

	REG32_STORE(ENET_EIR, state);

	REG32_STORE(ENET_RDAR, 1 << 24);

	if (state == 0x10000000)
		return IRQ_HANDLED;

	if (state & EIR_EBERR) {
		log_error("Ethernet bus error, resetting ENET!");
		//REG32_STORE(ENET_ECR, RESET);
		_reset(dev_id);

		return IRQ_HANDLED;
	}

	if (state & (EIR_RXB | EIR_RXF)) {
		log_debug("RX interrupt");
		desc = &_rx_desc_ring[_cur_rx];
		dcache_inval(desc, sizeof(struct imx6_buf_desc));
		dcache_inval((void *)desc->data_pointer, 2048);

		if (desc->flags1 & FLAG_E) {
			log_error("Current RX descriptor is empty!");
		} else {
			struct sk_buff *skb = skb_alloc(desc->len);
			assert(skb);
			skb->len = desc->len;
			skb->dev = dev_id;
			memcpy(skb_data_cast_in(skb->data),
				(void*)desc->data_pointer, desc->len);
			netif_rx(skb);

			desc->flags1 = FLAG_E;
			if (_cur_rx == RX_BUF_FRAMES - 1)
				desc->flags1 |= FLAG_W;
			dcache_flush(desc, sizeof(struct imx6_buf_desc));
			_cur_rx = (_cur_rx + 1) % RX_BUF_FRAMES;
		}
	}

	if (state & (EIR_TXB | EIR_TXF)) {
		log_debug("finished TX");
		desc = &_tx_desc_ring[_dirty_tx];
		dcache_inval(desc, sizeof(struct imx6_buf_desc));
		if (desc->flags1 & FLAG_R)
			log_error("No single frame transmitted!");

		while (!(desc->flags1 & FLAG_R)) {
			assert(desc->data_pointer == (uint32_t) &_tx_buf[_dirty_tx]);
			log_debug("Frame %2d transmitted", _dirty_tx);
			_dirty_tx = (_dirty_tx + 1) % TX_BUF_FRAMES;
			desc = &_tx_desc_ring[_dirty_tx];
			dcache_inval(desc, sizeof(struct imx6_buf_desc));
		}
	}

	_reg_dump();

	return IRQ_HANDLED;
}

static const struct net_driver imx6_net_drv_ops = {
	.xmit = imx6_net_xmit,
	.start = imx6_net_open,
	.set_macaddr = imx6_net_set_macaddr
};

EMBOX_UNIT_INIT(imx6_net_init);
static int imx6_net_init(void) {
	struct net_device *nic;
	int tmp;

	if (NULL == (nic = etherdev_alloc(0))) {
		return -ENOMEM;
	}

	nic->drv_ops = &imx6_net_drv_ops;
	fec_priv.base_addr = NIC_BASE;
	fec_priv.rbd_base =  _rx_desc_ring;
	fec_priv.tbd_base =  _tx_desc_ring;
	nic->priv = &fec_priv;


	tmp = irq_attach(ENET_IRQ, imx6_irq_handler, 0, nic, "i.MX6 enet");
	if (tmp)
		return tmp;

	return inetdev_register_dev(nic);
}
