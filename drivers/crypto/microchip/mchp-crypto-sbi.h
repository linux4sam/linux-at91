// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC (MPFS) User Crypto driver
 *
 * Copyright (c) 2023 Microchip Corporation. All rights reserved.
 *
 * Author: Padmarao Begari <padmarao.begari@microchip.com>
 *
 */

#include <asm/sbi.h>
#include <asm/vendorid_list.h>
#include <crypto/aes.h>
#include <linux/bits.h>

#define SBI_EXT_MICROCHIP_TECHNOLOGY		(SBI_EXT_VENDOR_START | \
						 MICROCHIP_VENDOR_ID)

#define MICROCHIP_SBI_EXT_CRYPTO_INIT			0x11
#define MICROCHIP_SBI_EXT_CRYPTO_SERVICES_PROBE		0x12
#define MICROCHIP_SBI_EXT_CRYPTO_SERVICES		0x13

struct mchp_crypto_info {
	u32 services;
#define CRYPTO_SERVICE_AES		BIT(0)
#define CRYPTO_SERVICE_AEAD		BIT(1)
#define CRYPTO_SERVICE_HASH		BIT(2)
#define CRYPTO_SERVICE_MAC		BIT(3)
#define CRYPTO_SERVICE_RSA		BIT(4)
#define CRYPTO_SERVICE_DSA		BIT(5)
#define CRYPTO_SERVICE_ECDSA		BIT(6)
#define CRYPTO_SERVICE_NRBG		BIT(7)
	u64 cipher_algo;
#define CRYPTO_ALG_AES_ECB		BIT(0)
#define CRYPTO_ALG_AES_CBC		BIT(1)
#define CRYPTO_ALG_AES_OFB		BIT(2)
#define CRYPTO_ALG_AES_CFB		BIT(3)
#define CRYPTO_ALG_AES_CTR		BIT(4)
	u32 aead_algo;
#define CRYPTO_ALG_AEAD_GCM		BIT(0)
#define CRYPTO_ALG_AEAD_CCM		BIT(1)
	u32 hash_algo;
#define CRYPTO_ALG_HASH_SHA1		BIT(0)
#define CRYPTO_ALG_HASH_SHA224		BIT(1)
#define CRYPTO_ALG_HASH_SHA256		BIT(2)
#define CRYPTO_ALG_HASH_SHA384		BIT(3)
#define CRYPTO_ALG_HASH_SHA512		BIT(4)
#define CRYPTO_ALG_HASH_SHA512_224	BIT(5)
#define CRYPTO_ALG_HASH_SHA512_256	BIT(6)
	u64 mac_algo;
#define CRYPTO_ALG_MAC_SHA1		BIT(0)
#define CRYPTO_ALG_MAC_SHA224		BIT(1)
#define CRYPTO_ALG_MAC_SHA256		BIT(2)
#define CRYPTO_ALG_MAC_SHA384		BIT(3)
#define CRYPTO_ALG_MAC_SHA512		BIT(4)
#define CRYPTO_ALG_MAC_SHA512_224	BIT(5)
#define CRYPTO_ALG_MAC_SHA512_256	BIT(6)
#define CRYPTO_ALG_MAC_AESCMAC128	BIT(10)
#define CRYPTO_ALG_MAC_AESCMAC192	BIT(11)
#define CRYPTO_ALG_MAC_AESCMAC256	BIT(12)
#define CRYPTO_ALG_MAC_AESGMAC		BIT(13)
	u64 akcipher_algo;
#define CRYPTO_ALG_DSA			BIT(0)
#define CRYPTO_ALG_RSA			BIT(1)
#define CRYPTO_ALG_DH			BIT(2)
#define CRYPTO_ALG_ECDSA		BIT(3)
#define CRYPTO_ALG_ECDH			BIT(4)
	u32 nrbg_algo;
};

struct mchp_crypto_ctx {
	struct mchp_crypto_dev		*cryp;
	u8				key[AES_MAX_KEY_SIZE];
	int				keylen;
	unsigned long			flags;
};

struct mchp_crypto_dev {
	struct list_head		list;
	struct device			*dev;
	struct crypto_engine		*engine;
	struct mchp_crypto_info		*crypto;
};

struct mchp_crypto_dev *mchp_crypto_find_dev(struct mchp_crypto_ctx *ctx);

int mchp_crypto_sbi_services(u32 service, u64 crypto_addr, u32 flags);

int mchp_aes_register_algs(struct mchp_crypto_dev *cryp);
void mchp_aes_unregister_algs(struct mchp_crypto_dev *cryp);
