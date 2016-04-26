/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2016, Datto, Inc. All rights reserved.
 */

#ifndef	_SYS_ZIO_CRYPT_H
#define	_SYS_ZIO_CRYPT_H

#include <sys/dmu.h>
#include <sys/refcount.h>
#include <sys/crypto/api.h>
#include <sys/nvpair.h>
#include <sys/avl.h>
#include <sys/zio.h>

#define	ENCRYPT_DEBUG
#if defined(_KERNEL) && defined(ENCRYPT_DEBUG)

#define	LOG_DEBUG(fmt, args...) \
	printk(KERN_DEBUG "debug: %s: %s %d: " \
	fmt "\n", __FILE__, __FUNCTION__, __LINE__, ## args)

#define	LOG_ERROR(error, fmt, args...) \
	printk(KERN_ERR "error: %s: %s %d: " \
	fmt ": %d\n",  __FILE__, __FUNCTION__, __LINE__, ## args, error)

#define	LOG_CRYPTO_PARAMS(dcp) \
	LOG_DEBUG("cp: crypt = %llu, wkey = 0x%p", \
	(unsigned long long)(dcp)->cp_crypt, (dcp)->cp_wkey)

#define	LOG_BOOKMARK(bm) \
	LOG_DEBUG("bookmark: os = %u, obj = %u, level = %d, blkid = %u", \
	(unsigned)(bm)->zb_objset, (unsigned)(bm)->zb_object, \
	(int)(bm)->zb_level, (unsigned)(bm)->zb_blkid)

#define	dump_stack() spl_dumpstack()

#else

#define	LOG_DEBUG(fmt, args...)
#define	LOG_ERROR(error, fmt, args...)
#define	LOG_CRYPTO_PARAMS(dcp)
#define	LOG_BOOKMARK(bm)
#define	dump_stack()

#endif

/* macros defining key encryption lengths */
#define	MAX_CRYPT_KEY_LEN 32
#define	WRAPPING_KEY_LEN 32
#define	WRAPPING_IV_LEN 13
#define	WRAPPING_MAC_LEN 16
#define	ZIL_MAC_LEN 8
#define	L2ARC_MAC_LEN 8

#define	SHA_256_DIGEST_LEN 32
#define	HMAC_SHA256_KEYLEN 32

#define	ZIO_NO_ENCRYPTION_NEEDED -1

/* utility macros */
#define	BITS_TO_BYTES(x) (((x) + 7) >> 3)
#define	BYTES_TO_BITS(x) (x << 3)

/* supported commands for zfs_ioc_crypto() */
typedef enum zfs_ioc_crypto_cmd {
	ZFS_IOC_CRYPTO_CMD_NONE = 0,
	ZFS_IOC_CRYPTO_LOAD_KEY,
	ZFS_IOC_CRYPTO_UNLOAD_KEY,
	ZFS_IOC_CRYPTO_ADD_KEY,
	ZFS_IOC_CRYPTO_REWRAP,
} zfs_ioc_crypto_cmd_t;

typedef enum zio_crypt_type
{
	ZC_TYPE_NONE = 0,
	ZC_TYPE_CCM,
	ZC_TYPE_GCM
} zio_crypt_type_t;

/* table of supported crypto algorithms, modes and keylengths. */
typedef struct zio_crypt_info
{
	/* mechanism name, needed by ICP */
	crypto_mech_name_t ci_mechname;

	/* cipher mode type (GCM, CCM) */
	zio_crypt_type_t ci_crypt_type;

	/* length of the encryption key */
	size_t ci_keylen;

	/* human-readable name of the encryption alforithm */
	char *ci_name;
} zio_crypt_info_t;

extern zio_crypt_info_t zio_crypt_table[ZIO_CRYPT_FUNCTIONS];

/* physical representation of a wrapped key in the DSL Keychain */
typedef struct dsl_crypto_key_phys
{
	/* encryption algorithm (see zio_encrypt enum) */
	uint64_t dk_crypt_alg;

	/* iv / nonce for unwrapping the key */
	uint8_t dk_iv[13];

	uint8_t dk_padding[3];

	/* wrapped key data */
	uint8_t dk_keybuf[48];

	/* iv / nonce for unwrapping the dedup HMAC key */
	uint8_t dk_dd_iv[13];

	uint8_t dk_padding2[3];

	/* wrapped dedup HMAC key data */
	uint8_t dk_dd_keybuf[48];
} dsl_crypto_key_phys_t;

/* in memory representation of an unwrapped key that is loaded into memory */
typedef struct zio_crypt_key
{
	/* encryption algorithm */
	enum zio_encrypt zk_crypt;

	/* illumos crypto api key representation */
	crypto_key_t zk_key;

	/* private data for illumos crypto api */
	crypto_ctx_template_t zk_ctx_tmpl;

	/* illumos crypto api dedup key */
	crypto_key_t zk_dd_key;

	/* private data for dedup key */
	crypto_ctx_template_t zk_dd_ctx_tmpl;
} zio_crypt_key_t;

typedef struct zbookmark_phys zbookmark_phys_t;

void zio_crypt_key_destroy(zio_crypt_key_t *key);
int zio_crypt_key_init(uint64_t crypt, uint8_t *keydata, uint8_t *dd_keydata,
    zio_crypt_key_t *key);

int zio_crypt_key_wrap(crypto_key_t *cwkey, uint64_t crypt, uint8_t *keydata,
    uint8_t *dd_keydata, dsl_crypto_key_phys_t *dckp);
int zio_crypt_key_unwrap(crypto_key_t *cwkey, dsl_crypto_key_phys_t *dckp,
    uint8_t *keydata, uint8_t *dd_keydata);

int zio_crypt_generate_iv(zbookmark_phys_t *bookmark, uint64_t txgid,
    uint_t ivlen, uint8_t *ivbuf);
int zio_crypt_generate_iv_dd(zio_crypt_key_t *key, uint8_t *plainbuf,
    uint_t datalen, uint_t ivlen, uint8_t *ivbuf);

int zio_do_crypt_data(boolean_t encrypt, zio_crypt_key_t *key,
    dmu_object_type_t ot, uint8_t *iv, uint8_t *mac, uint_t datalen,
    uint8_t *plainbuf, uint8_t *cipherbuf);
#define	zio_encrypt_data(key, ot, iv, mac, datalen, pb, cb) \
    zio_do_crypt_data(B_TRUE, key, ot, iv, mac, datalen, pb, cb)
#define	zio_decrypt_data(key, ot, iv, mac, datalen, pb, cb) \
    zio_do_crypt_data(B_FALSE, key, ot, iv, mac, datalen, pb, cb)

#endif
