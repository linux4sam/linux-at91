/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2024 hexDEV GmbH - https://hexdev.de */

#ifndef _NET_LIN_H_
#define _NET_LIN_H_

#include <linux/bitfield.h>
#include <linux/can/dev.h>
#include <linux/device.h>

#define LIN_NUM_IDS		64
#define LIN_HEADER_SIZE		3
#define LIN_MAX_DLEN		8

#define LIN_MAX_BAUDRATE	20000
#define LIN_MIN_BAUDRATE	1000
#define LIN_DEFAULT_BAUDRATE	9600
#define LIN_SYNC_BYTE		0x55

#define LIN_ID_MASK		GENMASK(5, 0)
/* special ID descriptions for LIN */
#define LIN_RXOFFLOAD_DATA_FLAG	BIT(9)
#define LIN_ENHANCED_CKSUM_FLAG	BIT(8)

u8 lin_get_id_parity(u8 id);

#define LIN_GET_ID(PID)		FIELD_GET(LIN_ID_MASK, PID)
#define LIN_FORM_PID(ID)	(LIN_GET_ID(ID) | \
					lin_get_id_parity(LIN_GET_ID(ID)))
#define LIN_GET_PARITY(PID)	((PID) & ~LIN_ID_MASK)
#define LIN_CHECK_PID(PID)	(LIN_GET_PARITY(PID) == \
					LIN_GET_PARITY(LIN_FORM_PID(PID)))

struct lin_attr {
	struct kobj_attribute attr;
	struct lin_device *ldev;
};

struct lin_device {
	struct can_priv can;  /* must be the first member */
	struct net_device *ndev;
	struct device *dev;
	const struct lin_device_ops *ldev_ops;
	struct workqueue_struct *wq;
	struct work_struct tx_work;
	struct sk_buff *tx_skb;
};

enum lin_checksum_mode {
	LINBUS_CLASSIC = 0,
	LINBUS_ENHANCED,
};

struct lin_frame {
	u8 lin_id;
	u8 len;
	u8 data[LIN_MAX_DLEN];
	u8 checksum;
	enum lin_checksum_mode checksum_mode;
};

struct lin_responder_answer {
	bool is_active;
	bool is_event_frame;
	u8 event_associated_id;
	struct lin_frame lf;
};

struct lin_device_ops {
	int (*ldo_open)(struct lin_device *ldev);
	/* ldo_stop must quiesce the backend rx before it returns, as lin_rx()
	 * runs after it only on a freed ldev
	 */
	int (*ldo_stop)(struct lin_device *ldev);
	int (*ldo_tx)(struct lin_device *ldev, const struct lin_frame *frame);
	int (*update_bitrate)(struct lin_device *ldev, u16 bitrate);
	int (*update_responder_answer)(struct lin_device *ldev,
				       const struct lin_responder_answer *answ);
	int (*get_responder_answer)(struct lin_device *ldev, u8 id,
				    struct lin_responder_answer *answ);
};

int lin_rx(struct lin_device *ldev, const struct lin_frame *lf);

u8 lin_get_checksum(u8 pid, u8 n_of_bytes, const u8 *bytes,
		    enum lin_checksum_mode cm);

struct lin_device *register_lin(struct device *dev,
				const struct lin_device_ops *ldops);
void unregister_lin(struct lin_device *ldev);

#endif /* _NET_LIN_H_ */
