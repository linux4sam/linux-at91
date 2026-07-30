// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2024 hexDEV GmbH - https://hexdev.de */

#include <linux/can/core.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/can/skb.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/lin.h>

static const u8 lin_id_parity_tbl[] = {
	0x80, 0xC0, 0x40, 0x00, 0xC0, 0x80, 0x00, 0x40,
	0x00, 0x40, 0xC0, 0x80, 0x40, 0x00, 0x80, 0xC0,
	0x40, 0x00, 0x80, 0xC0, 0x00, 0x40, 0xC0, 0x80,
	0xC0, 0x80, 0x00, 0x40, 0x80, 0xC0, 0x40, 0x00,
	0x00, 0x40, 0xC0, 0x80, 0x40, 0x00, 0x80, 0xC0,
	0x80, 0xC0, 0x40, 0x00, 0xC0, 0x80, 0x00, 0x40,
	0xC0, 0x80, 0x00, 0x40, 0x80, 0xC0, 0x40, 0x00,
	0x40, 0x00, 0x80, 0xC0, 0x00, 0x40, 0xC0, 0x80,
};

u8 lin_get_id_parity(u8 id)
{
	return lin_id_parity_tbl[id & LIN_ID_MASK];
}
EXPORT_SYMBOL_GPL(lin_get_id_parity);

static ssize_t lin_identifier_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct lin_device *ldev = netdev_priv(to_net_dev(dev));
	struct lin_responder_answer answ = {};
	unsigned int k;
	int count, ret;
	unsigned long id;

	if (!ldev->ldev_ops->get_responder_answer)
		return -EOPNOTSUPP;

	/* attribute name is a fixed "00".."3f", so id is always in range */
	ret = kstrtoul(attr->attr.name, 16, &id);
	if (ret)
		return ret;

	count = sysfs_emit(buf, "%-6s %-11s %-9s %-9s %-2s %-24s %-6s\n",
			   "state", "cksum-mode", "is_event", "event_id",
			   "n", "data", "cksum");

	ret = ldev->ldev_ops->get_responder_answer(ldev, id, &answ);
	if (ret)
		return ret;

	/* guard the fixed-size data[] against a misbehaving backend */
	if (answ.lf.len > LIN_MAX_DLEN)
		return -EPROTO;

	count += sysfs_emit_at(buf, count, "%-6s %-11s %-9s %-9u %-2u ",
			       answ.is_active ? "active" : "off",
			       answ.lf.checksum_mode ? "enhanced" : "classic",
			       answ.is_event_frame ? "yes" : "no",
			       answ.event_associated_id,
			       answ.lf.len);

	for (k = 0; k < answ.lf.len; k++)
		count += sysfs_emit_at(buf, count, "%02x ", answ.lf.data[k]);
	for (; k < LIN_MAX_DLEN; k++)
		count += sysfs_emit_at(buf, count, "   ");
	/* cksum column is always present, even for a zero-length answer */
	count += sysfs_emit_at(buf, count, " %02x", answ.lf.checksum);

	count += sysfs_emit_at(buf, count, "\n");

	return count;
}

static const char *parse_and_advance(const char *buf, unsigned long *result,
				     unsigned int base)
{
	char num_str[5] = {};
	unsigned int num_len = 0;

	while (*buf && isspace(*buf))
		buf++;
	while (*buf && isalnum(*buf)) {
		if (num_len >= sizeof(num_str) - 1)
			return NULL;	/* number too long */
		num_str[num_len++] = *buf++;
	}
	if (kstrtoul(num_str, base, result))
		return NULL;

	return buf;
}

static ssize_t lin_identifier_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct lin_device *ldev = netdev_priv(to_net_dev(dev));
	struct lin_responder_answer answ = {};
	const char *ptr = buf;
	unsigned long v;
	int ret;

	if (!ldev->ldev_ops->update_responder_answer)
		return -EOPNOTSUPP;

	/* attribute name is a fixed "00".."3f", so the id is always in range */
	ret = kstrtoul(attr->attr.name, 16, &v);
	if (ret)
		return ret;
	answ.lf.lin_id = v;

	ptr = parse_and_advance(ptr, &v, 2);
	if (!ptr || v > 1)
		return -EINVAL;
	answ.is_active = v;

	ptr = parse_and_advance(ptr, &v, 2);
	if (!ptr || v > 1)
		return -EINVAL;
	answ.lf.checksum_mode = v;

	ptr = parse_and_advance(ptr, &v, 2);
	if (!ptr || v > 1)
		return -EINVAL;
	answ.is_event_frame = v;

	ptr = parse_and_advance(ptr, &v, 16);
	if (!ptr || v > LIN_ID_MASK)
		return -EINVAL;
	answ.event_associated_id = v;

	ptr = parse_and_advance(ptr, &v, 16);
	if (!ptr || v > LIN_MAX_DLEN)
		return -EINVAL;
	answ.lf.len = v;

	for (unsigned int i = 0; i < answ.lf.len; i++) {
		ptr = parse_and_advance(ptr, &v, 16);
		if (!ptr || v > U8_MAX)
			return -EINVAL;
		answ.lf.data[i] = v;
	}

	/* reject any trailing non-whitespace input */
	while (*ptr && isspace(*ptr))
		ptr++;
	if (*ptr)
		return -EINVAL;

	ret = ldev->ldev_ops->update_responder_answer(ldev, &answ);
	if (ret)
		return ret;

	return count;
}

#define LID(_name) \
	static struct device_attribute linid_##_name = __ATTR(_name, 0644, \
	lin_identifier_show, lin_identifier_store)

LID(00); LID(01); LID(02); LID(03); LID(04); LID(05); LID(06); LID(07);
LID(08); LID(09); LID(0a); LID(0b); LID(0c); LID(0d); LID(0e); LID(0f);
LID(10); LID(11); LID(12); LID(13); LID(14); LID(15); LID(16); LID(17);
LID(18); LID(19); LID(1a); LID(1b); LID(1c); LID(1d); LID(1e); LID(1f);
LID(20); LID(21); LID(22); LID(23); LID(24); LID(25); LID(26); LID(27);
LID(28); LID(29); LID(2a); LID(2b); LID(2c); LID(2d); LID(2e); LID(2f);
LID(30); LID(31); LID(32); LID(33); LID(34); LID(35); LID(36); LID(37);
LID(38); LID(39); LID(3a); LID(3b); LID(3c); LID(3d); LID(3e); LID(3f);

static struct attribute *lin_sysfs_attrs[] = {
	&linid_00.attr, &linid_01.attr, &linid_02.attr, &linid_03.attr,
	&linid_04.attr, &linid_05.attr, &linid_06.attr, &linid_07.attr,
	&linid_08.attr, &linid_09.attr, &linid_0a.attr, &linid_0b.attr,
	&linid_0c.attr, &linid_0d.attr, &linid_0e.attr, &linid_0f.attr,
	&linid_10.attr, &linid_11.attr, &linid_12.attr, &linid_13.attr,
	&linid_14.attr, &linid_15.attr, &linid_16.attr, &linid_17.attr,
	&linid_18.attr, &linid_19.attr, &linid_1a.attr, &linid_1b.attr,
	&linid_1c.attr, &linid_1d.attr, &linid_1e.attr, &linid_1f.attr,
	&linid_20.attr, &linid_21.attr, &linid_22.attr, &linid_23.attr,
	&linid_24.attr, &linid_25.attr, &linid_26.attr, &linid_27.attr,
	&linid_28.attr, &linid_29.attr, &linid_2a.attr, &linid_2b.attr,
	&linid_2c.attr, &linid_2d.attr, &linid_2e.attr, &linid_2f.attr,
	&linid_30.attr, &linid_31.attr, &linid_32.attr, &linid_33.attr,
	&linid_34.attr, &linid_35.attr, &linid_36.attr, &linid_37.attr,
	&linid_38.attr, &linid_39.attr, &linid_3a.attr, &linid_3b.attr,
	&linid_3c.attr, &linid_3d.attr, &linid_3e.attr, &linid_3f.attr,
	NULL
};

/* hide the id files on backends that do not answer as a responder */
static umode_t lin_sysfs_is_visible(struct kobject *kobj,
				    struct attribute *attr, int n)
{
	struct lin_device *ldev = netdev_priv(to_net_dev(kobj_to_dev(kobj)));

	if (!ldev->ldev_ops->get_responder_answer)
		return 0;

	/* read-only unless the backend can also store an answer */
	if (!ldev->ldev_ops->update_responder_answer)
		return attr->mode & ~0222;

	return attr->mode;
}

static const struct attribute_group lin_sysfs_group = {
	.name = "lin_ids",
	.attrs = lin_sysfs_attrs,
	.is_visible = lin_sysfs_is_visible,
};

static void lin_tx_work_handler(struct work_struct *ws)
{
	struct lin_device *ldev = container_of(ws, struct lin_device,
					       tx_work);
	struct net_device *ndev = ldev->ndev;
	struct canfd_frame *cfd;
	struct lin_frame lf;
	int ret;

	cfd = (struct canfd_frame *)ldev->tx_skb->data;

	if (cfd->len > LIN_MAX_DLEN) {
		DEV_STATS_INC(ndev, tx_dropped);
		netdev_warn_once(ndev, "tx frame exceeds %u bytes\n",
				 LIN_MAX_DLEN);
		dev_kfree_skb(ldev->tx_skb);
		goto lin_tx_out;
	}

	lf.checksum_mode = (cfd->can_id & LIN_ENHANCED_CKSUM_FLAG) ?
			   LINBUS_ENHANCED : LINBUS_CLASSIC;
	lf.lin_id = cfd->can_id & LIN_ID_MASK;
	lf.len = cfd->len;
	memcpy(lf.data, cfd->data, lf.len);

	/* consumes the skb on every path, so a failure leaves no echo */
	ret = can_put_echo_skb(ldev->tx_skb, ndev, 0, lf.len);
	if (ret) {
		DEV_STATS_INC(ndev, tx_dropped);
		goto lin_tx_out;
	}

	ret = ldev->ldev_ops->ldo_tx(ldev, &lf);
	if (ret) {
		DEV_STATS_INC(ndev, tx_dropped);
		netdev_err_once(ndev, "transmission failure %d\n", ret);
		can_free_echo_skb(ndev, 0, NULL);
		goto lin_tx_out;
	}

	DEV_STATS_INC(ndev, tx_packets);
	DEV_STATS_ADD(ndev, tx_bytes, can_get_echo_skb(ndev, 0, NULL));

lin_tx_out:
	ldev->tx_skb = NULL;
	netif_wake_queue(ndev);
}

static netdev_tx_t lin_start_xmit(struct sk_buff *skb,
				  struct net_device *ndev)
{
	struct lin_device *ldev = netdev_priv(ndev);
	struct canfd_frame *cfd = (struct canfd_frame *)skb->data;

	if (can_dropped_invalid_skb(ndev, skb))
		return NETDEV_TX_OK;

	/* LIN carries a 6-bit id only: no EFF/RTR/ERR, no bits above the mask */
	if (cfd->can_id & ~(LIN_ID_MASK | LIN_ENHANCED_CKSUM_FLAG)) {
		DEV_STATS_INC(ndev, tx_dropped);
		netdev_warn_once(ndev, "cannot represent can_id %#x on LIN\n",
				 cfd->can_id);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	netif_stop_queue(ndev);
	ldev->tx_skb = skb;
	queue_work(ldev->wq, &ldev->tx_work);

	return NETDEV_TX_OK;
}

static int lin_open(struct net_device *ndev)
{
	struct lin_device *ldev = netdev_priv(ndev);
	int ret;

	ret = open_candev(ndev);
	if (ret)
		return ret;

	/* go live before arming rx, else lin_rx drops the gap frame */
	ldev->can.state = CAN_STATE_ERROR_ACTIVE;

	ret = ldev->ldev_ops->ldo_open(ldev);
	if (ret) {
		ldev->can.state = CAN_STATE_STOPPED;
		/* quiesce any rx a partial open armed before the ldev is freed */
		ldev->ldev_ops->ldo_stop(ldev);
		/* open_candev raised the carrier, drop it as the open failed */
		netif_carrier_off(ndev);
		close_candev(ndev);
		return ret;
	}

	ndev->mtu = CAN_MTU;
	netif_wake_queue(ndev);

	return 0;
}

static int lin_stop(struct net_device *ndev)
{
	struct lin_device *ldev = netdev_priv(ndev);

	flush_work(&ldev->tx_work);

	close_candev(ndev);

	ldev->can.state = CAN_STATE_STOPPED;

	return ldev->ldev_ops->ldo_stop(ldev);
}

static const struct net_device_ops lin_netdev_ops = {
	.ndo_open = lin_open,
	.ndo_stop = lin_stop,
	.ndo_start_xmit = lin_start_xmit,
	.ndo_change_mtu = can_change_mtu,
};

u8 lin_get_checksum(u8 pid, u8 n_of_bytes, const u8 *bytes,
		    enum lin_checksum_mode cm)
{
	unsigned int csum = 0;
	unsigned int i;

	if (cm == LINBUS_ENHANCED)
		csum += pid;

	for (i = 0; i < n_of_bytes; i++) {
		csum += bytes[i];
		if (csum > 255)
			csum -= 255;
	}

	return ~csum & 0xff;
}
EXPORT_SYMBOL_GPL(lin_get_checksum);

static int lin_bump_rx_err(struct lin_device *ldev, const struct lin_frame *lf)
{
	struct net_device *ndev = ldev->ndev;
	struct can_frame cf = {};

	if (lf->lin_id > LIN_ID_MASK) {
		netdev_dbg(ndev, "id exceeds LIN max id\n");
		cf.can_id = CAN_ERR_FLAG | CAN_ERR_PROT;
		cf.data[3] = CAN_ERR_PROT_LOC_ID12_05;
	}

	if (lf->len > LIN_MAX_DLEN) {
		netdev_dbg(ndev, "frame exceeds number of bytes\n");
		cf.can_id = CAN_ERR_FLAG | CAN_ERR_PROT;
		cf.data[3] = CAN_ERR_PROT_LOC_DLC;
	}

	if (lf->len <= LIN_MAX_DLEN) {
		u8 checksum = lin_get_checksum(LIN_FORM_PID(lf->lin_id),
					       lf->len, lf->data,
					       lf->checksum_mode);

		if (checksum != lf->checksum) {
			netdev_dbg(ndev, "expected cksm: 0x%02x got: 0x%02x\n",
				   checksum, lf->checksum);
			cf.can_id = CAN_ERR_FLAG | CAN_ERR_PROT;
			cf.data[2] = CAN_ERR_PROT_FORM;
		}
	}

	if (cf.can_id & CAN_ERR_FLAG) {
		struct can_frame *err_cf;
		struct sk_buff *skb = alloc_can_err_skb(ndev, &err_cf);

		if (unlikely(!skb))
			return -ENOMEM;

		err_cf->can_id |= cf.can_id;
		memcpy(err_cf->data, cf.data, CAN_MAX_DLEN);

		netif_rx(skb);

		return -EREMOTEIO;
	}

	return 0;
}

int lin_rx(struct lin_device *ldev, const struct lin_frame *lf)
{
	struct net_device *ndev = ldev->ndev;
	struct can_frame *cf;
	struct sk_buff *skb;
	int ret;

	if (ldev->can.state == CAN_STATE_STOPPED)
		return 0;

	/* clamp %*ph: lf->len is validated later, in lin_bump_rx_err() */
	netdev_dbg(ndev, "id:%02x, len:%u, data:%*ph, checksum:%02x (%s)\n",
		   lf->lin_id, lf->len, min_t(u8, lf->len, LIN_MAX_DLEN),
		   lf->data, lf->checksum,
		   lf->checksum_mode ? "enhanced" : "classic");

	ret = lin_bump_rx_err(ldev, lf);
	if (ret) {
		DEV_STATS_INC(ndev, rx_dropped);
		return ret;
	}

	skb = alloc_can_skb(ndev, &cf);
	if (unlikely(!skb)) {
		DEV_STATS_INC(ndev, rx_dropped);
		return -ENOMEM;
	}

	cf->can_id = lf->lin_id;
	if (lf->checksum_mode == LINBUS_ENHANCED)
		cf->can_id |= LIN_ENHANCED_CKSUM_FLAG;
	cf->len = lf->len;
	memcpy(cf->data, lf->data, cf->len);

	DEV_STATS_INC(ndev, rx_packets);
	DEV_STATS_ADD(ndev, rx_bytes, cf->len);

	netif_receive_skb(skb);

	return 0;
}
EXPORT_SYMBOL_GPL(lin_rx);

static int lin_set_bittiming(struct net_device *ndev)
{
	struct lin_device *ldev = netdev_priv(ndev);
	unsigned int bitrate = ldev->can.bittiming.bitrate;

	return ldev->ldev_ops->update_bitrate(ldev, bitrate);
}

static const u32 lin_bitrate[] = { 1200, 2400, 4800, 9600, 19200 };

struct lin_device *register_lin(struct device *dev,
				const struct lin_device_ops *ldops)
{
	struct net_device *ndev;
	struct lin_device *ldev;
	int ret;

	if (WARN_ON_ONCE(!ldops || !ldops->ldo_tx || !ldops->update_bitrate ||
			 !ldops->ldo_open || !ldops->ldo_stop))
		return ERR_PTR(-EINVAL);

	ndev = alloc_candev(sizeof(struct lin_device), 1);
	if (!ndev)
		return ERR_PTR(-ENOMEM);

	ldev = netdev_priv(ndev);

	ldev->ldev_ops = ldops;
	ndev->netdev_ops = &lin_netdev_ops;
	ndev->flags |= IFF_ECHO;
	ndev->mtu = CAN_MTU;
	ndev->sysfs_groups[0] = &lin_sysfs_group;
	ldev->can.bittiming.bitrate = LIN_DEFAULT_BAUDRATE;
	ldev->can.ctrlmode = CAN_CTRLMODE_LIN;
	ldev->can.ctrlmode_supported = 0;
	ldev->can.bitrate_const = lin_bitrate;
	ldev->can.bitrate_const_cnt = ARRAY_SIZE(lin_bitrate);
	ldev->can.do_set_bittiming = lin_set_bittiming;
	ldev->ndev = ndev;
	ldev->dev = dev;

	SET_NETDEV_DEV(ndev, dev);

	ret = lin_set_bittiming(ndev);
	if (ret) {
		netdev_err(ndev, "set bittiming failed\n");
		goto exit_candev;
	}

	/* Using workqueue as tx over USB/SPI/... may sleep */
	ldev->wq = alloc_workqueue(dev_name(dev), WQ_FREEZABLE | WQ_MEM_RECLAIM,
				   0);
	if (!ldev->wq) {
		ret = -ENOMEM;
		goto exit_candev;
	}

	INIT_WORK(&ldev->tx_work, lin_tx_work_handler);

	ret = register_candev(ndev);
	if (ret)
		goto exit_wq;

	netdev_info(ndev, "LIN initialized\n");

	return ldev;

exit_wq:
	destroy_workqueue(ldev->wq);
exit_candev:
	free_candev(ndev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(register_lin);

void unregister_lin(struct lin_device *ldev)
{
	struct net_device *ndev = ldev->ndev;

	unregister_candev(ndev);
	destroy_workqueue(ldev->wq);
	free_candev(ndev);
}
EXPORT_SYMBOL_GPL(unregister_lin);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Christoph Fritz <christoph.fritz@hexdev.de>");
MODULE_DESCRIPTION("LIN bus to CAN glue driver");
