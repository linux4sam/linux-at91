// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC (MPFS) User Crypto driver
 *
 * Copyright (c) 2023 Microchip Corporation. All rights reserved.
 *
 * Author: Padmarao Begari <padmarao.begari@microchip.com>
 *
 */

#include <crypto/aead.h>
#include <crypto/engine.h>
#include <crypto/gcm.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <linux/crypto.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
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

#define MCHP_GCM_MAXAUTHSIZE		16

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

struct mchp_aes_gcm_ctx {
	struct mchp_crypto_ctx		base;
	u8				key[AES_MAX_KEY_SIZE];
	unsigned int			keylen;
	unsigned int			authsize;
};

struct mchp_aes_gcm_rctx {
	bool encrypt;
};

struct mchp_crypto_aes_gcm_req {
	u64 src;
	u64 iv;
	u64 key;
	u64 dst;
	u64 size;
	u64 aad;
	u64 aad_size;
	u64 tag;
	u64 tag_size;
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

static int mchp_aes_gcm_do_one_req(struct crypto_engine *engine, void *areq)
{
	struct aead_request *req = container_of(areq, struct aead_request, base);
	struct mchp_aes_gcm_rctx *rctx = aead_request_ctx(req);
	struct mchp_aes_gcm_ctx *ctx = crypto_aead_ctx(crypto_aead_reqtfm(req));
	struct device *dev = ctx->base.cryp->dev;
	struct mchp_crypto_aes_gcm_req *gcm_req;
	dma_addr_t dma_addr_data, dma_addr_gcm_req;
	unsigned int cryptlen = req->cryptlen;
	size_t iv_offset, key_offset, aad_offset, tag_offset;
	size_t dma_size;
	u8 *kbuf;
	u32 flags;
	int ret;

	/*
	 * On decryption, req->cryptlen includes the appended auth tag.
	 * Strip it so we only DMA the actual ciphertext to firmware.
	 */
	if (!rctx->encrypt)
		cryptlen -= ctx->authsize;

	iv_offset  = cryptlen;
	key_offset = iv_offset  + GCM_AES_IV_SIZE;
	aad_offset = key_offset + ctx->keylen;
	tag_offset = aad_offset + req->assoclen;

	dma_size = tag_offset + ctx->authsize;

	kbuf = dma_alloc_coherent(dev, dma_size, &dma_addr_data, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	gcm_req = dma_alloc_coherent(dev, sizeof(struct mchp_crypto_aes_gcm_req),
				     &dma_addr_gcm_req, GFP_KERNEL);
	if (!gcm_req) {
		dma_free_coherent(dev, dma_size, kbuf, dma_addr_data);
		return -ENOMEM;
	}

	/*
	 * Copy plaintext/ciphertext from src, skipping the leading AAD.
	 * For encrypt: src = [assoclen AAD][cryptlen plaintext]
	 * For decrypt: src = [assoclen AAD][cryptlen ciphertext][authsize tag]
	 */
	scatterwalk_map_and_copy(kbuf, req->src, req->assoclen, cryptlen, 0);

	if (!rctx->encrypt) {
		/* Copy the trailing auth tag from src for firmware verification */
		scatterwalk_map_and_copy(kbuf + tag_offset, req->src,
					 req->assoclen + cryptlen, ctx->authsize, 0);
	}

	memcpy(kbuf + iv_offset, req->iv, GCM_AES_IV_SIZE);
	memcpy(kbuf + key_offset, ctx->key, ctx->keylen);

	if (req->assoclen)
		scatterwalk_map_and_copy(kbuf + aad_offset, req->src, 0, req->assoclen, 0);

	gcm_req->src      = dma_addr_data;
	gcm_req->dst      = dma_addr_data;
	gcm_req->iv       = dma_addr_data + iv_offset;
	gcm_req->key      = dma_addr_data + key_offset;
	gcm_req->size     = cryptlen;
	gcm_req->aad      = dma_addr_data + aad_offset;
	gcm_req->aad_size = req->assoclen;
	gcm_req->tag      = dma_addr_data + tag_offset;
	gcm_req->tag_size = ctx->authsize;

	flags = MCHP_AES_MODE_GCM;
	if (rctx->encrypt)
		flags |= MCHP_AES_DIR_ENCRYPT;

	switch (ctx->keylen) {
	case AES_KEYSIZE_128:
		flags |= MCHP_AES_TYPE_128;
		break;
	case AES_KEYSIZE_192:
		flags |= MCHP_AES_TYPE_192;
		break;
	case AES_KEYSIZE_256:
		flags |= MCHP_AES_TYPE_256;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	ret = mchp_crypto_sbi_services(CRYPTO_SERVICE_AES,
				       dma_addr_gcm_req, flags);
	if (ret) {
		if (!rctx->encrypt && ret == -EINVAL)
			ret = -EBADMSG;
		goto out;
	}

	if (rctx->encrypt) {
		scatterwalk_map_and_copy(kbuf, req->dst, req->assoclen,
					 cryptlen, 1);
		scatterwalk_map_and_copy(kbuf + tag_offset, req->dst, req->assoclen + cryptlen,
					 ctx->authsize, 1);
	} else {
		/*
		 * Decryption: firmware verified the tag and decrypted in-place.
		 * Copy recovered plaintext at offset assoclen in dst.
		 */
		scatterwalk_map_and_copy(kbuf, req->dst, req->assoclen,
					 cryptlen, 1);
	}

out:
	memzero_explicit(gcm_req, sizeof(struct mchp_crypto_aes_gcm_req));
	dma_free_coherent(dev, sizeof(struct mchp_crypto_aes_gcm_req),
			  gcm_req, dma_addr_gcm_req);

	memzero_explicit(kbuf, dma_size);
	dma_free_coherent(dev, dma_size, kbuf, dma_addr_data);

	crypto_finalize_aead_request(engine, req, ret);

	return 0;
}

static int mchp_aes_gcm_encrypt(struct aead_request *req)
{
	struct mchp_aes_gcm_rctx *rctx = aead_request_ctx(req);
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct mchp_aes_gcm_ctx *ctx = crypto_aead_ctx(tfm);

	rctx->encrypt = true;

	return crypto_transfer_aead_request_to_engine(ctx->base.cryp->engine, req);
}

static int mchp_aes_gcm_decrypt(struct aead_request *req)
{
	struct mchp_aes_gcm_rctx *rctx = aead_request_ctx(req);
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct mchp_aes_gcm_ctx *ctx = crypto_aead_ctx(tfm);

	/* req->cryptlen includes the appended auth tag on decryption */
	if (req->cryptlen < ctx->authsize)
		return -EINVAL;

	rctx->encrypt = false;

	return crypto_transfer_aead_request_to_engine(ctx->base.cryp->engine, req);
}

static int mchp_aes_gcm_setkey(struct crypto_aead *tfm, const u8 *key,
			       unsigned int keylen)
{
	struct mchp_aes_gcm_ctx *ctx = crypto_aead_ctx(tfm);

	if (!key || !keylen)
		return -EINVAL;

	if (keylen != AES_KEYSIZE_128 &&
	    keylen != AES_KEYSIZE_192 &&
	    keylen != AES_KEYSIZE_256) {
		memzero_explicit(ctx->key, sizeof(ctx->key));
		ctx->keylen = 0;
		return -EINVAL;
	}

	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;

	return 0;
}

static int mchp_aes_gcm_setauthsize(struct crypto_aead *tfm,
				    unsigned int authsize)
{
	struct mchp_aes_gcm_ctx *ctx = crypto_aead_ctx(tfm);
	int ret;

	ret = crypto_gcm_check_authsize(authsize);
	if (ret)
		return ret;

	ctx->authsize = authsize;

	return 0;
}

static int mchp_aes_gcm_init_tfm(struct crypto_aead *tfm)
{
	struct mchp_aes_gcm_ctx *ctx = crypto_aead_ctx(tfm);

	ctx->base.cryp = mchp_crypto_find_dev(&ctx->base);
	if (!ctx->base.cryp)
		return -ENODEV;

	ctx->authsize = MCHP_GCM_MAXAUTHSIZE;

	crypto_aead_set_reqsize(tfm, sizeof(struct mchp_aes_gcm_rctx));

	return 0;
}

static void mchp_aes_gcm_exit_tfm(struct crypto_aead *tfm)
{
	struct mchp_aes_gcm_ctx *ctx = crypto_aead_ctx(tfm);

	memzero_explicit(ctx->key, sizeof(ctx->key));
}

static struct aead_engine_alg mchp_aes_gcm_alg = {
	.base.base = {
		.cra_name		= "gcm(aes)",
		.cra_driver_name	= "microchip-gcm-aes",
		.cra_priority		= 300,
		.cra_flags		= CRYPTO_ALG_ASYNC,
		.cra_blocksize		= 1,
		.cra_ctxsize		= sizeof(struct mchp_aes_gcm_ctx),
		.cra_alignmask		= 0,
		.cra_module		= THIS_MODULE,
	},
	.base.init		= mchp_aes_gcm_init_tfm,
	.base.exit		= mchp_aes_gcm_exit_tfm,
	.base.setkey		= mchp_aes_gcm_setkey,
	.base.setauthsize	= mchp_aes_gcm_setauthsize,
	.base.encrypt		= mchp_aes_gcm_encrypt,
	.base.decrypt		= mchp_aes_gcm_decrypt,
	.base.ivsize		= GCM_AES_IV_SIZE,
	.base.maxauthsize	= MCHP_GCM_MAXAUTHSIZE,
	.op.do_one_request	= mchp_aes_gcm_do_one_req,
};

int mchp_aes_gcm_register_algs(struct mchp_crypto_dev *cryp)
{
	if (!(cryp->crypto->cipher_algo & CRYPTO_ALG_AES_GCM))
		return 0;

	return crypto_engine_register_aead(&mchp_aes_gcm_alg);
}

void mchp_aes_gcm_unregister_algs(struct mchp_crypto_dev *cryp)
{
	if (!(cryp->crypto->cipher_algo & CRYPTO_ALG_AES_GCM))
		return;

	crypto_engine_unregister_aead(&mchp_aes_gcm_alg);
}
