/*
 *
 *  Bluetooth HCI UART driver
 *
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2004-2005  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/poll.h>

#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/signal.h>
#include <linux/ioctl.h>
#include <linux/skbuff.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "hci_uart.h"

#define VERSION "2.2"

static int reset = 0;

static struct hci_uart_proto *hup[HCI_UART_MAX_PROTO];

int hci_uart_register_proto(struct hci_uart_proto *p)
{
	if (p->id >= HCI_UART_MAX_PROTO)
		return -EINVAL;

	if (hup[p->id])
		return -EEXIST;

	hup[p->id] = p;

	return 0;
}

int hci_uart_unregister_proto(struct hci_uart_proto *p)
{
	if (p->id >= HCI_UART_MAX_PROTO)
		return -EINVAL;

	if (!hup[p->id])
		return -EINVAL;

	hup[p->id] = NULL;

	return 0;
}

static struct hci_uart_proto *hci_uart_get_proto(unsigned int id)
{
	if (id >= HCI_UART_MAX_PROTO)
		return NULL;

	return hup[id];
}

static inline void hci_uart_tx_complete(struct hci_uart *hu, int pkt_type)
{
	struct hci_dev *hdev = hu->hdev;

#if defined(CONFIG_BT_CSR8811)
	if(hdev == NULL)
		return ;
#endif


	/* Update HCI stat counters */
	switch (pkt_type) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;

	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;

	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	}
}

static inline struct sk_buff *hci_uart_dequeue(struct hci_uart *hu)
{
	struct sk_buff *skb = hu->tx_skb;

	if (!skb)
		skb = hu->proto->dequeue(hu);
	else
		hu->tx_skb = NULL;

	return skb;
}

static void hci_uart_tx_deferred_wakeup(unsigned long arg)
{
	struct hci_uart *hu = (struct hci_uart *)arg;
	struct tty_struct *tty = hu->tty;
	struct hci_dev *hdev = hu->hdev;
	struct sk_buff *skb;

#if defined(CONFIG_BT_CSR8811)
	if(hdev == NULL)
		return -1;
#endif

restart:
	clear_bit(HCI_UART_TX_WAKEUP, &hu->tx_state);

	while ((skb = hci_uart_dequeue(hu))) {
		int len;
/* Samsung Bluetooth Feature.2012.01.19
 * Add wake_peer uart operation which is called before starting UART TX
 */
#if !defined(CONFIG_BT_CSR8811)
		if (hdev->wake_peer)
			hdev->wake_peer(hdev);
#endif

		set_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
		len = tty->ops->write(tty, skb->data, skb->len);
		hdev->stat.byte_tx += len;

		skb_pull(skb, len);
		if (skb->len) {
			hu->tx_skb = skb;
			break;
		}

		hci_uart_tx_complete(hu, bt_cb(skb)->pkt_type);
		kfree_skb(skb);
	}

	if (test_bit(HCI_UART_TX_WAKEUP, &hu->tx_state))
		goto restart;

	clear_bit(HCI_UART_SENDING, &hu->tx_state);
}

int hci_uart_tx_wakeup(struct hci_uart *hu)
{
	if (test_and_set_bit(HCI_UART_SENDING, &hu->tx_state)) {
		set_bit(HCI_UART_TX_WAKEUP, &hu->tx_state);
		return 0;
	}
	tasklet_schedule(&hu->tx_tasklet);
	return 0;
}

/* ------- Interface to HCI layer ------ */
/* Initialize device */
static int hci_uart_open(struct hci_dev *hdev)
{
	BT_DBG("%s %p", hdev->name, hdev);

	/* Nothing to do for UART driver */

	set_bit(HCI_RUNNING, &hdev->flags);

	return 0;
}

/* Reset device */
static int hci_uart_flush(struct hci_dev *hdev)
{
	struct hci_uart *hu  = (struct hci_uart *) hdev->driver_data;
	struct tty_struct *tty = hu->tty;

	BT_DBG("hdev %p tty %p", hdev, tty);

	if (hu->tx_skb) {
		kfree_skb(hu->tx_skb); hu->tx_skb = NULL;
	}

	/* Flush any pending characters in the driver and discipline. */
	tty_ldisc_flush(tty);
	tty_driver_flush_buffer(tty);

	if (test_bit(HCI_UART_PROTO_SET, &hu->flags))
		hu->proto->flush(hu);

	return 0;
}

/* Close device */
static int hci_uart_close(struct hci_dev *hdev)
{
	BT_DBG("hdev %p", hdev);

	if (!test_and_clear_bit(HCI_RUNNING, &hdev->flags))
		return 0;

	hci_uart_flush(hdev);
	hdev->flush = NULL;
	return 0;
}

/* Send frames from HCI layer */
static int hci_uart_send_frame(struct sk_buff *skb)
{
	struct hci_dev* hdev = (struct hci_dev *) skb->dev;
	struct hci_uart *hu;

	if (!hdev) {
		BT_ERR("Frame for unknown device (hdev=NULL)");
		return -ENODEV;
	}

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return -EBUSY;

	hu = (struct hci_uart *) hdev->driver_data;

	BT_DBG("%s: type %d len %d", hdev->name, bt_cb(skb)->pkt_type, skb->len);

	hu->proto->enqueue(hu, skb);

	hci_uart_tx_wakeup(hu);

	return 0;
}

static void hci_uart_destruct(struct hci_dev *hdev)
{
	if (!hdev)
		return;

	BT_DBG("%s", hdev->name);
}

/* ------ LDISC part ------ */
/* hci_uart_tty_open
 * 
 *     Called when line discipline changed to HCI_UART.
 *
 * Arguments:
 *     tty    pointer to tty info structure
 * Return Value:    
 *     0 if success, otherwise error code
 */
static int hci_uart_tty_open(struct tty_struct *tty)
{
	struct hci_uart *hu = (void *) tty->disc_data;

	BT_DBG("tty %p", tty);

	/* FIXME: This btw is bogus, nothing requires the old ldisc to clear
	   the pointer */
	if (hu)
		return -EEXIST;

	/* Error if the tty has no write op instead of leaving an exploitable
	   hole */
	if (tty->ops->write == NULL)
		return -EOPNOTSUPP;

	if (!(hu = kzalloc(sizeof(struct hci_uart), GFP_KERNEL))) {
		BT_ERR("Can't allocate control structure");
		return -ENFILE;
	}

	tty->disc_data = hu;
	hu->tty = tty;
	tty->receive_room = 65536;

	spin_lock_init(&hu->rx_lock);
	tasklet_init(&hu->tx_tasklet, hci_uart_tx_deferred_wakeup,
			(unsigned long)hu);

	/* Flush any pending characters in the driver and line discipline. */

	/* FIXME: why is this needed. Note don't use ldisc_ref here as the
	   open path is before the ldisc is referencable */

	if (tty->ldisc->ops->flush_buffer)
		tty->ldisc->ops->flush_buffer(tty);
	tty_driver_flush_buffer(tty);

	return 0;
}

/* hci_uart_tty_close()
 *
 *    Called when the line discipline is changed to something
 *    else, the tty is closed, or the tty detects a hangup.
 */
static void hci_uart_tty_close(struct tty_struct *tty)
{
	struct hci_uart *hu = (void *)tty->disc_data;

	BT_DBG("tty %p", tty);

	/* Detach from the tty */
	tty->disc_data = NULL;

	if (hu) {
		struct hci_dev *hdev = hu->hdev;

		tasklet_kill(&hu->tx_tasklet);

		if (hdev)
			hci_uart_close(hdev);

		if (test_and_clear_bit(HCI_UART_PROTO_SET, &hu->flags)) {
			if (hdev) {
				hci_unregister_dev(hdev);
				hci_free_dev(hdev);
			}
			hu->proto->close(hu);
		}
		kfree(hu);
	}
}

/* hci_uart_tty_wakeup()
 *
 *    Callback for transmit wakeup. Called when low level
 *    device driver can accept more send data.
 *
 * Arguments:        tty    pointer to associated tty instance data
 * Return Value:    None
 */
static void hci_uart_tty_wakeup(struct tty_struct *tty)
{
	struct hci_uart *hu = (void *)tty->disc_data;

#if defined(CONFIG_BT_CSR8811)
	if(hu->hdev == NULL)
		return ;
#endif

	BT_DBG("");

	if (!hu)
		return;

	clear_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);

	if (tty != hu->tty)
		return;

	if (test_bit(HCI_UART_PROTO_SET, &hu->flags))
		hci_uart_tx_wakeup(hu);
}

/* hci_uart_tty_receive()
 * 
 *     Called by tty low level driver when receive data is
 *     available.
 *     
 * Arguments:  tty          pointer to tty isntance data
 *             data         pointer to received data
 *             flags        pointer to flags for data
 *             count        count of received data in bytes
 *     
 * Return Value:    None
 */
static void hci_uart_tty_receive(struct tty_struct *tty, const u8 *data, char *flags, int count)
{
	struct hci_uart *hu = (void *)tty->disc_data;

#if defined(CONFIG_BT_CSR8811)
	if(hu->hdev == NULL)
		return ;
#endif

	if (!hu || tty != hu->tty)
		return;

	if (!test_bit(HCI_UART_PROTO_SET, &hu->flags))
		return;

/* CSR8811 Project(Dayton.Kim) 2012.02.23 */
	if (hu == NULL || hu->proto == NULL || hu->proto->recv == NULL || data == NULL)
		return;
/* CSR8811 Project(Dayton.Kim) end */

	spin_lock(&hu->rx_lock);
	hu->proto->recv(hu, (void *) data, count);
	hu->hdev->stat.byte_rx += count;
	spin_unlock(&hu->rx_lock);

	tty_unthrottle(tty);
}

static int hci_uart_register_dev(struct hci_uart *hu)
{
	struct hci_dev *hdev;

	BT_DBG("");

	BT_ERR("hci_uart_register_dev");

	/* Initialize and register HCI device */
	hdev = hci_alloc_dev();
	if (!hdev) {
		BT_ERR("Can't allocate HCI device");
		return -ENOMEM;
	}

	hu->hdev = hdev;

	hdev->bus = HCI_UART;
	hdev->driver_data = hu;

	hdev->open  = hci_uart_open;
	hdev->close = hci_uart_close;
	hdev->flush = hci_uart_flush;
	hdev->send  = hci_uart_send_frame;
	hdev->destruct = hci_uart_destruct;
	hdev->parent = hu->tty->dev;

	hdev->owner = THIS_MODULE;

	if (!reset)
		set_bit(HCI_QUIRK_NO_RESET, &hdev->quirks);

	if (test_bit(HCI_UART_RAW_DEVICE, &hu->hdev_flags))
		set_bit(HCI_QUIRK_RAW_DEVICE, &hdev->quirks);

	if (hci_register_dev(hdev) < 0) {
		BT_ERR("Can't register HCI device");
		hci_free_dev(hdev);
		return -ENODEV;
	}

	return 0;
}

static int hci_uart_set_proto(struct hci_uart *hu, int id)
{
	struct hci_uart_proto *p;
	int err;

	p = hci_uart_get_proto(id);
	if (!p)
		return -EPROTONOSUPPORT;

	err = p->open(hu);
	if (err)
		return err;

	hu->proto = p;

	err = hci_uart_register_dev(hu);
	if (err) {
		p->close(hu);
		return err;
	}

	return 0;
}

/* hci_uart_tty_ioctl()
 *
 *    Process IOCTL system call for the tty device.
 *
 * Arguments:
 *
 *    tty        pointer to tty instance data
 *    file       pointer to open file object for device
 *    cmd        IOCTL command code
 *    arg        argument for IOCTL call (cmd dependent)
 *
 * Return Value:    Command dependent
 */
static int hci_uart_tty_ioctl(struct tty_struct *tty, struct file * file,
					unsigned int cmd, unsigned long arg)
{
	struct hci_uart *hu = (void *)tty->disc_data;
	int err = 0;

	BT_DBG("");

	/* Verify the status of the device */
	if (!hu)
		return -EBADF;

	switch (cmd) {
	case HCIUARTSETPROTO:
		BT_DBG("SETPROTO %lu hu %p", arg, hu);
		if (!test_and_set_bit(HCI_UART_PROTO_SET, &hu->flags)) {
			BT_DBG("called hci_uart_set_proto");
			err = hci_uart_set_proto(hu, arg);
			if (err) {
				BT_DBG("error set proto");
				clear_bit(HCI_UART_PROTO_SET, &hu->flags);
				return err;
			}
			tty->low_latency = 1;
		} else
			return -EBUSY;
		break;

	case HCIUARTGETPROTO:
		BT_DBG("GETPROTO");
		if (test_bit(HCI_UART_PROTO_SET, &hu->flags))
			return hu->proto->id;
		return -EUNATCH;

	case HCIUARTGETDEVICE:
		if (test_bit(HCI_UART_PROTO_SET, &hu->flags))
			return hu->hdev->id;
		return -EUNATCH;

	case HCIUARTSETFLAGS:
		if (test_bit(HCI_UART_PROTO_SET, &hu->flags))
			return -EBUSY;
		hu->hdev_flags = arg;
		break;

	case HCIUARTGETFLAGS:
		return hu->hdev_flags;

	default:
		err = n_tty_ioctl_helper(tty, file, cmd, arg);
		break;
	};

	return err;
}

/*
 * We don't provide read/write/poll interface for user space.
 */
struct hci_uart_hook {
    unsigned int len;
    unsigned char *head;
    unsigned char data[HCI_MAX_EVENT_SIZE + 1];  /* save packet type at data[0] and then place event packet */
};

static struct hci_uart_hook *hook;
static DECLARE_WAIT_QUEUE_HEAD(read_wait);

void hci_uart_tty_read_hook(struct sk_buff *skb)
{
    if (!hook) {
		BT_DBG("%s: hooking wasn't requested, skip it", __func__);
		goto hci_uart_tty_read_hook_exit;
    }

	if (bt_cb(skb)->pkt_type != HCI_EVENT_PKT) {
		BT_DBG("%s: Packet type is %d, skip it", __func__, bt_cb(skb)->pkt_type);
	goto hci_uart_tty_read_hook_exit;
    }

	BT_DBG("%s: Received len = %d", __func__, skb->len);
	if (skb->len > sizeof(hook->data)-1) {
		BT_DBG("Packet size exceeds max len, skip it");
		goto hci_uart_tty_read_hook_exit;
    }

    memcpy(hook->data, &bt_cb(skb)->pkt_type, 1);
    skb_copy_from_linear_data(skb, &hook->data[1], skb->len);
    hook->len = skb->len + 1;

hci_uart_tty_read_hook_exit:
    wake_up_interruptible(&read_wait);
}
EXPORT_SYMBOL(hci_uart_tty_read_hook);

static int hci_uart_tty_access_allowed(void)
{
    char name[TASK_COMM_LEN];
    get_task_comm(name, current_thread_info()->task);
    BT_DBG("%s: %s", __func__, name);
    if (strcmp(name, "brcm_poke_helpe")) {
		BT_ERR("%s isn't allowed", name);
		return -EACCES;
    }

    return 0;
}

static ssize_t hci_uart_tty_read(struct tty_struct *tty, struct file *file,
					unsigned char __user *buf, size_t nr)
{
	struct hci_uart *hu = (void *) tty->disc_data;
	struct hci_dev *hdev = hu->hdev;
	int ret = 0, count;

	BT_DBG("%s: hu = 0x%p hci_dev = 0x%p, nr = %d", __func__, hu, hdev, nr);

	ret = hci_uart_tty_access_allowed();
    if (ret < 0)
		return ret;

    if (!hook)
		return -ENOMEM;

    if (!hook->len)
		interruptible_sleep_on_timeout(&read_wait, 3 * HZ);

    if (!hook->len) {
		BT_INFO("No data to read");
    } else {
		count = nr > hook->len ? hook->len : nr;

		ret = copy_to_user(buf, hook->head, count);

		hook->len -= (count - ret);
			hook->head += (count - ret);

			ret = count - ret;
    }

    if (!hook->len) {
		BT_DBG("%s: free hook", __func__);
		kfree(hook);
		hook = NULL;
    }

    BT_DBG("%s: ret = %d", __func__, ret);

	return ret;
}

static ssize_t hci_uart_tty_write(struct tty_struct *tty, struct file *file,
					const unsigned char *data, size_t count)
{
    struct hci_uart *hu = (void *) tty->disc_data;
    struct hci_dev *hdev = hu->hdev;
    int ret;

    BT_DBG("%s: hu = 0x%p, hci_dev = 0x%p", __func__, hu, hdev);

    ret = hci_uart_tty_access_allowed();
    if (ret < 0)
		return ret;

    if (!hdev)
		return -ENODEV;

    if (!hook)
		hook = kzalloc(sizeof(*hook), GFP_KERNEL);
    else {
		/* Cuase brcm_poke_helper's read/write is serialized,
		* it's almost safe to init hook data here
		*/
		BT_INFO("hook data still remains");
		memset(hook, 0, sizeof(*hook));
    }

    if (!hook)
		return -ENOMEM;

    hook->head = hook->data;

    hci_uart_flush(hdev);

#if 1
    if (hdev->wake_peer)
		hdev->wake_peer(hdev);
#endif

    ret = tty->ops->write(tty, data, count);

    BT_DBG("%s: ret = %d", __func__, ret);

	return ret;
}

static unsigned int hci_uart_tty_poll(struct tty_struct *tty,
					struct file *filp, poll_table *wait)
{
	return 0;
}

static int __init hci_uart_init(void)
{
	static struct tty_ldisc_ops hci_uart_ldisc;
	int err;

	BT_INFO("HCI UART driver ver %s", VERSION);

	/* Register the tty discipline */

	memset(&hci_uart_ldisc, 0, sizeof (hci_uart_ldisc));
	hci_uart_ldisc.magic		= TTY_LDISC_MAGIC;
	hci_uart_ldisc.name		= "n_hci";
	hci_uart_ldisc.open		= hci_uart_tty_open;
	hci_uart_ldisc.close		= hci_uart_tty_close;
	hci_uart_ldisc.read		= hci_uart_tty_read;
	hci_uart_ldisc.write		= hci_uart_tty_write;
	hci_uart_ldisc.ioctl		= hci_uart_tty_ioctl;
	hci_uart_ldisc.poll		= hci_uart_tty_poll;
	hci_uart_ldisc.receive_buf	= hci_uart_tty_receive;
	hci_uart_ldisc.write_wakeup	= hci_uart_tty_wakeup;
	hci_uart_ldisc.owner		= THIS_MODULE;

	if ((err = tty_register_ldisc(N_HCI, &hci_uart_ldisc))) {
		BT_ERR("HCI line discipline registration failed. (%d)", err);
		return err;
	}

#ifdef CONFIG_BT_HCIUART_H4
	h4_init();
#endif
#ifdef CONFIG_BT_HCIUART_BCSP
	bcsp_init();
#endif
#ifdef CONFIG_BT_HCIUART_LL
	ll_init();
#endif
#ifdef CONFIG_BT_HCIUART_ATH3K
	ath_init();
#endif
#ifdef CONFIG_BT_HCIUART_BRCM
	brcm_init();
#endif

	return 0;
}

static void __exit hci_uart_exit(void)
{
	int err;

#ifdef CONFIG_BT_HCIUART_H4
	h4_deinit();
#endif
#ifdef CONFIG_BT_HCIUART_BCSP
	bcsp_deinit();
#endif
#ifdef CONFIG_BT_HCIUART_LL
	ll_deinit();
#endif
#ifdef CONFIG_BT_HCIUART_BRCM
	brcm_deinit();
#endif

	/* Release tty registration of line discipline */
	if ((err = tty_unregister_ldisc(N_HCI)))
		BT_ERR("Can't unregister HCI line discipline (%d)", err);
}

module_init(hci_uart_init);
module_exit(hci_uart_exit);

module_param(reset, bool, 0644);
MODULE_PARM_DESC(reset, "Send HCI reset command on initialization");

MODULE_AUTHOR("Marcel Holtmann <marcel@holtmann.org>");
MODULE_DESCRIPTION("Bluetooth HCI UART driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
MODULE_ALIAS_LDISC(N_HCI);
