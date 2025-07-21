// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC (MPFS) User Crypto driver
 *
 * Copyright (c) 2023 Microchip Corporation. All rights reserved.
 *
 * Author: Padmarao Begari <padmarao.begari@microchip.com>
 *
 */

#include <crypto/engine.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <linux/crypto.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/sbi.h>
#include "mchp-crypto-sbi.h"

#define MCHP_AES_DIR_DECRYPT		0x00
#define MCHP_AES_DIR_ENCRYPT		0x01

#define MCHP_AES_MODE_ECB		0x0100
#define MCHP_AES_MODE_CBC		0x0200
#define MCHP_AES_MODE_CFB		0x0300
#define MCHP_AES_MODE_OFB		0x0400
#define MCHP_AES_MODE_CTR		0x0500
#define MCHP_AES_MODE_GCM		0x0600
#define MCHP_AES_MODE_CCM		0x0700
#define MCHP_AES_MODE_GHASH		0x0800
#define MCHP_AES_MODE_MASK		0x0F00

#define MCHP_AES_TYPE_128		0x010000
#define MCHP_AES_TYPE_192		0x020000
#define MCHP_AES_TYPE_256		0x030000

struct mchp_crypto_aes_algo {
	u64 algonum;
	struct skcipher_engine_alg algo;
};

struct mchp_crypto_aes_req {
	u64 src;
	u64 iv;
	u64 key;
	u64 dst;
	u64 size;
};

static int mchp_aes_do_one_req(struct crypto_engine *engine, void *areq)
{
	struct skcipher_request *req =
			container_of(areq, struct skcipher_request, base);
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct mchp_crypto_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct device *dev = ctx->cryp->dev;
	struct mchp_crypto_aes_req *aes_req;
	dma_addr_t dma_addr_data, dma_addr_aes_req;
	unsigned int iv_size;
	unsigned int data_size;
	size_t dma_size;
	char *kbuf;
	int ret;

	switch (ctx->keylen) {
	case AES_KEYSIZE_128:
		ctx->flags |= MCHP_AES_TYPE_128;
		break;
	case AES_KEYSIZE_192:
		ctx->flags |= MCHP_AES_TYPE_192;
		break;
	case AES_KEYSIZE_256:
		ctx->flags |= MCHP_AES_TYPE_256;
		break;
	default:
		return -EINVAL;
	}

	iv_size = crypto_skcipher_ivsize(tfm);

	dma_size = req->cryptlen + ctx->keylen + iv_size;

	kbuf = dma_alloc_coherent(dev, dma_size, &dma_addr_data, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	aes_req = dma_alloc_coherent(dev, sizeof(struct mchp_crypto_aes_req),
				     &dma_addr_aes_req, GFP_KERNEL);
	if (!aes_req) {
		dma_free_coherent(dev, dma_size, kbuf, dma_addr_data);
		return -ENOMEM;
	}

	sg_copy_to_buffer(req->src, sg_nents(req->src),
			  kbuf, req->cryptlen);
	data_size = req->cryptlen;
	memcpy(kbuf + data_size, req->iv, iv_size);
	memcpy(kbuf + data_size + iv_size, ctx->key, ctx->keylen);

	aes_req->src = dma_addr_data;
	aes_req->dst = dma_addr_data;
	aes_req->iv = aes_req->src + data_size;
	aes_req->size = data_size;
	aes_req->key = aes_req->src + data_size + iv_size;

	ret = mchp_crypto_sbi_services(CRYPTO_SERVICE_AES,
				     dma_addr_aes_req, ctx->flags);
	if (!ret)
		sg_copy_from_buffer(req->dst, sg_nents(req->dst),
				    kbuf, data_size);

	memzero_explicit(kbuf, dma_size);
	dma_free_coherent(dev, dma_size, kbuf, dma_addr_data);

	memzero_explicit(aes_req, sizeof(struct mchp_crypto_aes_req));
	dma_free_coherent(dev, sizeof(struct mchp_crypto_aes_req),
			  aes_req, dma_addr_aes_req);

	crypto_finalize_skcipher_request(engine, req, ret);

	return ret;
}

static int mchp_aes_crypt(struct skcipher_request *req, unsigned long flags)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct mchp_crypto_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct mchp_crypto_dev *cryp = ctx->cryp;
	unsigned int blocksize_align = crypto_skcipher_blocksize(tfm) - 1;

	ctx->flags = flags;

	if ((ctx->flags & MCHP_AES_MODE_MASK) == MCHP_AES_MODE_ECB ||
	    (ctx->flags & MCHP_AES_MODE_MASK) == MCHP_AES_MODE_CBC)
		if (req->cryptlen & blocksize_align)
			return -EINVAL;

	return crypto_transfer_skcipher_request_to_engine(cryp->engine, req);
}

static int mchp_aes_ecb_encrypt(struct skcipher_request *req)
{
	return mchp_aes_crypt(req, MCHP_AES_MODE_ECB | MCHP_AES_DIR_ENCRYPT);
}

static int mchp_aes_ecb_decrypt(struct skcipher_request *req)
{
	return mchp_aes_crypt(req, MCHP_AES_MODE_ECB);
}

static int mchp_aes_cbc_encrypt(struct skcipher_request *req)
{
	return mchp_aes_crypt(req, MCHP_AES_MODE_CBC | MCHP_AES_DIR_ENCRYPT);
}

static int mchp_aes_cbc_decrypt(struct skcipher_request *req)
{
	return mchp_aes_crypt(req, MCHP_AES_MODE_CBC);
}

static int mchp_aes_cfb_encrypt(struct skcipher_request *req)
{
	return mchp_aes_crypt(req, MCHP_AES_MODE_CFB | MCHP_AES_DIR_ENCRYPT);
}

static int mchp_aes_cfb_decrypt(struct skcipher_request *req)
{
	return mchp_aes_crypt(req, MCHP_AES_MODE_CFB);
}

static int mchp_aes_ofb_encrypt(struct skcipher_request *req)
{
	return mchp_aes_crypt(req, MCHP_AES_MODE_OFB | MCHP_AES_DIR_ENCRYPT);
}

static int mchp_aes_ofb_decrypt(struct skcipher_request *req)
{
	return mchp_aes_crypt(req, MCHP_AES_MODE_OFB);
}

static int mchp_aes_ctr_encrypt(struct skcipher_request *req)
{
	return mchp_aes_crypt(req, MCHP_AES_MODE_CTR | MCHP_AES_DIR_ENCRYPT);
}

static int mchp_aes_ctr_decrypt(struct skcipher_request *req)
{
	return mchp_aes_crypt(req, MCHP_AES_MODE_CTR);
}

static int mchp_aes_setkey(struct crypto_skcipher *tfm, const u8 *key,
			   unsigned int keylen)
{
	struct mchp_crypto_ctx *ctx = crypto_skcipher_ctx(tfm);

	if (!key || !keylen)
		return -EINVAL;

	if (keylen != AES_KEYSIZE_256 &&
	    keylen != AES_KEYSIZE_192 &&
	    keylen != AES_KEYSIZE_128)
		return -EINVAL;

	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;

	return 0;
}

static int mchp_aes_init_tfm(struct crypto_skcipher *tfm)
{
	struct mchp_crypto_ctx *ctx = crypto_skcipher_ctx(tfm);

	ctx->cryp = mchp_crypto_find_dev(ctx);
	if (!ctx->cryp)
		return -ENODEV;

	crypto_skcipher_set_reqsize(tfm, sizeof(struct mchp_crypto_ctx) +
				    sizeof(struct skcipher_request));

	return 0;
}

static struct mchp_crypto_aes_algo mchp_aes_algs[] = {
{
	.algonum = CRYPTO_ALG_AES_ECB,
	.algo.base = {
		.base.cra_name		= "ecb(aes)",
		.base.cra_driver_name	= "microchip-ecb-aes",
		.base.cra_priority	= 300,
		.base.cra_flags		= CRYPTO_ALG_ASYNC,
		.base.cra_blocksize	= AES_BLOCK_SIZE,
		.base.cra_ctxsize	= sizeof(struct mchp_crypto_ctx),
		.base.cra_alignmask	= 0xf,
		.base.cra_module	= THIS_MODULE,

		.init			= mchp_aes_init_tfm,
		.setkey			= mchp_aes_setkey,
		.encrypt		= mchp_aes_ecb_encrypt,
		.decrypt		= mchp_aes_ecb_decrypt,
		.min_keysize		= AES_MIN_KEY_SIZE,
		.max_keysize		= AES_MAX_KEY_SIZE,
	},
	.algo.op = {
		.do_one_request = mchp_aes_do_one_req
	},
}, {
	.algonum = CRYPTO_ALG_AES_CBC,
	.algo.base = {
		.base.cra_name		= "cbc(aes)",
		.base.cra_driver_name	= "microchip-cbc-aes",
		.base.cra_priority	= 300,
		.base.cra_flags		= CRYPTO_ALG_ASYNC,
		.base.cra_blocksize	= AES_BLOCK_SIZE,
		.base.cra_ctxsize	= sizeof(struct mchp_crypto_ctx),
		.base.cra_alignmask	= 0xf,
		.base.cra_module	= THIS_MODULE,

		.init			= mchp_aes_init_tfm,
		.setkey			= mchp_aes_setkey,
		.encrypt		= mchp_aes_cbc_encrypt,
		.decrypt		= mchp_aes_cbc_decrypt,
		.min_keysize		= AES_MIN_KEY_SIZE,
		.max_keysize		= AES_MAX_KEY_SIZE,
		.ivsize			= AES_BLOCK_SIZE,
	},
	.algo.op = {
		.do_one_request = mchp_aes_do_one_req
	},
}, {
	.algonum = CRYPTO_ALG_AES_OFB,
	.algo.base = {
		.base.cra_name		= "ofb(aes)",
		.base.cra_driver_name	= "microchip-ofb-aes",
		.base.cra_priority	= 300,
		.base.cra_flags		= CRYPTO_ALG_ASYNC,
		.base.cra_blocksize	= 1,
		.base.cra_ctxsize	= sizeof(struct mchp_crypto_ctx),
		.base.cra_alignmask	= 0xf,
		.base.cra_module	= THIS_MODULE,

		.init			= mchp_aes_init_tfm,
		.setkey			= mchp_aes_setkey,
		.encrypt		= mchp_aes_ofb_encrypt,
		.decrypt		= mchp_aes_ofb_decrypt,
		.min_keysize		= AES_MIN_KEY_SIZE,
		.max_keysize		= AES_MAX_KEY_SIZE,
		.ivsize			= AES_BLOCK_SIZE,
	},
	.algo.op = {
		.do_one_request = mchp_aes_do_one_req
	},
}, {
	.algonum = CRYPTO_ALG_AES_CFB,
	.algo.base = {
		.base.cra_name		= "cfb(aes)",
		.base.cra_driver_name	= "microchip-cfb-aes",
		.base.cra_priority	= 300,
		.base.cra_flags		= CRYPTO_ALG_ASYNC,
		.base.cra_blocksize	= 1,
		.base.cra_ctxsize	= sizeof(struct mchp_crypto_ctx),
		.base.cra_alignmask	= 0xf,
		.base.cra_module	= THIS_MODULE,

		.init			= mchp_aes_init_tfm,
		.setkey			= mchp_aes_setkey,
		.encrypt		= mchp_aes_cfb_encrypt,
		.decrypt		= mchp_aes_cfb_decrypt,
		.min_keysize		= AES_MIN_KEY_SIZE,
		.max_keysize		= AES_MAX_KEY_SIZE,
		.ivsize			= AES_BLOCK_SIZE,
	},
	.algo.op = {
		.do_one_request = mchp_aes_do_one_req
	},
}, {
	.algonum = CRYPTO_ALG_AES_CTR,
	.algo.base = {
		.base.cra_name		= "ctr(aes)",
		.base.cra_driver_name	= "microchip-ctr-aes",
		.base.cra_priority	= 300,
		.base.cra_flags		= CRYPTO_ALG_ASYNC,
		.base.cra_blocksize	= 1,
		.base.cra_ctxsize	= sizeof(struct mchp_crypto_ctx),
		.base.cra_alignmask	= 0xf,
		.base.cra_module	= THIS_MODULE,

		.init			= mchp_aes_init_tfm,
		.setkey			= mchp_aes_setkey,
		.encrypt		= mchp_aes_ctr_encrypt,
		.decrypt		= mchp_aes_ctr_decrypt,
		.min_keysize		= AES_MIN_KEY_SIZE,
		.max_keysize		= AES_MAX_KEY_SIZE,
		.ivsize			= AES_BLOCK_SIZE,
	},
	.algo.op = {
		.do_one_request = mchp_aes_do_one_req
	},
},
};

int mchp_aes_register_algs(struct mchp_crypto_dev *cryp)
{
	for (int i = 0; i < ARRAY_SIZE(mchp_aes_algs); i++) {
		u64 algonum = mchp_aes_algs[i].algonum;
		int ret;

		if (!(algonum & cryp->crypto->cipher_algo))
			continue;

		ret = crypto_engine_register_skcipher(&mchp_aes_algs[i].algo);
		if (ret)
			return ret;
	}

	return 0;
}

void mchp_aes_unregister_algs(struct mchp_crypto_dev *cryp)
{
	for (int i = 0; i < ARRAY_SIZE(mchp_aes_algs); i++) {
		u64 algonum = mchp_aes_algs[i].algonum;

		if (!(algonum & cryp->crypto->cipher_algo))
			continue;

		crypto_engine_unregister_skcipher(&mchp_aes_algs[i].algo);
	}
}
