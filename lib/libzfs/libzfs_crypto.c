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
 * Copyright 2016 Datto, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/zfs_context.h>
#include <sys/fs/zfs.h>
#include <sys/dsl_keychain.h>
#include <sys/crypto/icp.h>
#include <libintl.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <libzfs.h>
#include "libzfs_impl.h"
#include "zfeature_common.h"

typedef enum key_format {
	KEY_FORMAT_NONE = 0,
	KEY_FORMAT_RAW,
	KEY_FORMAT_HEX,
	KEY_FORMAT_PASSPHRASE
} key_format_t;

typedef enum key_locator {
	KEY_LOCATOR_NONE,
	KEY_LOCATOR_PROMPT,
	KEY_LOCATOR_URI
} key_locator_t;

#define	MIN_PASSPHRASE_LEN 8
#define	MAX_PASSPHRASE_LEN 64
#define	PBKDF2_ITERATIONS 1000

static int caught_interrupt;

static int
parse_format(key_format_t *format, char *s, int len)
{
	if (strncmp("raw", s, len) == 0 && len == 3)
		*format = KEY_FORMAT_RAW;
	else if (strncmp("hex", s, len) == 0 && len == 3)
		*format = KEY_FORMAT_HEX;
	else if (strncmp("passphrase", s, len) == 0 && len == 10)
		*format = KEY_FORMAT_PASSPHRASE;
	else
		return (EINVAL);

	return (0);
}

static int
parse_locator(key_locator_t *locator, char *s, int len, char **uri)
{
	if (len == 6 && strncmp("prompt", s, 6) == 0) {
		*locator = KEY_LOCATOR_PROMPT;
		return (0);
	}

	/* uri can currently only be an absolut file path */
	if (len > 8 && strncmp("file:///", s, 8) == 0) {
		*locator = KEY_LOCATOR_URI;
		*uri = s;
		return (0);
	}

	return (EINVAL);
}

static int
keysource_prop_parser(char *keysource, key_format_t *format,
	key_locator_t *locator, char **uri)
{
	int len, ret;
	int keysource_len = strlen(keysource);
	char *s = keysource;

	*format = KEY_FORMAT_NONE;
	*locator = KEY_LOCATOR_NONE;

	if (keysource_len > ZPOOL_MAXPROPLEN)
		return (EINVAL);

	for (len = 0; len < keysource_len; len++)
		if (s[len] == ',')
			break;

	/* If we are at the end of the key property, there is a problem */
	if (len == keysource_len)
		return (EINVAL);

	ret = parse_format(format, s, len);
	if (ret)
		return (ret);

	s = s + len + 1;
	len = keysource_len - len - 1;
	ret = parse_locator(locator, s, len, uri);

	return (ret);
}

static int
hex_key_to_raw(char *hex, int hexlen, uint8_t *out)
{
	int ret, i;
	unsigned int c;

	for (i = 0; i < hexlen; i += 2) {
		ret = sscanf(&hex[i], "%02x", &c);
		if (ret != 1) {
			ret = EINVAL;
			goto error;
		}

		out[i / 2] = c;
	}

	return (0);

error:
	return (ret);
}


static void
catch_signal(int sig)
{
	caught_interrupt = sig;
}

static char *
get_format_prompt_string(key_format_t format)
{
	switch(format) {
	case KEY_FORMAT_RAW:
		return "raw key";
	case KEY_FORMAT_HEX:
		return "hex key";
	case KEY_FORMAT_PASSPHRASE:
		return "passphrase";
	default:
		/* shouldn't happen */
		return NULL;
	}

}

static int
get_key_material_raw(FILE *fd, const char *fsname, key_format_t format,
    uint8_t *buf, size_t buf_len, boolean_t again, size_t *len_out)
{
	int ret = 0, bytes;
	char c;
	struct termios old_term, new_term;
	struct sigaction act, osigint, osigtstp;

	*len_out = 0;

	if (isatty(fileno(fd))) {
		/* handle SIGINT and ignore SIGSTP. This is necessary to
		 * restore the state of the terminal.
		 */
		caught_interrupt = 0;
		act.sa_flags = 0;
		(void) sigemptyset(&act.sa_mask);
		act.sa_handler = catch_signal;

		(void) sigaction(SIGINT, &act, &osigint);
		act.sa_handler = SIG_IGN;
		(void) sigaction(SIGTSTP, &act, &osigtstp);

		/* prompt for the passphrase */
		if (fsname) {
			(void) printf("%s %s for '%s': ",
			    (!again) ? "Enter" : "Renter",
			    get_format_prompt_string(format), fsname);
		} else {
			(void) printf("%s %s: ",
			    (!again) ? "Enter" : "Renter",
			    get_format_prompt_string(format));

		}
		(void) fflush(stdout);

		/* disable the terminal echo for passphrase input*/
		(void) tcgetattr(fileno(fd), &old_term);

		new_term = old_term;
		new_term.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);

		ret = tcsetattr(fileno(fd), TCSAFLUSH, &new_term);
		if (ret) {
			ret = errno;
			errno = 0;
			goto out;
		}
	}

	/* read the key material */
	if (format == KEY_FORMAT_PASSPHRASE) {
		bytes = getline((char **)&buf, &buf_len, fd);
		if (bytes < 0) {
			ret = errno;
			errno = 0;
			goto out;
		}

		/* trim the ending newline if it exists */
		if(buf[bytes - 1] == '\n')
			buf[bytes - 1] = '\0';
	} else {
		bytes = read(fileno(fd), buf, buf_len);
		if (bytes < 0) {
			ret = errno;
			errno = 0;
			goto out;
		}

		/* clean off the newline from stdin if needed */
		if (isatty(fileno(fd)))
			while ((c = getc(fd)) != '\n' && c != EOF);
	}
	*len_out = bytes;

out:
	if (isatty(fileno(fd))) {
		/* reset the teminal */
		(void) tcsetattr(fileno(fd), TCSAFLUSH, &old_term);
		(void) sigaction(SIGINT, &osigint, NULL);
		(void) sigaction(SIGTSTP, &osigtstp, NULL);

		/* if we caught a signal, re-throw it now */
		if (caught_interrupt != 0) {
			(void) kill(getpid(), caught_interrupt);
		}

		/* print the newline that was not echo'ed */
		printf("\n");
	}

	return (ret);

}

static int
get_key_material(libzfs_handle_t *hdl, boolean_t do_verify, key_format_t format,
    key_locator_t locator, char *uri, const char *fsname, uint8_t **km_out,
    size_t *kmlen_out)
{
	int ret;
	FILE *fd = NULL;
	uint8_t *km = NULL, *km2 = NULL;
	size_t buflen, kmlen, kmlen2;

	/* open the appropriate file descriptor */
	switch (locator) {
	case KEY_LOCATOR_PROMPT:
		fd = stdin;
		break;
	case KEY_LOCATOR_URI:
		fd = fopen(&uri[7], "r");
		if (!fd) {
			ret = errno;
			errno = 0;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Failed to open key material file"));
			goto error;
		}
		break;
	default:
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Invalid key locator."));
		goto error;
	}

	/* allocate memory for the key material */
	switch (format) {
	case KEY_FORMAT_RAW:
		buflen = WRAPPING_KEY_LEN;
		break;
	case KEY_FORMAT_HEX:
		buflen = WRAPPING_KEY_LEN * 2;
		break;
	case KEY_FORMAT_PASSPHRASE:
		buflen = MAX_PASSPHRASE_LEN;
		break;
	default:
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Invalid key format."));
		goto error;
	}

	km = zfs_alloc(hdl, buflen);
	if (!km) {
		ret = ENOMEM;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Failed to allocate memory for key material."));
		goto error;
	}

	/* fetch the key material into the buffer */
	ret = get_key_material_raw(fd, fsname, format, km, buflen,
	    B_FALSE, &kmlen);
	if (ret)
		goto error;

	/* do basic validation of the key material */
	switch (format) {
	case KEY_FORMAT_RAW:
	case KEY_FORMAT_HEX:
		/* just verify the key length is correct */
		if (kmlen != buflen) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Invalid key length."));
			goto error;
		}
		break;
	case KEY_FORMAT_PASSPHRASE:
		/* just verify the keylength is correct */
		if (kmlen > MAX_PASSPHRASE_LEN) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Passphrase too long (max 64)."));
			goto error;
		}

		if (kmlen < MIN_PASSPHRASE_LEN) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Passphrase too short (min 8)."));
			goto error;
		}

		if (do_verify) {
			/* prompt for the key again to make sure it is valid */
			km2 = zfs_alloc(hdl, buflen);
			if (!km2) {
				ret = ENOMEM;
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "Failed to allocate memory "
				    "for key material."));
				goto error;
			}

			ret = get_key_material_raw(fd, fsname, format, km2,
			    buflen, B_TRUE, &kmlen2);
			if (ret)
				goto error;

			if (kmlen2 != kmlen ||
			    (strncmp((char *)km, (char *)km2, kmlen) != 0)) {
				ret = EINVAL;
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "Passphrases do not match."));
				goto error;
			}
		}
		break;
	default:
		/* can't happen */
		break;
	}


	if (fd != stdin)
		fclose(fd);

	if (km2)
		free(km2);

	*km_out = km;
	*kmlen_out = kmlen;
	return (0);

error:
	if (km)
		free(km);

	if (km2)
		free(km2);

	if (fd && fd != stdin)
		fclose(fd);

	*km_out = NULL;
	*kmlen_out = 0;
	return (ret);
}

static int
pbkdf2(uint8_t *passphrase, size_t passphraselen, uint8_t *salt,
    size_t saltlen, uint32_t iterations, uint8_t *output,
    size_t outputlen)
{
	int ret;
	uint32_t blockptr, i, iter;
	uint16_t hmac_key_len;
	uint8_t *hmac_key;
	uint8_t block[SHA_256_DIGEST_LEN * 2];
	uint8_t *hmacresult = block + SHA_256_DIGEST_LEN;
	crypto_mechanism_t mech;
	crypto_key_t key;
	crypto_data_t in_data, out_data;
	crypto_ctx_template_t tmpl = NULL;

	/* initialize output */
	memset(output, 0, outputlen);

	/* initialize icp for use */
	thread_init();
	icp_init();

	/* HMAC key size is max(sizeof(uint32_t) + salt len, sha 256 len) */
	if (saltlen > SHA_256_DIGEST_LEN) {
		hmac_key_len = saltlen + sizeof(uint32_t);
	} else {
		hmac_key_len = SHA_256_DIGEST_LEN;
	}

	hmac_key = calloc(hmac_key_len, 1);
	if (!hmac_key) {
		ret = ENOMEM;
		goto error;
	}

	/* initialize sha 256 hmac mechanism */
	mech.cm_type = crypto_mech2id(SUN_CKM_SHA256_HMAC);
	mech.cm_param = NULL;
	mech.cm_param_len = 0;

	/* initialize passphrase as a crypto key */
	key.ck_format = CRYPTO_KEY_RAW;
	key.ck_length = BYTES_TO_BITS(passphraselen);
	key.ck_data = passphrase;

	/*
	 * initialize crypto data for the input data. length will change
	 * after the first iteration, so we will initialize it in the loop.
	 */
	in_data.cd_format = CRYPTO_DATA_RAW;
	in_data.cd_offset = 0;
	in_data.cd_raw.iov_base = (char *)hmac_key;

	/* initialize crypto data for the output data */
	out_data.cd_format = CRYPTO_DATA_RAW;
	out_data.cd_offset = 0;
	out_data.cd_length = SHA_256_DIGEST_LEN;
	out_data.cd_raw.iov_base = (char *)hmacresult;
	out_data.cd_raw.iov_len = SHA_256_DIGEST_LEN;

	/* initialize the context template */
	ret = crypto_create_ctx_template(&mech, &key, &tmpl, KM_SLEEP);
	if (ret != CRYPTO_SUCCESS) {
		ret = EIO;
		goto error;
	}

	/* main loop */
	for (blockptr = 0; blockptr < outputlen;
	    blockptr += SHA_256_DIGEST_LEN) {

		/*
		 * for the first iteration, the HMAC key is the user-provided
		 * salt concatenated with the block index (1-indexed)
		 */
		i = htobe32(1 + (blockptr / SHA_256_DIGEST_LEN));
		memmove(hmac_key, salt, saltlen);
		memmove(hmac_key + saltlen, (uint8_t *)(&i), sizeof(uint32_t));

		/* block initializes to zeroes (no XOR) */
		memset(block, 0, SHA_256_DIGEST_LEN);

		for (iter = 0; iter < iterations; iter++) {
			if (iter > 0) {
				in_data.cd_length = SHA_256_DIGEST_LEN;
				in_data.cd_raw.iov_len = SHA_256_DIGEST_LEN;
			} else {
				in_data.cd_length = saltlen + sizeof(uint32_t);
				in_data.cd_raw.iov_len = saltlen + sizeof(uint32_t);
			}

			ret = crypto_mac(&mech, &in_data, &key, tmpl,
			&out_data, NULL);
			if (ret != CRYPTO_SUCCESS) {
				ret = EIO;
				goto error;
			}

			/* HMAC key now becomes the output of this iteration */
			memmove(hmac_key, hmacresult, SHA_256_DIGEST_LEN);

			/* XOR this iteration's result with the current block */
			for (i = 0; i < SHA_256_DIGEST_LEN; i++) {
				block[i] ^= hmacresult[i];
			}
		}

		/*
		 * compute length of this block, make sure we don't write
		 * beyond the end of the output, truncating if necessary
		 */
		if (blockptr + SHA_256_DIGEST_LEN > outputlen) {
			memmove(output + blockptr, block, outputlen - blockptr);
		} else {
			memmove(output + blockptr, block, SHA_256_DIGEST_LEN);
		}
	}

	crypto_destroy_ctx_template(tmpl);
	free(hmac_key);
	icp_fini();
	thread_fini();

	return 0;

error:
	crypto_destroy_ctx_template(tmpl);
	if(hmac_key)
		free(hmac_key);
	icp_fini();
	thread_fini();

	return ret;
}

static int
derive_key(libzfs_handle_t *hdl, key_format_t format,
	uint8_t *key_material, size_t key_material_len, uint64_t salt,
	uint8_t **key_out)
{
	int ret, i;
	uint8_t *key;

	*key_out = NULL;

	key = zfs_alloc(hdl, WRAPPING_KEY_LEN);
	if (!key)
		return (ENOMEM);

	switch (format) {
	case KEY_FORMAT_RAW:
		bcopy(key_material, key, WRAPPING_KEY_LEN);
		break;
	case KEY_FORMAT_HEX:
		ret = hex_key_to_raw((char *) key_material,
		    WRAPPING_KEY_LEN * 2, key);
		if (ret) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Invalid hex key provided."));
			goto error;
		}
		break;
	case KEY_FORMAT_PASSPHRASE:
		ret = pbkdf2(key_material, strlen((char *)key_material),
		    ((uint8_t *)&salt), sizeof (uint64_t), PBKDF2_ITERATIONS,
		    key, WRAPPING_KEY_LEN);
		if (ret) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Failed to generate key from passphrase."));
			goto error;
		}

		for (i = 0; i < WRAPPING_KEY_LEN; i++) {
			printf("%02x", key[i] & 0xff);
		}
		printf("\n");
		break;
	default:
		ret = EINVAL;
		goto error;
	}

	*key_out = key;
	return (0);

error:
	free(key);

	*key_out = NULL;
	return (ret);
}

static boolean_t
encryption_feature_is_enabled(zpool_handle_t *zph)
{
	nvlist_t *features;
	uint64_t feat_refcount;

	/* check that features can be enabled */
	if (zpool_get_prop_int(zph, ZPOOL_PROP_VERSION, NULL)
	    < SPA_VERSION_FEATURES)
		return (B_FALSE);

	/* check for crypto feature */
	features = zpool_get_features(zph);
	if (!features || nvlist_lookup_uint64(features,
	    spa_feature_table[SPA_FEATURE_ENCRYPTION].fi_guid,
	    &feat_refcount) != 0)
		return (B_FALSE);

	return (B_TRUE);
}

static int
populate_create_encryption_params_nvlists(libzfs_handle_t *hdl, char *keysource,
    const char *fsname, nvlist_t *props, nvlist_t *hidden_args)
{
	int ret;
	uint64_t salt = 0;
	key_format_t keyformat;
	key_locator_t keylocator;
	uint8_t *key_material = NULL;
	size_t key_material_len = 0;
	uint8_t *key_data = NULL;
	char *uri;

	/* Parse the keysource */
	ret = keysource_prop_parser(keysource, &keyformat, &keylocator, &uri);
	if (ret) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "Invalid keysource."));
		goto error;
	}

	/* get key material from keysource */
	ret = get_key_material(hdl, B_TRUE, keyformat, keylocator, uri,
	    fsname, &key_material, &key_material_len);
	if (ret)
		goto error;

	/* passphrase formats require a salt property */
	if (keyformat == KEY_FORMAT_PASSPHRASE) {
		random_init();
		ret = random_get_bytes((uint8_t *) &salt, sizeof (uint64_t));
		if (ret) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Failed to generate salt."));
			goto error;
		}
		random_fini();
	}

	ret = nvlist_add_uint64(props, zfs_prop_to_name(ZFS_PROP_SALT), salt);
	if (ret) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Failed to add salt to properties."));
		goto error;
	}

	/* derive a key from the key material */
	ret = derive_key(hdl, keyformat, key_material, key_material_len, salt,
	    &key_data);
	if (ret)
		goto error;

	/* add the derived key to the properties list */
	ret = nvlist_add_uint8_array(hidden_args, "wkeydata", key_data,
	    WRAPPING_KEY_LEN);
	if (ret)
		goto error;

	free(key_material);
	free(key_data);

	return (0);

error:
	if (key_material)
		free(key_material);
	if (key_data)
		free(key_data);
	return (ret);
}

int
zfs_crypto_create(libzfs_handle_t *hdl, char *parent_name, nvlist_t *props,
    nvlist_t *pool_props, nvlist_t **hidden_args)
{
	int ret;
	char errbuf[1024];
	uint64_t crypt = 0, pcrypt = 0;
	char *keysource = NULL;
	zfs_handle_t *pzhp = NULL;
	nvlist_t *ha = NULL;
	boolean_t local_crypt = B_TRUE;

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Encryption create error"));

	/* lookup crypt from props */
	ret = nvlist_lookup_uint64(props,
	    zfs_prop_to_name(ZFS_PROP_ENCRYPTION), &crypt);
	if (ret)
		local_crypt = B_FALSE;

	/* lookup keysource from props */
	ret = nvlist_lookup_string(props,
	    zfs_prop_to_name(ZFS_PROP_KEYSOURCE), &keysource);
	if (ret)
		keysource = NULL;

	if (parent_name) {
		/* get a reference to parent dataset */
		pzhp = make_dataset_handle(hdl, parent_name);
		if (pzhp == NULL) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Failed to lookup parent."));
			return (ENOENT);
		}

		/* Lookup parent's crypt */
		pcrypt = zfs_prop_get_int(pzhp, ZFS_PROP_ENCRYPTION);

		/* Check for encryption feature */
		if (!encryption_feature_is_enabled(pzhp->zpool_hdl)) {
			if (!local_crypt && !keysource) {
				ret = 0;
				goto error;
			}

			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Encryption feature not enabled."));
			goto error;
		}
	} else {
		if (!nvlist_exists(pool_props, "feature@encryption")) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Encryption feature not enabled."));
		}

		pcrypt = crypt;
	}

	/* Check for encryption being explicitly truned off */
	if (crypt == ZIO_CRYPT_OFF && pcrypt != ZIO_CRYPT_OFF) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Invalid encryption value. Dataset must be encrypted."));
		goto error;
	}

	/* Get inherited the encryption property if we don't have it locally */
	if (!local_crypt)
		crypt = pcrypt;

	/*
	 * At this point crypt should be the actual encryption value.
	 * Return if encryption is off
	 */
	if (crypt == ZIO_CRYPT_OFF) {
		if (keysource) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Encryption required to set keysource."));
			goto error;
		}

		ret = 0;
		goto error;
	}

	/*
	 * If the parent doesn't have a keysource to inherit
	 *  we need one provided
	 */
	if (pcrypt == ZIO_CRYPT_OFF && !keysource) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Keysource required."));
		goto error;
	}

	/*
	 * If a local keysource is provided, this dataset will
	 * be a new encryption root. populate encryption params
	 */
	if (keysource) {
		ha = fnvlist_alloc();

		ret = populate_create_encryption_params_nvlists(hdl, keysource,
		    NULL, props, ha);
		if (ret)
			goto error;
	}

	if (pzhp)
		zfs_close(pzhp);

	*hidden_args = ha;
	return (0);

error:
	if (pzhp)
		zfs_close(pzhp);
	if (ha)
		nvlist_free(ha);

	*hidden_args = NULL;
	return (ret);
}

int
zfs_crypto_clone(libzfs_handle_t *hdl, zfs_handle_t *origin_zhp,
    char *parent_name, boolean_t add_key, nvlist_t *props,
    nvlist_t **hidden_args)
{
	int ret;
	char errbuf[1024];
	char *keysource = NULL;
	nvlist_t *ha = NULL;
	zfs_handle_t *pzhp = NULL;
	uint64_t crypt, pcrypt, ocrypt, okey_status;

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Encryption clone error"));

	/* get a reference to parent dataset, should never be null */
	pzhp = make_dataset_handle(hdl, parent_name);
	if (pzhp == NULL) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Failed to lookup parent."));
		return (ENOENT);
	}

	/* Lookup parent's crypt */
	pcrypt = zfs_prop_get_int(pzhp, ZFS_PROP_ENCRYPTION);
	ocrypt = zfs_prop_get_int(origin_zhp, ZFS_PROP_ENCRYPTION);

	/* lookup keysource from props */
	ret = nvlist_lookup_string(props,
	    zfs_prop_to_name(ZFS_PROP_KEYSOURCE), &keysource);
	if (ret)
		keysource = NULL;

	/* crypt should not be set */
	ret = nvlist_lookup_uint64(props, zfs_prop_to_name(ZFS_PROP_ENCRYPTION),
	    &crypt);
	if (ret == 0) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Encryption may not be specified during cloning."));
		goto out;
	}

	/* all children of encrypted parents must be encrypted */
	if (pcrypt != ZIO_CRYPT_OFF && ocrypt == ZIO_CRYPT_OFF) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Cannot create unencrypted clone as child "
		    "of encrypted parent."));
		goto out;
	}

	/*
	 * if neither parent nor the origin is encrypted check to make
	 * sure no encryption parameters are set
	 */
	if (pcrypt == ZIO_CRYPT_OFF && ocrypt == ZIO_CRYPT_OFF) {
		if (add_key || keysource) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Encryption properties may not be set "
			    "for an unencrypted clone."));
			goto out;
		}

		ret = 0;
		goto out;
	}

	/*
	 * by this point this dataset will be encrypted. The origin's
	 * wrapping key must be loaded
	 */
	okey_status = zfs_prop_get_int(origin_zhp, ZFS_PROP_KEYSTATUS);
	if (okey_status != ZFS_KEYSTATUS_AVAILABLE) {
		ret = EPERM;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Origin wrapping key must be loaded."));
		goto out;
	}

	/*
	 * if the parent doesn't have a keysource to inherit we need
	 * one provided for us
	 */
	if (pcrypt == ZIO_CRYPT_OFF && !keysource) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Keysource required."));
		goto out;
	}

	/* prepare the keysource if needed */
	if (keysource) {
		ha = fnvlist_alloc();

		ret = populate_create_encryption_params_nvlists(hdl,keysource,
		    NULL, props, ha);
		if (ret)
			goto out;
	}

	if (add_key) {
		ret = nvlist_add_uint64(props, "crypto_cmd",
		    ZFS_IOC_CRYPTO_ADD_KEY);
		if (ret)
			goto out;
	}

	zfs_close(pzhp);

	*hidden_args = ha;
	return (0);

out:
	if (pzhp)
		zfs_close(pzhp);
	if (ha)
		nvlist_free(ha);

	*hidden_args = NULL;
	return (ret);
}

int
zfs_crypto_load_key(zfs_handle_t *zhp)
{
	int ret;
	char errbuf[1024];
	uint64_t crypt, keystatus, salt = 0;
	char keysource[MAXNAMELEN];
	char keysource_src[MAXNAMELEN];
	key_format_t format;
	key_locator_t locator;
	char *uri;
	uint8_t *key_material, *key_data;
	size_t key_material_len;
	nvlist_t *crypto_args = NULL;
	zprop_source_t keysource_srctype;

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Key load error"));

	if (!encryption_feature_is_enabled(zhp->zpool_hdl)) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption feature not enabled."));
		ret = EINVAL;
		goto error;
	}

	/* fetch relevent info from the dataset properties */
	crypt = zfs_prop_get_int(zhp, ZFS_PROP_ENCRYPTION);
	if (crypt == ZIO_CRYPT_OFF) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption not enabled for this dataset."));
		ret = EINVAL;
		goto error;
	}

	/* check that we are loading for an encryption root */
	ret = zfs_prop_get(zhp, ZFS_PROP_KEYSOURCE, keysource,
	    sizeof (keysource), &keysource_srctype, keysource_src,
	    sizeof (keysource_src), B_TRUE);
	if (ret) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Failed to obtain keysource property."));
		ret = EIO;
		goto error;
	} else if (keysource_srctype == ZPROP_SRC_INHERITED) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Keys must be loaded for encryption root '%s'."),
		    keysource_src);
		ret = EINVAL;
		goto error;
	}

	/* check that the key is unloaded */
	keystatus = zfs_prop_get_int(zhp, ZFS_PROP_KEYSTATUS);
	if (keystatus == ZFS_KEYSTATUS_AVAILABLE) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Key already loaded."));
		ret = EINVAL;
		goto error;
	}

	/* parse the keysource */
	ret = keysource_prop_parser(keysource, &format, &locator, &uri);
	if (ret) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Invalid keysource property."));
		ret = EIO;
		goto error;
	}

	/* get key material from keysource */
	ret = get_key_material(zhp->zfs_hdl, B_FALSE, format, locator, uri,
	    zfs_get_name(zhp), &key_material, &key_material_len);
	if (ret)
		goto error;

	/* passphrase formats require a salt property */
	if (format == KEY_FORMAT_PASSPHRASE)
		salt = zfs_prop_get_int(zhp, ZFS_PROP_SALT);

	/* derive a key from the key material */
	ret = derive_key(zhp->zfs_hdl, format, key_material,
	    key_material_len, salt, &key_data);
	if (ret)
		goto error;

	/* put the key in an nvlist and pass to the ioctl */
	crypto_args = fnvlist_alloc();

	ret = nvlist_add_uint8_array(crypto_args, "wkeydata", key_data,
	    WRAPPING_KEY_LEN);
	if (ret)
		goto error;

	ret = lzc_crypto(zhp->zfs_name, ZFS_IOC_CRYPTO_LOAD_KEY, NULL,
	    crypto_args);

	if (ret) {
		switch (ret) {
		case EINVAL:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Incorrect key provided."));
			break;
		case EEXIST:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Keychain is already loaded."));
			break;
		case EBUSY:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Dataset is busy."));
			break;
		}
		zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	}

	nvlist_free(crypto_args);
	free(key_material);
	free(key_data);

	return (ret);

error:
	zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	if (key_material)
		free(key_material);
	if (key_data)
		free(key_data);
	if (crypto_args)
		nvlist_free(crypto_args);

	return (ret);
}

int
zfs_crypto_unload_key(zfs_handle_t *zhp)
{
	int ret;
	char errbuf[1024];
	char keysource[MAXNAMELEN];
	char keysource_src[MAXNAMELEN];
	uint64_t crypt, keystatus;
	zprop_source_t keysource_srctype;

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Key unload error"));

	if (!encryption_feature_is_enabled(zhp->zpool_hdl)) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption feature not enabled."));
		ret = EINVAL;
		goto error;
	}

	/* fetch relevent info from the dataset properties */
	crypt = zfs_prop_get_int(zhp, ZFS_PROP_ENCRYPTION);
	if (crypt == ZIO_CRYPT_OFF) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption not enabled."));
		ret = EINVAL;
		goto error;
	}

	/* check that we are loading for an encryption root */
	ret = zfs_prop_get(zhp, ZFS_PROP_KEYSOURCE, keysource,
	    sizeof (keysource), &keysource_srctype, keysource_src,
	    sizeof (keysource_src), B_TRUE);
	if (ret) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Failed to obtain keysource property."));
		ret = EIO;
		goto error;
	} else if (keysource_srctype == ZPROP_SRC_INHERITED) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Keys must be unloaded for encryption root '%s'."),
		    keysource_src);
		ret = EINVAL;
		goto error;
	}

	/* check that the key is loaded */
	keystatus = zfs_prop_get_int(zhp, ZFS_PROP_KEYSTATUS);
	if (keystatus == ZFS_KEYSTATUS_UNAVAILABLE) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Key already unloaded."));
		return (EINVAL);
	}

	/* call the ioctl */
	ret = lzc_crypto(zhp->zfs_name, ZFS_IOC_CRYPTO_UNLOAD_KEY, NULL, NULL);

	if (ret) {
		switch (ret) {
		case ENOENT:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Keychain is not currently loaded."));
			break;
		case EBUSY:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Dataset is busy."));
			break;
		}
		zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	}

	return (ret);

error:
	zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	return (ret);
}

int
zfs_crypto_add_key(zfs_handle_t *zhp)
{
	int ret;
	char errbuf[1024];
	uint64_t crypt;

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Add key error"));

	if (!encryption_feature_is_enabled(zhp->zpool_hdl)) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption feature not enabled."));
		ret = EINVAL;
		goto error;
	}

	/* check that encryption is on for the dataset */
	crypt = zfs_prop_get_int(zhp, ZFS_PROP_ENCRYPTION);
	if (crypt == ZIO_CRYPT_OFF) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption not enabled."));
		ret = EINVAL;
		goto error;
	}

	/* call the ioctl */
	ret = lzc_crypto(zhp->zfs_name, ZFS_IOC_CRYPTO_ADD_KEY, NULL, NULL);
	if (ret) {
		switch (ret) {
		case ENOENT:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Keychain is not currently loaded."));
			break;
		}
		zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	}

	return (ret);

error:
	zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	return (ret);
}

int
zfs_crypto_rewrap(zfs_handle_t *zhp, nvlist_t *props)
{
	int ret;
	char errbuf[1024];
	nvlist_t *crypto_args = NULL;
	uint64_t crypt;
	char prop_keysource[MAXNAMELEN];
	char *keysource;

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Rewrap keychain error"));

	if (!encryption_feature_is_enabled(zhp->zpool_hdl)) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption feature not enabled."));
		ret = EINVAL;
		goto error;
	}

	/* get crypt from dataset */
	crypt = zfs_prop_get_int(zhp, ZFS_PROP_ENCRYPTION);
	if (crypt == ZIO_CRYPT_OFF) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption not enabled."));
		ret = EINVAL;
		goto error;
	}

	/* load keysource from dataset if not specified */
	ret = nvlist_lookup_string(props, zfs_prop_to_name(ZFS_PROP_KEYSOURCE),
	    &keysource);
	if (ret == ENOENT) {
		ret = zfs_prop_get(zhp, ZFS_PROP_KEYSOURCE, prop_keysource,
		    sizeof (prop_keysource), NULL, NULL, 0, B_TRUE);
		if (ret) {
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Failed to obtain keysource property."));
			ret = EIO;
			goto error;
		}
		keysource = prop_keysource;
	} else if (ret) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Failed to find keysource."));
		ret = EIO;
		goto error;
	}

	/* populate an nvlist with the encryption params */
	crypto_args = fnvlist_alloc();

	ret = populate_create_encryption_params_nvlists(zhp->zfs_hdl, keysource,
	    zfs_get_name(zhp), props, crypto_args);
	if (ret)
		goto error;

	/* call the ioctl */
	ret = lzc_crypto(zhp->zfs_name, ZFS_IOC_CRYPTO_REWRAP, props,
	    crypto_args);
	if (ret) {
		switch (ret) {
		case EINVAL:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Invalid properties for key change."));
			break;
		case ENOENT:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Keychain is not currently loaded."));
			break;
		}
		zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	}

	nvlist_free(crypto_args);

	return (ret);

error:
	if (crypto_args)
		nvlist_free(crypto_args);

	zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	return (ret);
}
