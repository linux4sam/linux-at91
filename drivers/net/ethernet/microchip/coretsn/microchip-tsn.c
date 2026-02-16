// SPDX-License-Identifier: (GPL-2.0)
/**
 * Microchip CoreTSN driver
 *
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Pallela Venkat Karthik <pallela.karthik@microchip.com>
 *
 */

#include "microchip-tsn.h"
#include "microchip-tsn-cmds.h"

static const u16 addr_frag_size[] = {60, 124, 188, 252}; /* Supported Frag sizes */

static void mchp_tsn_write_register(struct mchp_tsn_dev *tsn_dev, u32 reg, u32 data, u32 mask)
{
	u32 val;

	val = readl_relaxed(tsn_dev->tsn_reg_base + reg);
	val = (~mask & val) | data;
	writel_relaxed(val, tsn_dev->tsn_reg_base + reg);
}

static u32 gcl_time_to_ns(struct mchp_tsn_dev *tsn_dev, u32 time_interval_gcl)
{
	u64 time_interval_64;
	u32 time_interval_32_ns;

	time_interval_64 = (u64)time_interval_gcl * tsn_dev->qbv_gcl_div;
	time_interval_64 = time_interval_64 / tsn_dev->qbv_gcl_mul;
	time_interval_32_ns = (u32)time_interval_64;

	return time_interval_32_ns;
}

static u32 ns_to_gcl_time(struct mchp_tsn_dev *tsn_dev, u32 time_interval_ns)
{
	u64 time_interval_64;
	u32 time_interval_gcl;

	time_interval_64 = (u64)time_interval_ns * tsn_dev->qbv_gcl_mul;
	time_interval_64 = time_interval_64 / tsn_dev->qbv_gcl_div;
	time_interval_gcl = (u32)time_interval_64;

	return time_interval_gcl;
}

static int mchp_tsn_get_qci_config(struct mchp_tsn_dev *tsn_dev,
				   struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_qci *tsn_qciconf;
	u32 mac_addr_mb;
	u16 mac_addr_lb;
	__be32 mac_addr_msb;
	__be16 mac_addr_lsb;

	tsn_qciconf = (struct mchp_tsn_config_qci *)&tsn_conf->tsn_config_data[0];
	tsn_conf->cmd_status = 0;

	tsn_qciconf->da_check = mchp_tsn_get(tsn_dev, TSN_REG_PSC, TSN_MASK_DA_CHECK);
	tsn_qciconf->sa_check = mchp_tsn_get(tsn_dev, TSN_REG_PSC, TSN_MASK_SA_CHECK);
	dev_dbg(&tsn_dev->pdev->dev, "DA_check :%08x\n", tsn_qciconf->da_check);
	dev_dbg(&tsn_dev->pdev->dev, "SA_check :%08x\n", tsn_qciconf->sa_check);

	mac_addr_mb = mchp_tsn_get(tsn_dev, TSN_REG_DST_MAC_MSB, TSN_MASK_MAC_MSB);
	mac_addr_lb = mchp_tsn_get(tsn_dev, TSN_REG_DST_MAC_LSB, TSN_MASK_MAC_LSB);
	mac_addr_msb = cpu_to_be32(mac_addr_mb);
	mac_addr_lsb = cpu_to_be16(mac_addr_lb);

	dev_dbg(&tsn_dev->pdev->dev, "dst mac_addr_msb :%08x\n", mac_addr_msb);
	dev_dbg(&tsn_dev->pdev->dev, "dst mac_addr_lsb :%08x\n", mac_addr_lsb);

	memcpy(tsn_qciconf->destination_mac_addr, &mac_addr_msb, TSN_REG_SIZE);
	memcpy(&tsn_qciconf->destination_mac_addr[TSN_REG_SIZE], &mac_addr_lsb, sizeof(u16));

	mac_addr_mb = mchp_tsn_get(tsn_dev, TSN_REG_SRC_MAC_MSB, TSN_MASK_MAC_MSB);
	mac_addr_lb = mchp_tsn_get(tsn_dev, TSN_REG_SRC_MAC_LSB, TSN_MASK_MAC_LSB);
	mac_addr_msb = cpu_to_be32(mac_addr_mb);
	mac_addr_lsb = cpu_to_be16(mac_addr_lb);

	dev_dbg(&tsn_dev->pdev->dev, "src mac_addr_msb :%08x\n", mac_addr_msb);
	dev_dbg(&tsn_dev->pdev->dev, "src mac_addr_lsb :%08x\n", mac_addr_lsb);

	memcpy(tsn_qciconf->source_mac_addr, &mac_addr_msb, TSN_REG_SIZE);
	memcpy(&tsn_qciconf->source_mac_addr[TSN_REG_SIZE], &mac_addr_lsb, sizeof(u16));

	tsn_conf->tsn_config_size = cpu_to_be16(sizeof(struct mchp_tsn_config_qci));

	return 0;
}

static int mchp_tsn_get_qbu_config(struct mchp_tsn_dev *tsn_dev,
				   struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_qbu *tsn_qbuconf;
	u32 preemption, addr_frag_sze;

	tsn_qbuconf = (struct mchp_tsn_config_qbu *)&tsn_conf->tsn_config_data;
	tsn_conf->cmd_status = 0;

	preemption = mchp_tsn_get(tsn_dev, TSN_REG_PREEMPT_CONTROL, TSN_MASK_PREEMPT_EN);
	addr_frag_sze = mchp_tsn_get(tsn_dev, TSN_REG_PREEMPT_CONTROL, TSN_MASK_PREEMPT_FRAG_SIZE);

	tsn_qbuconf->pre_empt_en = preemption;
	tsn_qbuconf->pre_empt_size = cpu_to_be16(addr_frag_size[addr_frag_sze]);

	tsn_conf->tsn_config_size = cpu_to_be16(sizeof(struct mchp_tsn_config_qbu));

	return 0;
}

static int mchp_tsn_get_qbv_config(struct mchp_tsn_dev *tsn_dev,
				   struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_qbv *tsn_qbvconf;
	u32 regval;
	u32 time_interval, gate_state;
	u32 cycle_time, basetimehigh0, basetimehigh1;
	u16 i, control_list_length = 0;

	tsn_qbvconf = (struct mchp_tsn_config_qbv *)&tsn_conf->tsn_config_data;
	tsn_conf->cmd_status = 0;

	tsn_qbvconf->initial_gate_state = mchp_tsn_get(tsn_dev, TSN_REG_SGSR,
						       TSN_MASK_INIT_GATE_STATE);

	tsn_qbvconf->priority_enable = mchp_tsn_get(tsn_dev, TSN_REG_PCR, TSN_MASK_PRIO_ENABLE);

	tsn_qbvconf->gate_enable = mchp_tsn_get(tsn_dev, TSN_REG_PCR, TSN_MASK_GATE_ENABLE);

	tsn_qbvconf->priority_queue_enable = mchp_tsn_get(tsn_dev, TSN_REG_PQE,
							  TSN_MASK_PRIOQ_ENABLE);
	dev_dbg(&tsn_dev->pdev->dev, "PQE %08x\n", tsn_qbvconf->priority_queue_enable);

	cycle_time = mchp_tsn_get(tsn_dev, TSN_REG_SCTR, TSN_MASK_CYCLE_TIME);
	cycle_time = gcl_time_to_ns(tsn_dev, cycle_time);
	dev_dbg(&tsn_dev->pdev->dev, "cycle time ns: %08x\n", cycle_time);

	tsn_qbvconf->cycle_time = cpu_to_be32(cycle_time);

	basetimehigh0 = mchp_tsn_get(tsn_dev, TSN_REG_BTHR0, TSN_MASK_BASETIME_HIGH);
	basetimehigh1 = mchp_tsn_get(tsn_dev, TSN_REG_BTHR1, TSN_MASK_BASETIME_HIGH);
	tsn_qbvconf->basetime_sec = cpu_to_be64((u64)basetimehigh0 | ((u64)basetimehigh1 << 32));

	regval = mchp_tsn_get(tsn_dev, TSN_REG_BTLR, TSN_MASK_BASETIME_LOW);
	tsn_qbvconf->basetime_nsec = cpu_to_be32(regval);

	tsn_qbvconf->basetime_adjust = mchp_tsn_get(tsn_dev, TSN_REG_TIME_ADJUST,
						    TSN_MASK_BASETIME_ADJUST);

	control_list_length = mchp_tsn_get(tsn_dev, TSN_REG_SCLLR, TSN_MASK_CONTROL_LIST_LENGTH);
	tsn_qbvconf->control_list_length = control_list_length;
	dev_dbg(&tsn_dev->pdev->dev, "control_list_length : %d\n", control_list_length);

	for (i = 0 ; i < MCHP_TSN_NUM_PRIO_QUEUES ; i++) {
		regval = mchp_tsn_get(tsn_dev, TSN_REG_PQ0VR + i * TSN_REG_SIZE,
				      TSN_MASK_PRIOQ_PRIO);
		tsn_qbvconf->priority_queue_prios[i] = regval;
	}
	for (i = 0 ; i < control_list_length ; i++) {
		gate_state = mchp_tsn_get(tsn_dev, TSN_REG_SGCL0ER + i * TSN_REG_SIZE,
					  TSN_MASK_GATESTATE);
		time_interval = mchp_tsn_get(tsn_dev, TSN_REG_SGCL0ER + i * TSN_REG_SIZE,
					     TSN_MASK_TIME_INTERVAL);
		time_interval = gcl_time_to_ns(tsn_dev, time_interval);
		tsn_qbvconf->gcle[i].time_interval = cpu_to_be32(time_interval);

		dev_dbg(&tsn_dev->pdev->dev, "i : %d time interval : %d gate state : %08x\n", i,
			time_interval, gate_state);
		tsn_qbvconf->gcle[i].gate_state = gate_state;
	}
	tsn_conf->tsn_config_size = cpu_to_be16(sizeof(struct mchp_tsn_config_qbv)
			+ control_list_length
			* sizeof(struct mchp_tsn_gcl_entry));

	return 0;
}

static int mchp_tsn_get_misc_rx_port_id_config(struct mchp_tsn_dev *tsn_dev,
					       struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_misc_rx_port_id *tsn_rxportconf;
	u32 port_id_rx;

	tsn_rxportconf = (struct mchp_tsn_config_misc_rx_port_id *)&tsn_conf->tsn_config_data;
	tsn_conf->cmd_status = 0;

	port_id_rx = mchp_tsn_get(tsn_dev, TSN_REG_DRXPOID, TSN_MASK_PORT_ID_RX);
	tsn_rxportconf->port_id_rx =  cpu_to_be16(port_id_rx);
	dev_dbg(&tsn_dev->pdev->dev, "destination_rx_port_id : %d\n", tsn_rxportconf->port_id_rx);

	tsn_rxportconf->port_id_rx_check = mchp_tsn_get(tsn_dev, TSN_REG_PSC,
							TSN_MASK_PORT_ID_RX_CHECK);
	dev_dbg(&tsn_dev->pdev->dev, "port_id_rx_check : %d\n", tsn_rxportconf->port_id_rx_check);
	tsn_conf->tsn_config_size = cpu_to_be16(sizeof(struct mchp_tsn_config_misc_rx_port_id));

	return 0;
}

static int mchp_tsn_get_misc_ptp_tx_prioq(struct mchp_tsn_dev *tsn_dev,
					  struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_misc_ptp_tx_prioq *tsn_ptpconf;

	tsn_ptpconf = (struct mchp_tsn_config_misc_ptp_tx_prioq *)&tsn_conf->tsn_config_data;
	tsn_conf->cmd_status = 0;

	tsn_ptpconf->ptp_tx_prioq = mchp_tsn_get(tsn_dev, TSN_REG_PTP_TX_PRIOQ,
						 TSN_MASK_PTP_TX_PRIOQ);

	dev_dbg(&tsn_dev->pdev->dev, "ptp tx prioq : %d\n", tsn_ptpconf->ptp_tx_prioq);
	tsn_conf->tsn_config_size = cpu_to_be16(sizeof(struct mchp_tsn_config_misc_ptp_tx_prioq));

	return 0;
}

static int mchp_tsn_get_misc_length_deduct_byte(struct mchp_tsn_dev *tsn_dev,
						struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_misc_length_deduct_byte *tsn_ldbconf;
	u32 ldb;

	tsn_ldbconf = (struct mchp_tsn_config_misc_length_deduct_byte *)
		       &tsn_conf->tsn_config_data;
	tsn_conf->cmd_status = 0;

	ldb = mchp_tsn_get(tsn_dev, TSN_REG_LDB, TSN_MASK_LDB);

	dev_dbg(&tsn_dev->pdev->dev, "Get length_deduct_byte : %d\n", ldb);

	tsn_ldbconf->crc_deduct_len = cpu_to_be16(ldb);
	tsn_conf->tsn_config_size = cpu_to_be16(sizeof(struct
						mchp_tsn_config_misc_length_deduct_byte));

	return 0;
}

static int mchp_tsn_set_qci_config(struct mchp_tsn_dev *tsn_dev,
				   struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_qci *tsn_qciconf;
	__be32 mac_addr_msb;
	__be16 mac_addr_lsb;

	tsn_qciconf = (struct mchp_tsn_config_qci *)&tsn_conf->tsn_config_data[0];
	tsn_conf->cmd_status = 0;

	dev_dbg(&tsn_dev->pdev->dev, "set da_check : %d\n", tsn_qciconf->da_check);
	dev_dbg(&tsn_dev->pdev->dev, "set sa_check : %d\n", tsn_qciconf->sa_check);
	if (tsn_qciconf->sa_check)
		dev_dbg(&tsn_dev->pdev->dev,
			"qciconf src mac addr : %02x:%02x:%02x:%02x:%02x:%02x\n",
			tsn_qciconf->source_mac_addr[0],
			tsn_qciconf->source_mac_addr[1],
			tsn_qciconf->source_mac_addr[2],
			tsn_qciconf->source_mac_addr[3],
			tsn_qciconf->source_mac_addr[4],
			tsn_qciconf->source_mac_addr[5]);
	if (tsn_qciconf->da_check)
		dev_dbg(&tsn_dev->pdev->dev,
			"qciconf dst mac addr : %02x:%02x:%02x:%02x:%02x:%02x\n",
			tsn_qciconf->destination_mac_addr[0],
			tsn_qciconf->destination_mac_addr[1],
			tsn_qciconf->destination_mac_addr[2],
			tsn_qciconf->destination_mac_addr[3],
			tsn_qciconf->destination_mac_addr[4],
			tsn_qciconf->destination_mac_addr[5]);

	mchp_tsn_set(tsn_dev, TSN_REG_PSC, tsn_qciconf->da_check, TSN_MASK_DA_CHECK);
	mchp_tsn_set(tsn_dev, TSN_REG_PSC, tsn_qciconf->sa_check, TSN_MASK_SA_CHECK);

	if (tsn_qciconf->da_check) {
		mac_addr_msb = *((__be32 *)tsn_qciconf->destination_mac_addr);
		mac_addr_lsb = *((__be16 *)&tsn_qciconf->destination_mac_addr[TSN_REG_SIZE]);
		dev_dbg(&tsn_dev->pdev->dev, "qciconf dst mac addr msb : %08x\n",
			be32_to_cpu(mac_addr_msb));
		dev_dbg(&tsn_dev->pdev->dev, "qciconf dst mac addr lsb : %08x\n",
			be16_to_cpu(mac_addr_lsb));
		mchp_tsn_set(tsn_dev, TSN_REG_DST_MAC_MSB, be32_to_cpu(mac_addr_msb),
			     TSN_MASK_MAC_MSB);
		mchp_tsn_set(tsn_dev, TSN_REG_DST_MAC_LSB, be16_to_cpu(mac_addr_lsb),
			     TSN_MASK_MAC_LSB);
	}

	if (tsn_qciconf->sa_check) {
		mac_addr_msb = *((__be32 *)tsn_qciconf->source_mac_addr);
		mac_addr_lsb = *((__be16 *)&tsn_qciconf->source_mac_addr[TSN_REG_SIZE]);
		dev_dbg(&tsn_dev->pdev->dev, "qciconf src mac addr msb : %08x\n",
			be32_to_cpu(mac_addr_msb));
		dev_dbg(&tsn_dev->pdev->dev, "qciconf src mac addr lsb : %08x\n",
			be16_to_cpu(mac_addr_lsb));
		mchp_tsn_set(tsn_dev, TSN_REG_SRC_MAC_MSB, be32_to_cpu(mac_addr_msb),
			     TSN_MASK_MAC_MSB);
		mchp_tsn_set(tsn_dev, TSN_REG_SRC_MAC_LSB, be16_to_cpu(mac_addr_lsb),
			     TSN_MASK_MAC_LSB);
	}
	tsn_conf->tsn_config_size = 0;

	return 0;
}

static int mchp_tsn_set_qbu_config(struct mchp_tsn_dev *tsn_dev,
				   struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_qbu *tsn_qbuconf;
	int i;
	int addr_frag_sze = -1;

	tsn_qbuconf = (struct mchp_tsn_config_qbu *)&tsn_conf->tsn_config_data;

	dev_dbg(&tsn_dev->pdev->dev, "set qbu conf, pre_empt_en : %d  preempt size : %d\n",
		tsn_qbuconf->pre_empt_en,
		be16_to_cpu(tsn_qbuconf->pre_empt_size));
	tsn_conf->cmd_status = 0;

	if (tsn_qbuconf->pre_empt_en) {
		for (i = 0 ; i < ARRAY_SIZE(addr_frag_size) ; i++) {
			if (be16_to_cpu(tsn_qbuconf->pre_empt_size)
					== addr_frag_size[i]) {
				addr_frag_sze = i;
				break;
			}
		}

		if (addr_frag_sze < 0) {
			tsn_conf->cmd_status = EINVAL;
			dev_dbg(&tsn_dev->pdev->dev, "Invalid preempt size : %d\n",
				be16_to_cpu(tsn_qbuconf->pre_empt_size));
			tsn_conf->cmd_status_string_avail = 1;
			snprintf(tsn_conf->cmd_status_string, TSN_CMD_ERR_STR_LEN,
				 "Invalid pre_empt_size : %d valid sizes are {60, 124, 188, 252}\n",
				 be16_to_cpu(tsn_qbuconf->pre_empt_size));
		}
	}

	mchp_tsn_set(tsn_dev, TSN_REG_PREEMPT_CONTROL, tsn_qbuconf->pre_empt_en,
		     TSN_MASK_PREEMPT_EN);

	if (tsn_qbuconf->pre_empt_en) {
		dev_dbg(&tsn_dev->pdev->dev, "Setting QBU conf with addr_frag_sze : %d\n",
			addr_frag_sze);
		mchp_tsn_set(tsn_dev, TSN_REG_PREEMPT_CONTROL, addr_frag_sze,
			     TSN_MASK_PREEMPT_FRAG_SIZE);
	}

	tsn_conf->tsn_config_size = 0;

	return 0;
}

static int mchp_tsn_set_qbv_config(struct mchp_tsn_dev *tsn_dev,
				   struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_qbv *tsn_qbvconf;
	u32 cycle_time;
	u32 time_interval;
	u16 i, control_list_length = 0;
	u32 prioq_enable;

	tsn_qbvconf = (struct mchp_tsn_config_qbv *)&tsn_conf->tsn_config_data;
	tsn_conf->cmd_status = 0;

	if (tsn_qbvconf->control_list_length > MAX_TSN_GCL_LEN) {
		tsn_conf->cmd_status = EINVAL;
		tsn_conf->cmd_status_string_avail = 1;
		snprintf(tsn_conf->cmd_status_string, TSN_CMD_ERR_STR_LEN,
			 "Invalid gate control list length : %d valid values are 0 to 32\n",
			 tsn_qbvconf->control_list_length);
		goto exit_invalid_control_list_length;
	}

	mchp_tsn_set(tsn_dev, TSN_REG_PCR, TSN_REG_PREEMPT_CONTROL, TSN_MASK_CONFIG_EN);
	mb(); /* Config writes should be done after cfg_val is 0 */

	mchp_tsn_set(tsn_dev, TSN_REG_PCR, tsn_qbvconf->gate_enable, TSN_MASK_GATE_ENABLE);
	mchp_tsn_set(tsn_dev, TSN_REG_PCR, tsn_qbvconf->priority_enable, TSN_MASK_PRIO_ENABLE);
	mchp_tsn_set(tsn_dev, TSN_REG_SGSR, tsn_qbvconf->initial_gate_state,
		     TSN_MASK_INIT_GATE_STATE);

	dev_dbg(&tsn_dev->pdev->dev, "Initial Gate state : %08x\n",
		tsn_qbvconf->initial_gate_state);

	dev_dbg(&tsn_dev->pdev->dev, "Gate Enable : %08x\n",
		tsn_qbvconf->gate_enable);

	dev_dbg(&tsn_dev->pdev->dev, "Priority Enable : %08x\n",
		tsn_qbvconf->priority_enable);

	prioq_enable = tsn_qbvconf->priority_queue_enable  & TSN_MASK_PRIOQ_ENABLE;
	mchp_tsn_set(tsn_dev, TSN_REG_PQE, prioq_enable, TSN_MASK_PRIOQ_ENABLE);

	dev_dbg(&tsn_dev->pdev->dev, "PQE : %08x\n", tsn_qbvconf->priority_queue_enable);

	cycle_time = be32_to_cpu(tsn_qbvconf->cycle_time);
	mchp_tsn_set(tsn_dev, TSN_REG_SCTR, ns_to_gcl_time(tsn_dev, cycle_time),
		     TSN_MASK_CYCLE_TIME);

	dev_dbg(&tsn_dev->pdev->dev, "cycletime : %08x\n", be32_to_cpu(tsn_qbvconf->cycle_time));

	mchp_tsn_set(tsn_dev, TSN_REG_BTHR0, lower_32_bits(be64_to_cpu(tsn_qbvconf->basetime_sec)),
		     TSN_MASK_BASETIME_HIGH);
	mchp_tsn_set(tsn_dev, TSN_REG_BTHR1, upper_32_bits(be64_to_cpu(tsn_qbvconf->basetime_sec)),
		     TSN_MASK_BASETIME_HIGH);

	dev_dbg(&tsn_dev->pdev->dev, "basetime sec : %llx\n",
		be64_to_cpu(tsn_qbvconf->basetime_sec));

	mchp_tsn_set(tsn_dev, TSN_REG_BTLR, be32_to_cpu(tsn_qbvconf->basetime_nsec),
		     TSN_MASK_BASETIME_LOW);

	dev_dbg(&tsn_dev->pdev->dev, "basetime ns : %08x\n",
		be32_to_cpu(tsn_qbvconf->basetime_nsec));

	control_list_length = tsn_qbvconf->control_list_length;
	mchp_tsn_set(tsn_dev, TSN_REG_SCLLR, control_list_length, TSN_MASK_CONTROL_LIST_LENGTH);
	dev_dbg(&tsn_dev->pdev->dev, "control_list_length : %d\n", control_list_length);

	mchp_tsn_set(tsn_dev, TSN_REG_TIME_ADJUST, tsn_qbvconf->basetime_adjust,
		     TSN_MASK_BASETIME_ADJUST);

	dev_dbg(&tsn_dev->pdev->dev, "basetime adjust : %08x\n", tsn_qbvconf->basetime_adjust);

	for (i = 0 ; i < MCHP_TSN_NUM_PRIO_QUEUES ; i++) {
		dev_dbg(&tsn_dev->pdev->dev, "PRIOQ : %d , Priority : %02x\n", i,
			tsn_qbvconf->priority_queue_prios[i]);

		mchp_tsn_set(tsn_dev, TSN_REG_PQ0VR + i * TSN_REG_SIZE,
			     tsn_qbvconf->priority_queue_prios[i], TSN_MASK_PRIOQ_PRIO);
	}

	for (i = 0 ; i < control_list_length ; i++) {
		dev_dbg(&tsn_dev->pdev->dev,
			"Gate : %d, gate state : %02x, gate interval : %u\n",
			i,
			tsn_qbvconf->gcle[i].gate_state,
			be32_to_cpu(tsn_qbvconf->gcle[i].time_interval));

		time_interval = ns_to_gcl_time(tsn_dev,
					       be32_to_cpu(tsn_qbvconf->gcle[i].time_interval));
		mchp_tsn_set(tsn_dev, TSN_REG_SGCL0ER + i * TSN_REG_SIZE,
			     tsn_qbvconf->gcle[i].gate_state, TSN_MASK_GATESTATE);
		mchp_tsn_set(tsn_dev, TSN_REG_SGCL0ER + i * TSN_REG_SIZE, time_interval,
			     TSN_MASK_TIME_INTERVAL);
	}

	mb(); /*  cfg_val should be set to 1 after config writes are completed */
	mchp_tsn_set(tsn_dev, TSN_REG_PCR, 1, TSN_MASK_CONFIG_EN);
exit_invalid_control_list_length:

	tsn_conf->tsn_config_size = 0;

	return 0;
}

static int mchp_tsn_set_misc_rx_port_id_config(struct mchp_tsn_dev *tsn_dev,
					       struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_misc_rx_port_id *tsn_rxportconf;

	tsn_rxportconf = (struct mchp_tsn_config_misc_rx_port_id *)&tsn_conf->tsn_config_data;
	tsn_conf->cmd_status = 0;

	dev_dbg(&tsn_dev->pdev->dev, "set port id rx check : %d\n",
		tsn_rxportconf->port_id_rx_check);
	if (tsn_rxportconf->port_id_rx_check)
		dev_dbg(&tsn_dev->pdev->dev, "set port id rx : %d\n",
			be16_to_cpu(tsn_rxportconf->port_id_rx));

	mchp_tsn_set(tsn_dev, TSN_REG_PSC, tsn_rxportconf->port_id_rx_check,
		     TSN_MASK_PORT_ID_RX_CHECK);

	if (tsn_rxportconf->port_id_rx_check)
		mchp_tsn_set(tsn_dev, TSN_REG_DRXPOID,
			     be16_to_cpu(tsn_rxportconf->port_id_rx), TSN_MASK_PORT_ID_RX);

	tsn_conf->tsn_config_size = 0;

	return 0;
}

static int mchp_tsn_set_misc_ptp_tx_prioq(struct mchp_tsn_dev *tsn_dev,
					  struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_misc_ptp_tx_prioq *tsn_ptpconf;

	tsn_ptpconf = (struct mchp_tsn_config_misc_ptp_tx_prioq *)&tsn_conf->tsn_config_data;
	tsn_conf->cmd_status = 0;

	dev_dbg(&tsn_dev->pdev->dev, "set PTP TX PRIOQ : %d\n", tsn_ptpconf->ptp_tx_prioq);

	mchp_tsn_set(tsn_dev, TSN_REG_PTP_TX_PRIOQ, tsn_ptpconf->ptp_tx_prioq,
		     TSN_MASK_PTP_TX_PRIOQ);
	tsn_conf->tsn_config_size = 0;

	return 0;
}

static int mchp_tsn_set_misc_length_deduct_byte(struct mchp_tsn_dev *tsn_dev,
						struct mchp_tsn_config_cmd_resp *tsn_conf)
{
	struct mchp_tsn_config_misc_length_deduct_byte *tsn_ldbconf;

	tsn_ldbconf = (struct mchp_tsn_config_misc_length_deduct_byte *)
		       &tsn_conf->tsn_config_data;
	tsn_conf->cmd_status = 0;

	dev_dbg(&tsn_dev->pdev->dev, "set length_deduct_byte (new): %d\n",
		be16_to_cpu(tsn_ldbconf->crc_deduct_len) & 0x7ff);

	mchp_tsn_set(tsn_dev, TSN_REG_LDB, be16_to_cpu(tsn_ldbconf->crc_deduct_len), TSN_MASK_LDB);
	tsn_conf->tsn_config_size = 0;

	return 0;
}

static int mchp_chardev_tsn_open(struct inode *inode, struct file *filp)
{
	pr_debug("mchp_tsn open with minor : %d\n", iminor(inode));
	filp->private_data = container_of(inode->i_cdev, struct mchp_tsn_dev, cdev);

	return 0;
}

static int mchp_chardev_tsn_release(struct inode *inode, struct file *filp)
{
	pr_debug("mchp_tsn release\n");

	return 0;
}

static long mchp_tsn_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mchp_tsn_config_cmd_resp config_cmd;
	struct mchp_tsn_config_cmd_resp *tsn_conf = NULL;
	struct mchp_tsn_dev *tsn_dev = NULL;
	int copy_ret;
	int ioctl_ret = 0;
	u64 tsn_dev_id;

	if (cmd != MCHP_TSN_CONFIG_CMD)
		return -EINVAL;

	tsn_dev = filp->private_data;

	if (!tsn_dev)
		return -EINVAL;

	pr_debug("mchp_tsn ioctl cmd : %08x\n", cmd);

	copy_ret = copy_from_user((void *)&config_cmd, (const void __user *)arg,
				  sizeof(config_cmd));

	tsn_dev_id = be64_to_cpu(config_cmd.tsn_dev_id);
	if (tsn_dev_id != tsn_dev->tsn_dev_id)
		return -EINVAL;

	mutex_lock(&tsn_dev->tsn_dev_mutex);
	tsn_conf = (struct mchp_tsn_config_cmd_resp *)tsn_dev->tsn_config_buff;
	/* Each device has its config buffer */
	dev_dbg(&tsn_dev->pdev->dev, "tsn_conf :%p\n", tsn_conf);
	dev_dbg(&tsn_dev->pdev->dev, "Copying from user buffer with conf size : %u\n",
		be16_to_cpu(tsn_conf->tsn_config_size));
	copy_ret = copy_from_user((void *)tsn_conf, (const void __user *)arg, sizeof(config_cmd)
				  + be16_to_cpu(config_cmd.tsn_config_size));
	dev_dbg(&tsn_dev->pdev->dev, "cmd is  %d\n", tsn_conf->cmd);
	switch (tsn_conf->cmd) {
	case MCHP_TSN_GET_QBV:
		mchp_tsn_get_qbv_config(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_GET_QBU:
		mchp_tsn_get_qbu_config(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_GET_QCI:
		mchp_tsn_get_qci_config(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_SET_QCI:
		mchp_tsn_set_qci_config(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_SET_QBU:
		mchp_tsn_set_qbu_config(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_SET_QBV:
		mchp_tsn_set_qbv_config(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_SET_MISC_RX_PORT:
		mchp_tsn_set_misc_rx_port_id_config(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_GET_MISC_RX_PORT:
		mchp_tsn_get_misc_rx_port_id_config(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_SET_MISC_PTP_TX_PRIOQ:
		mchp_tsn_set_misc_ptp_tx_prioq(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_GET_MISC_PTP_TX_PRIOQ:
		mchp_tsn_get_misc_ptp_tx_prioq(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_SET_MISC_LENGTH_DEDUCT_BYTE:
		mchp_tsn_set_misc_length_deduct_byte(tsn_dev, tsn_conf);
		break;
	case MCHP_TSN_GET_MISC_LENGTH_DEDUCT_BYTE:
		mchp_tsn_get_misc_length_deduct_byte(tsn_dev, tsn_conf);
		break;
	default:
		ioctl_ret = -EINVAL;
		break;
	}

	dev_dbg(&tsn_dev->pdev->dev, "ioctl ret : %d\n", ioctl_ret);

	if (!ioctl_ret) {
		dev_dbg(&tsn_dev->pdev->dev, "Copying to user buffer with conf size : %u\n",
			be16_to_cpu(tsn_conf->tsn_config_size));
		copy_ret = copy_to_user((void __user *)arg, tsn_conf,
					sizeof(struct mchp_tsn_config_cmd_resp)
					+ be16_to_cpu((tsn_conf->tsn_config_size)));
	}

	mutex_unlock(&tsn_dev->tsn_dev_mutex);

	if (ioctl_ret)
		dev_dbg(&tsn_dev->pdev->dev, "cmd failed with err : %d\n", ioctl_ret);

	return ioctl_ret;
}

static void qbv_gcl_mul_and_div_init(struct mchp_tsn_dev *tsn_dev)
{
	u32 denominator = NSEC_PER_SEC;
	u32 numerator;

	numerator = tsn_dev->core_clk_rate;

	tsn_dev->qbv_gcl_mul = numerator;
	tsn_dev->qbv_gcl_div = denominator;

	dev_dbg(&tsn_dev->pdev->dev, "mul : %u div : %u\n",
		tsn_dev->qbv_gcl_mul, tsn_dev->qbv_gcl_div);
}

static const struct file_operations mchp_tsn_ops = {
	.owner = THIS_MODULE,
	.open = mchp_chardev_tsn_open,
	.release = mchp_chardev_tsn_release,
	.unlocked_ioctl = mchp_tsn_unlocked_ioctl,
};

static int class_device_count(const struct class *class)
{
	struct class_dev_iter iter;
	struct device *dev;
	int count = 0;

	if (!class)
		return -EINVAL;

	class_dev_iter_init(&iter, class, NULL, NULL);
	while ((dev = class_dev_iter_next(&iter)))
		count++;

	class_dev_iter_exit(&iter);

	return count;
}

static int mchp_tsn_class_destroy(struct class *class)
{
	int count;

	if (!class)
		return -EINVAL;

	count = class_device_count(class);

	if (count == 0)
		class_destroy(class);

	return 0;
}

static int mchp_tsn_cdevice_init(struct mchp_tsn_dev *tsn_dev)
{
	struct device *classdev;
	static struct class *local_class;
	int error;
	int mchp_tsn_major;

	error = alloc_chrdev_region(&tsn_dev->dev, 0, MCHP_TSN_NUM_CHARDEVS,
				    MCHP_TSN_CHARDEV_NAME);

	if (error) {
		dev_dbg(&tsn_dev->pdev->dev, "Alloc chrdev Failed with error : %d\n", error);
		return -EINVAL;
	}

	mchp_tsn_major = MAJOR(tsn_dev->dev);
	dev_dbg(&tsn_dev->pdev->dev, "Alloc chrdev success with majorno : %d\n", mchp_tsn_major);

	cdev_init(&tsn_dev->cdev, &mchp_tsn_ops);
	error = cdev_add(&tsn_dev->cdev, tsn_dev->dev, 1);

	if (error) {
		dev_dbg(&tsn_dev->pdev->dev, "%s : Failed to add char device\n",
			tsn_dev->cdevname);
		return error;
	}

	if (!local_class) {
		local_class = class_create(MCHP_TSN_CHARDEV_CLASS_NAME);
		if (IS_ERR(local_class)) {
			dev_dbg(&tsn_dev->pdev->dev, "%s : Failed to create class\n",
				MCHP_TSN_CHARDEV_CLASS_NAME);
			return PTR_ERR(local_class);
		}
	}

	tsn_dev->class = local_class;

	classdev = device_create(tsn_dev->class, NULL, tsn_dev->dev, NULL,
				 tsn_dev->cdevname);
	if (IS_ERR(classdev)) {
		dev_dbg(&tsn_dev->pdev->dev, "%s : Failed to create device\n", tsn_dev->cdevname);
		mchp_tsn_class_destroy(tsn_dev->class);
		return PTR_ERR(classdev);
	}

	return 0;
}

static int mchp_tsn_cdevice_exit(struct mchp_tsn_dev *tsn_dev)
{
	device_destroy(tsn_dev->class, tsn_dev->dev);
	cdev_del(&tsn_dev->cdev);
	mchp_tsn_class_destroy(tsn_dev->class);
	unregister_chrdev_region(tsn_dev->dev, MCHP_TSN_NUM_CHARDEVS);

	return 0;
}

static int get_dt_node_addr(struct platform_device *pdev, u64 *nodeaddr)
{
	struct device_node *np = pdev->dev.of_node;
	const __be32 *addr;
	u64 unit_address;

	if (!np) {
		dev_err(&pdev->dev, "No device tree node found\n");
		return -ENODEV;
	}

	addr = of_get_address(np, 0, NULL, NULL);
	if (!addr) {
		dev_err(&pdev->dev, "Failed to get device node address\n");
		return -ENODEV;
	}

	unit_address = of_translate_address(np, addr);

	if (unit_address == OF_BAD_ADDR) {
		dev_err(&pdev->dev, "Failed to get device node address\n");
		return -ENODEV;
	}

	*nodeaddr = unit_address;

	return 0;
}

static int mchp_tsn_probe(struct platform_device *pdev)
{
	struct mchp_tsn_dev *tsn_dev;
	struct clk *apb, *core_clk;
	u32 alloc_size;
	int ret;
	u64 nodeaddr;

	dev_dbg(&pdev->dev, "mchp tsn probe\n");

	if (get_dt_node_addr(pdev, &nodeaddr) == -ENODEV)
		return -ENODEV;

	alloc_size = sizeof(struct mchp_tsn_dev);
	alloc_size += MAX_TSN_CONFIG_SIZE;

	tsn_dev = devm_kzalloc(&pdev->dev, alloc_size, GFP_KERNEL);
	if (!tsn_dev)
		return -ENOMEM;

	dev_dbg(&pdev->dev, "Created tsn_dev : %p\n", tsn_dev);
	tsn_dev->pdev = pdev;

	tsn_dev->tsn_reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(tsn_dev->tsn_reg_base))
		return PTR_ERR(tsn_dev->tsn_reg_base);

	tsn_dev->tsn_dev_id = nodeaddr;

	apb = devm_clk_get(&pdev->dev, "apb");
	if (IS_ERR(apb))
		return dev_err_probe(&pdev->dev, PTR_ERR(apb),
				     "could not get clock apb\n");

	core_clk = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(core_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(core_clk),
				     "could not get clock core\n");

	ret = clk_prepare_enable(apb);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable clock apb\n");

	ret = clk_prepare_enable(core_clk);
	if (ret) {
		clk_disable_unprepare(apb);
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable clock core\n");
	}

	tsn_dev->core_clk_rate = clk_get_rate(core_clk);

	dev_dbg(&tsn_dev->pdev->dev, "Clocks initialized successfully\n");

	qbv_gcl_mul_and_div_init(tsn_dev);

	mutex_init(&tsn_dev->tsn_dev_mutex);

	sprintf(tsn_dev->cdevname, "%s%llx", MCHP_TSN_CHARDEV_NAME, tsn_dev->tsn_dev_id);

	ret = mchp_tsn_cdevice_init(tsn_dev);
	if (ret) {
		clk_disable_unprepare(core_clk);
		clk_disable_unprepare(apb);
		return -EINVAL;
	}

	platform_set_drvdata(pdev, tsn_dev);

	return 0;
}

static void mchp_tsn_remove(struct platform_device *pdev)
{
	struct mchp_tsn_dev *tsn_dev = NULL;

	tsn_dev = platform_get_drvdata(pdev);
	dev_dbg(&tsn_dev->pdev->dev, "Removing tsn_dev : %p\n", tsn_dev);
	mchp_tsn_cdevice_exit(tsn_dev);

}

static const struct of_device_id mchp_tsn_match[] = {
	{ .compatible = "microchip,coretsn-rtl-v2" },
	{  }
};
MODULE_DEVICE_TABLE(of, mchp_tsn_match);

static struct platform_driver mchp_tsn_driver = {
	.probe   = mchp_tsn_probe,
	.remove  = mchp_tsn_remove,
	.driver  = {
		.name  =  MCHP_TSN_PDEV_DRV_NAME,
		.owner =  THIS_MODULE,
		.of_match_table = mchp_tsn_match,
	},
};

module_platform_driver(mchp_tsn_driver);

MODULE_AUTHOR("Pallela Venkat Karthik <pallela.karthik@microchip.com>");
MODULE_DESCRIPTION("Microchip CoreTSN driver");
MODULE_LICENSE("GPL");
