/* SPDX-License-Identifier: (GPL-2.0) */
/**
 * Microchip CoreTSN driver
 *
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Pallela Venkat Karthik <pallela.karthik@microchip.com>
 *
 */

#ifndef _MICROCHIP_TSN_CMDS_H_
#define _MICROCHIP_TSN_CMDS_H_

#define ETHER_ADDR_LEN			6
#define MAX_TSN_CONFIG_SIZE		4096
#define MCHP_TSN_NUM_PRIO_QUEUES	7
#define TSN_CMD_ERR_STR_LEN		128

struct mchp_tsn_config_cmd_resp {
	u8  cmd;
	u8  cmd_status;
	__be64 tsn_dev_id;
	__be16 tsn_config_size;
	u8  cmd_status_string_avail;
	u8  cmd_status_string[TSN_CMD_ERR_STR_LEN];
	u8  tsn_config_data[];
} __packed;

#define MCHP_TSN_CONFIG_CMD      _IOWR('P', 1, struct mchp_tsn_config_cmd_resp)

struct mchp_tsn_config_device_info {
	__be64 tsn_dev_id;
} __packed;

struct mchp_tsn_gcl_entry {
	__be32 time_interval;
	u8  gate_state;
} __packed;

struct mchp_tsn_config_qbv {
	u8 initial_gate_state;
	u8 priority_enable;
	u8 gate_enable;
	u8 priority_queue_enable;
	u8 priority_queue_prios[MCHP_TSN_NUM_PRIO_QUEUES];
	u8 control_list_length;
	__be32 cycle_time;
	__be64 basetime_sec;
	__be32 basetime_nsec;
	u8 basetime_adjust;
	struct mchp_tsn_gcl_entry gcle[];

} __packed;

struct mchp_tsn_config_qbu {
	u8 pre_empt_en;
	__be16 pre_empt_size;
} __packed;

struct mchp_tsn_config_qci {
	u8 da_check;
	u8 sa_check;
	u8 destination_mac_addr[ETHER_ADDR_LEN];
	u8 source_mac_addr[ETHER_ADDR_LEN];
} __packed;

struct mchp_tsn_config_misc_rx_port_id {
	u8 port_id_rx_check;
	__be16 port_id_rx;
} __packed;

struct mchp_tsn_config_misc_ptp_tx_prioq {
	u8 ptp_tx_prioq;
} __packed;

struct mchp_tsn_config_misc_length_deduct_byte {
	__be16 crc_deduct_len;
} __packed;

enum MCHP_TSN_CONFIG_CMDS {
	MCHP_TSN_GET_QBV = 1,
	MCHP_TSN_GET_QBU,
	MCHP_TSN_GET_QCI,
	MCHP_TSN_SET_QBV,
	MCHP_TSN_SET_QBU,
	MCHP_TSN_SET_QCI,
	MCHP_TSN_GET_MISC_RX_PORT,
	MCHP_TSN_SET_MISC_RX_PORT,
	MCHP_TSN_GET_MISC_PTP_TX_PRIOQ,
	MCHP_TSN_SET_MISC_PTP_TX_PRIOQ,
	MCHP_TSN_GET_MISC_LENGTH_DEDUCT_BYTE,
	MCHP_TSN_SET_MISC_LENGTH_DEDUCT_BYTE,
};

#endif /* _MICROCHIP_TSN_CMDS_H_ */
