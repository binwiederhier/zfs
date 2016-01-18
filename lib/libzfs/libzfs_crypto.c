/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Copyright 2016 Datto, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <libintl.h>
#include <libzfs.h>
#include <sys/zio_crypt.h>

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

static int parse_format(key_format_t *format, char *s, int len) {

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

static int parse_locator(key_locator_t *locator, char *s, int len, char **uri) {
	if (len == 6 && strncmp("prompt", s, 6) == 0) {
		*locator = KEY_LOCATOR_PROMPT;
		return (0);
	}

	if (len > 8 && strncmp("file:///", s, 8) == 0) {
		*locator = KEY_LOCATOR_URI;
		*uri = s;
		return (0);
	}

	return (EINVAL);
}

static int keysource_prop_parser(char *keysource, key_format_t *format, key_locator_t *locator, char **uri) {
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

static int get_key_material(libzfs_handle_t *hdl, key_format_t format, key_locator_t locator, int keylen, uint8_t **key_material, size_t *key_material_len){
	int ret;
	int rbytes;
	
	*key_material = NULL;
	*key_material_len = 0;

	switch (locator) {
	case KEY_LOCATOR_PROMPT:
		if (format == KEY_FORMAT_RAW) {
			*key_material = zfs_alloc(hdl, keylen);
			if(!*key_material)
				return (ENOMEM);
			
			errno = 0;
			rbytes = read(STDIN_FILENO, *key_material, keylen);
			if (rbytes != keylen) {
				ret = errno;
				goto error;
			}
			*key_material_len = keylen;

		} else {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "URI key location not yet supported."));
			return (EOPNOTSUPP);
		}

		break;

	case KEY_LOCATOR_URI:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "URI key location not yet supported."));
		return (EOPNOTSUPP);
	default:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "Invalid key locator."));
		return (EINVAL);
	}

	return (0);

error:
	*key_material_len = 0;
	
	if(*key_material){
		free(*key_material);
		*key_material = NULL;
	}
	return (ret);
}

static int derive_key(libzfs_handle_t *hdl, key_format_t format, int keylen, uint8_t *key_material, size_t key_material_len, uint64_t salt, uint8_t **key){
	int ret;
	
	*key = zfs_alloc(hdl, keylen);
	if (!*key)
		return (ENOMEM);
	
	switch(format){
	case KEY_FORMAT_RAW:
		if(keylen != key_material_len){
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "Incorrect key size."));
			ret = EINVAL;
			goto error;
		}
		bcopy(*key, key_material, keylen);
		break;
	case KEY_FORMAT_HEX:
	case KEY_FORMAT_PASSPHRASE:
		ret = EOPNOTSUPP;
		goto error;
	default:
		ret = EINVAL;
		goto error;
	}
	
	return (0);
	
error:
	free(*key);
	return (ret);
}

int zfs_crypto_create(libzfs_handle_t *hdl, nvlist_t *props, char *parent_name){	
	char errbuf[1024];
	uint64_t crypt = 0, pcrypt = 0;
	char *keysource = NULL;
	int ret = 0;
	zfs_handle_t *pzhp = NULL;
	boolean_t local_crypt = B_TRUE;
	boolean_t local_keysource = B_TRUE;
	nvlist_t *features = NULL;
	uint64_t feat_refcount, salt = 0;
	key_format_t keyformat;
	key_locator_t keylocator;
	uint8_t *key_material = NULL;
	size_t key_material_len = 0;
	uint8_t *key_data = NULL;
	char *uri;
	
	printf("here 1\n");
	
	(void) snprintf(errbuf, sizeof (errbuf), dgettext(TEXT_DOMAIN, "Encryption create error"));

	/* lookup crypt from props */
	ret = nvlist_lookup_uint64(props, zfs_prop_to_name(ZFS_PROP_ENCRYPTION), &crypt);
	if (ret != 0) {
		local_crypt = B_FALSE;
	}

	/* lookup keysource from props */
	ret = nvlist_lookup_string(props, zfs_prop_to_name(ZFS_PROP_KEYSOURCE), &keysource);
	if (ret != 0) {
		local_keysource = B_FALSE;
	}

	/* get a reference to parent dataset, should never be null */
	pzhp = make_dataset_handle(hdl, parent_name);
	if (pzhp == NULL) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "Failed to obtain parent to check for encryption feature."));
		return (ENOENT);
	}
	
	/* Lookup parent's crypt */
	pcrypt = zfs_prop_get_int(pzhp, ZFS_PROP_ENCRYPTION);

	/* Check for non-feature pool version */
	if (zpool_get_prop_int(pzhp->zpool_hdl, ZPOOL_PROP_VERSION, NULL) < SPA_VERSION_FEATURES) {
		if (!local_crypt && !local_keysource)
			return (0);

		zfs_error_aux(hdl, gettext("Feature flags unavailable so encryption cannot be used."));
		return (EINVAL);
	}
	
	/* Check for encryption feature */
	features = zpool_get_features(pzhp->zpool_hdl);		
	if (!features || nvlist_lookup_uint64(features, spa_feature_table[SPA_FEATURE_CRYPTO].fi_guid, &feat_refcount) != 0){
		if (!local_crypt && !local_keysource)
			return (0);

		zfs_error_aux(hdl, gettext("Encyrypted datasets feature not enabled."));
		return (EINVAL);
	}
	
	/* Check for encryption being explicitly truned off */
	if (crypt == ZIO_CRYPT_OFF && pcrypt != ZIO_CRYPT_OFF) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "Invalid encryption value. Dataset must be encrypted."));
		return (EINVAL);
	}
	
	printf("here 2\n");
	
	/* Inherit the encryption property if we don't have it locally */
	if (!local_crypt)
		crypt = pcrypt;
	
	/* At this point crypt should be the actual encryption value. Return if encryption is off */
	if (crypt == ZIO_CRYPT_OFF){
		if (local_keysource){
			zfs_error_aux(hdl, gettext("Encryption required to set keysource."));
			return (EINVAL);
		}
		
		printf("here 3\n");
		return (0);
	}	
	
	/* Inherit the keysource property if we don't have it locally */
	if (!local_keysource) {
		keysource = zfs_alloc(hdl, ZPOOL_MAXPROPLEN);
		if (keysource == NULL) {
			(void) no_memory(hdl);
			return (ENOMEM);
		}

		if (zfs_prop_get(pzhp, ZFS_PROP_KEYSOURCE, keysource, ZPOOL_MAXPROPLEN, NULL, NULL, 0, FALSE) != 0) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "No keysource property available from parent."));
			ret = ENOENT;
			goto out;
		}
	}
	
	/* Parse the keysource */
	ret = keysource_prop_parser(keysource, &keyformat, &keylocator, &uri);
	if (ret) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "Invalid keysource."));
		goto out;
	}

	printf("here 4\n");
	
	/* If a local keysource or crypt is provided, this dataset will have a new keychain. Otherwise use the parent's. */
	if (local_crypt || local_keysource) {
		
		/* get key material from keysource */
		ret = get_key_material(hdl, keyformat, keylocator, zio_crypt_table[crypt].ci_keylen, &key_material, &key_material_len);
		if (ret)
			goto out;
		
		/* passphrase formats require a salt property */
		if (keyformat == KEY_FORMAT_PASSPHRASE) {
			ret = random_get_bytes((uint8_t *)&salt, sizeof(uint64_t));
			if (ret) {
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "Failed to generate salt."));
				goto out;
			}
			
			ret = nvlist_add_uint64(props, zfs_prop_to_name(ZFS_PROP_SALT), salt);
			if (ret) {
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "Failed to add salt to properties."));
				goto out;
			}
		}
		
		printf("here 5\n");
		
		/* derive a key from the key material */
		ret = derive_key(hdl, keyformat, zio_crypt_table[crypt].ci_keylen, key_material, key_material_len, salt, &key_data);
		if (ret)
			goto out;
		
		printf("here 6\n");
		
		/* add the derived key to the properties array */
		ret = nvlist_add_uint8_array(props, "wkeydata", key_data, zio_crypt_table[crypt].ci_keylen);
		if (ret)
			goto out;
		
		printf("here 7\n");
	}
	
	if (!local_keysource)
		free(keysource);
	
	printf("here 7\n");
	
	return (0);
	
out:
	if (!local_keysource)
		free(keysource);
	
	return (ret);
}