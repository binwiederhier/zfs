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
 * Copyright (c) 2016, Datto, Inc. All rights reserved.
 */

#include <sys/dsl_keychain.h>
#include <sys/dsl_pool.h>
#include <sys/zap.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_prop.h>
#include <sys/spa_impl.h>

//macros for defining encryption parameter lengths
#define MAX_KEY_LEN 32
#define ZIO_CRYPT_WRAPKEY_IVLEN 13
#define WRAPPING_MAC_LEN 16
#define WRAPPED_KEYDATA_LEN(keylen) ((keylen) + ZIO_CRYPT_WRAPKEY_IVLEN + WRAPPING_MAC_LEN)

static int zio_crypt_key_wrap(dsl_wrapping_key_t *wkey, uint64_t crypt, uint8_t *keydata, uint8_t *ivdata, dsl_crypto_key_phys_t *dckp){
	int ret;
	
	ASSERT(crypt < ZIO_CRYPT_FUNCTIONS);
	ASSERT(wkey->wk_key.ck_format == CRYPTO_KEY_RAW);

	//copy the crypt and iv data into the dsl_crypto_key_phys_t
	dckp->dk_crypt_alg = crypt;
	bcopy(ivdata, dckp->dk_iv, ZIO_CRYPT_WRAPKEY_IVLEN);
	bzero(dckp->dk_padding, sizeof(dckp->dk_padding));
	bzero(dckp->dk_keybuf, sizeof(dckp->dk_keybuf));
	
	//encrypt the key and store the result in dckp->keybuf
	ret = zio_encrypt(crypt, &wkey->wk_key, NULL, ivdata, ZIO_CRYPT_WRAPKEY_IVLEN, WRAPPING_MAC_LEN, keydata, dckp->dk_keybuf, zio_crypt_table[crypt].ci_keylen);
	if(ret) goto error;
	
	return 0;
error:
	LOG_ERROR(ret, "");
	return ret;
}

static int zio_crypt_key_unwrap(dsl_wrapping_key_t *wkey, uint64_t crypt, dsl_crypto_key_phys_t *dckp, uint8_t *keydata){
	int ret;
	
	ASSERT(crypt < ZIO_CRYPT_FUNCTIONS);
	ASSERT(wkey->wk_key.zk_key->ck_format == CRYPTO_KEY_RAW);

	//encrypt the key and store the result in dckp->keybuf
	ret = zio_decrypt(crypt, &wkey->wk_key, NULL, dckp->dk_iv, ZIO_CRYPT_WRAPKEY_IVLEN, WRAPPING_MAC_LEN, keydata, dckp->dk_keybuf, zio_crypt_table[crypt].ci_keylen);
	if(ret) goto error;
	
	return 0;
error:
	LOG_ERROR(ret, "");
	return ret;
}

static int spa_keychains_compare(const void *a, const void *b){
	const dsl_keychain_t *kca = a;
	const dsl_keychain_t *kcb = b;
	
	if(kca->kc_obj < kcb->kc_obj) return -1;
	if(kca->kc_obj > kcb->kc_obj) return 1;
	return 0;
}

static int spa_keychain_recs_compare(const void *a, const void *b){
	const dsl_keychain_record_t *kra = a;
	const dsl_keychain_record_t *krb = b;
	
	if(kra->kr_dsobj < krb->kr_dsobj) return -1;
	if(kra->kr_dsobj > krb->kr_dsobj) return 1;
	return 0;
}

static int spa_wkey_compare(const void *a, const void *b){
	const dsl_wrapping_key_t *wka = a;
	const dsl_wrapping_key_t *wkb = b;
	
	if(wka->wk_ddobj < wkb->wk_ddobj) return -1;
	if(wka->wk_ddobj > wkb->wk_ddobj) return 1;
	return 0;
}

void spa_keystore_init(spa_keystore_t *sk){
	rw_init(&sk->sk_kc_lock, NULL, RW_DEFAULT, NULL);
	rw_init(&sk->sk_kr_lock, NULL, RW_DEFAULT, NULL);
	rw_init(&sk->sk_wkeys_lock, NULL, RW_DEFAULT, NULL);
	avl_create(&sk->sk_keychains, spa_keychains_compare, sizeof (dsl_keychain_t), offsetof(dsl_keychain_t, kc_avl_link));
	avl_create(&sk->sk_keychain_recs, spa_keychain_recs_compare, sizeof (dsl_keychain_record_t), offsetof(dsl_keychain_record_t, kr_avl_link));
	avl_create(&sk->sk_wkeys, spa_wkey_compare, sizeof (dsl_wrapping_key_t), offsetof(dsl_wrapping_key_t, wk_avl_link));
}

void spa_keystore_fini(spa_keystore_t *sk){
	avl_destroy(&sk->sk_wkeys);
	avl_destroy(&sk->sk_keychain_recs);
	avl_destroy(&sk->sk_keychains);
	rw_destroy(&sk->sk_wkeys_lock);
	rw_destroy(&sk->sk_kr_lock);
	rw_destroy(&sk->sk_kc_lock);
}

static void dsl_keychain_entry_free(dsl_keychain_entry_t *kce){
	zio_crypt_key_destroy(&kce->ke_key);
	kmem_free(kce, sizeof(dsl_keychain_entry_t));
}

static int dsl_keychain_entry_sync(dsl_keychain_entry_t *kce, dsl_wrapping_key_t *wkey, uint64_t kcobj, dmu_tx_t *tx){
	int ret;
	uint8_t ivdata[ZIO_CRYPT_WRAPKEY_IVLEN];
	dsl_crypto_key_phys_t key_phys;
	
	//generate an iv
	ret = random_get_bytes(ivdata, ZIO_CRYPT_WRAPKEY_IVLEN);
	if(ret) goto error;
	
	//wrap the key and store the result in key_phys
	ret = zio_crypt_key_wrap(wkey, kce->ke_key.zk_crypt, kce->ke_key.zk_key.ck_data, ivdata, &key_phys);
	if(ret) goto error;
	
	//sync the change to disk
	ret = zap_update_uint64(tx->tx_pool->dp_meta_objset, kcobj, &kce->ke_txgid, 1, 1, sizeof(dsl_crypto_key_phys_t), &key_phys, tx);
	if(ret) goto error;
	
	return 0;

error:
	LOG_ERROR(ret, "");
	return ret;
}

static int dsl_keychain_entry_init(dsl_keychain_entry_t *kce, uint64_t crypt, uint64_t txgid){
	int ret;
	uint64_t keydata_len = zio_crypt_table[crypt].ci_keylen;
	uint8_t rnddata[keydata_len];
	
	//initialize the struct values
	list_link_init(&kce->ke_link);
	kce->ke_txgid = txgid;
	
	//fill a buffer with random data
	ret = random_get_bytes(rnddata, keydata_len);
	if(ret) goto error;
	
	//create the key from the random data
	ret = zio_crypt_key_init(crypt, rnddata, &kce->ke_key);
	if(ret) goto error;
	
	return 0;
	
error:
	LOG_ERROR(ret, "");
	return ret;
}

static int dsl_keychain_add_key_sync_impl(dsl_keychain_t *kc, uint64_t crypt, uint64_t txgid, dmu_tx_t *tx){
	int ret;
	dsl_keychain_entry_t *kce = NULL;
	
	//allocate the keychain entry
	kce = kmem_zalloc(sizeof(dsl_keychain_entry_t), KM_SLEEP);
	if(!kce) return SET_ERROR(ENOMEM);
	
	ret = dsl_keychain_entry_init(kce, crypt, txgid);
	if(ret) goto error;
	
	rw_enter(&kc->kc_lock, RW_WRITER);
	
	//sync the new keychain entry to disk
	ret = dsl_keychain_entry_sync(kce, kc->kc_wkey, kc->kc_obj, tx);
	if(ret) goto error_unlock;
		
	list_insert_tail(&kc->kc_entries, kce);

	rw_exit(&kc->kc_lock);
	
	return 0;

error_unlock:
	rw_exit(&kc->kc_lock);
error:
	LOG_ERROR(ret, "");
	zio_crypt_key_destroy(&kce->ke_key);
	kmem_free(kce, sizeof(dsl_keychain_entry_t));
	return ret;
}

static int dsl_dir_hold_keysource_source_dd(dsl_dir_t *dd, void *tag, dsl_dir_t **inherit_dd_out){
	int ret;
	dsl_dir_t *inherit_dd = NULL;
	char keysource[MAXNAMELEN];
	char setpoint[MAXNAMELEN];
	
	//lookup dd's keysource property and store where it inherited it from in setpoint
	ret = dsl_prop_get_dd(dd, zfs_prop_to_name(ZFS_PROP_KEYSOURCE), 1, sizeof(keysource), keysource, setpoint, B_FALSE);
	if(ret) goto error;
	
	//hold the dsl dir that we inherited the property from (this may be the same as dd)
	ret = dsl_dir_hold(dd->dd_pool, setpoint, tag, &inherit_dd, NULL);
	if(ret) goto error;
	
	*inherit_dd_out = inherit_dd;
	return 0;
	
error:
	LOG_ERROR(ret, "");
	
	*inherit_dd_out = NULL;
	return ret;
}

static int spa_keystore_wkey_hold_ddobj_impl(spa_t *spa, uint64_t ddobj, void *tag, dsl_wrapping_key_t **wkey_out){
	int ret;
	dsl_wrapping_key_t search_wkey;
	dsl_wrapping_key_t *found_wkey;
	
	ASSERT(RW_LOCK_HELD(&spa->spa_keystore.sk_wkeys_lock));
	LOG_DEBUG("looking up wkey directly %d", (int)ddobj);
	
	//init the search wrapping key
	search_wkey.wk_ddobj = ddobj;
	
	//lookup the wrapping key
	found_wkey = avl_find(&spa->spa_keystore.sk_wkeys, &search_wkey, NULL);
	if(!found_wkey){
		ret = SET_ERROR(ENOENT);
		goto error;
	}
	
	//increment the refcount
	dsl_wrapping_key_hold(found_wkey, tag);
	
	*wkey_out = found_wkey;
	return 0;
	
error:
	LOG_ERROR(ret, "");
	*wkey_out = NULL;
	return ret;
}

int spa_keystore_wkey_hold_ddobj(spa_t *spa, uint64_t ddobj, void *tag, dsl_wrapping_key_t **wkey_out){
	int ret;
	dsl_pool_t *dp = spa_get_dsl(spa);
	dsl_dir_t *dd = NULL, *inherit_dd = NULL;
	dsl_wrapping_key_t *wkey;
	boolean_t locked = B_FALSE;
	
	LOG_DEBUG("looking up wkey %u", (unsigned)ddobj);
	
	//hold the dsl dir
	ret = dsl_dir_hold_obj(dp, ddobj, NULL, FTAG, &dd);
	if(ret) goto error;
	
	//get the dd that the keysource property was inherited from
	ret = dsl_dir_hold_keysource_source_dd(dd, FTAG, &inherit_dd);
	if(ret) goto error;
	
	//lookup the wkey in the avl tree
	if(!RW_LOCK_HELD(&dp->dp_spa->spa_keystore.sk_wkeys_lock)){
		rw_enter(&spa->spa_keystore.sk_wkeys_lock, RW_READER);
		locked = B_TRUE;
	}
	
	ret = spa_keystore_wkey_hold_ddobj_impl(spa, inherit_dd->dd_object, tag, &wkey);
	if(ret) goto error_unlock;
	
	if(locked) rw_exit(&spa->spa_keystore.sk_wkeys_lock);
	
	LOG_DEBUG("found wkey %u", (unsigned)ddobj);
	
	dsl_dir_rele(inherit_dd, FTAG);
	dsl_dir_rele(dd, FTAG);
	
	*wkey_out = wkey;
	return 0;
	
error_unlock:
	if(locked) rw_exit(&spa->spa_keystore.sk_wkeys_lock);
error:
	LOG_ERROR(ret, "");
	if(inherit_dd) dsl_dir_rele(inherit_dd, FTAG);
	if(dd) dsl_dir_rele(dd, FTAG);
	
	*wkey_out = NULL;
	return ret;
}

static void dsl_keychain_free(dsl_keychain_t *kc){
	dsl_keychain_entry_t *kce;
	
	VERIFY0(refcount_count(&kc->kc_refcnt));
	
	//release each encryption key from the keychain
	while((kce = list_head(&kc->kc_entries)) != NULL){
		list_remove(&kc->kc_entries, kce);
		dsl_keychain_entry_free(kce);
	}
	
	//free the keychain entries list, refcount, wrapping key, and lock
	rw_destroy(&kc->kc_lock);
	list_destroy(&kc->kc_entries);
	refcount_destroy(&kc->kc_refcnt);
	if(kc->kc_wkey) dsl_wrapping_key_rele(kc->kc_wkey, kc);
	
	//free the keychain
	kmem_free(kc, sizeof(dsl_keychain_t));
}

static void dsl_keychain_rele(dsl_keychain_t *kc, void *tag){
	if(refcount_remove(&kc->kc_refcnt, tag) == 0) dsl_keychain_free(kc);
}

static int dsl_keychain_open(objset_t *mos, dsl_wrapping_key_t *wkey, uint64_t kcobj, void *tag, dsl_keychain_t **kc_out){
	int ret;
	boolean_t need_crypt = B_TRUE;
	zap_cursor_t zc;
	zap_attribute_t za;
	uint64_t txgid, crypt = 0;
	dsl_crypto_key_phys_t dckp;
	uint8_t keydata[MAX_KEY_LEN + WRAPPING_MAC_LEN];
	dsl_keychain_t *kc;
	dsl_keychain_entry_t *cur_kce, *kce = NULL;
	
	//allocate and initialize the keychain
	kc = kmem_zalloc(sizeof(dsl_keychain_t), KM_SLEEP);
	if(!kc) return SET_ERROR(ENOMEM);
	
	rw_init(&kc->kc_lock, NULL, RW_DEFAULT, NULL);
	list_create(&kc->kc_entries, sizeof(dsl_keychain_entry_t), offsetof(dsl_keychain_entry_t, ke_link));
	refcount_create(&kc->kc_refcnt);
	
	//iterate all entries in the on-disk keychain
	for(zap_cursor_init(&zc, mos, kcobj); zap_cursor_retrieve(&zc, &za) == 0; zap_cursor_advance(&zc)){
		//get the txgid from the name
		txgid = ((uint64_t)*za.za_name);
		
		//lookup the dsl_crypto_key_phys_t value of the encryption key
		ret = zap_lookup_uint64(mos, kcobj, &txgid, 1, 1, sizeof(dsl_crypto_key_phys_t), &dckp);
		if(ret){
			ret = SET_ERROR(EIO);
			goto error;
		}
		
		//all crypts should match
		ASSERT(need_crypt || dckp.dk_crypt_alg == crypt);
		
		if(need_crypt){
			crypt = dckp.dk_crypt_alg;
			need_crypt = B_FALSE;
		}
		
		//unwrap the key, will return error if wkey is incorrect by checking the MAC
		ret = zio_crypt_key_unwrap(wkey, crypt, &dckp, keydata);
		if(ret){
			ret = SET_ERROR(EINVAL);
			goto error;
		}
		
		//allocate an initialize a keychain entry
		kce = kmem_zalloc(sizeof(dsl_keychain_entry_t), KM_SLEEP);
		if(!kce){
			ret = SET_ERROR(ENOMEM);
			goto error;
		}
		list_link_init(&kce->ke_link);
		kce->ke_txgid = txgid;
		
		ret = zio_crypt_key_init(crypt, keydata, &kce->ke_key);
		if(ret) goto error;
		
		//the zap does not store keys in order, we must add them in order
		for(cur_kce = list_head(&kc->kc_entries); cur_kce; cur_kce = list_next(&kc->kc_entries, cur_kce)){
			if(cur_kce->ke_txgid > kce->ke_txgid) break;
		}
		list_insert_before(&kc->kc_entries, cur_kce, kce);
		
		//set kce to NULL so error handling doesn't attempt to double free it
		kce = NULL;
	}
	//release the zap crusor
	zap_cursor_fini(&zc);
	
	//if we still need the crypt, there no entries to loop over, return an error
	if(need_crypt){
		ret = SET_ERROR(EIO);
		goto error;
	}
	
	//finish initizing the keychain
	dsl_wrapping_key_hold(wkey, kc);
	kc->kc_wkey = wkey;
	kc->kc_obj = kcobj;
	kc->kc_crypt = crypt;
	refcount_add(&kc->kc_refcnt, tag);
	
	*kc_out = kc;
	return 0;
	
error:
	LOG_ERROR(ret, "");
	if(kce) dsl_keychain_entry_free(kce);
	if(kc) dsl_keychain_free(kc);
	
	*kc_out = NULL;
	return ret;
}

static int spa_keystore_keychain_hold_impl(spa_t *spa, uint64_t kcobj, void *tag, dsl_keychain_t **kc_out){
	int ret;
	dsl_keychain_t search_kc;
	dsl_keychain_t *found_kc;
	
	ASSERT(RW_LOCK_HELD(&spa->spa_keystore.sk_kc_lock));
	
	//init the search keychain
	search_kc.kc_obj = kcobj;
	
	//find the keychain in the keystore
	found_kc = avl_find(&spa->spa_keystore.sk_keychains, &search_kc, NULL);
	if(!found_kc){
		ret = SET_ERROR(ENOENT);
		goto error;
	}
	
	//increment the refcount
	refcount_add(&found_kc->kc_refcnt, tag);
	
	*kc_out = found_kc;
	return 0;
	
error:
	LOG_ERROR(ret, "");
	*kc_out = NULL;
	return ret;
}

int spa_keystore_keychain_hold_dd(spa_t *spa, dsl_dir_t *dd, void *tag, dsl_keychain_t **kc_out){
	int ret;
	avl_index_t where;
	dsl_keychain_t *kc = NULL;
	dsl_wrapping_key_t *wkey = NULL;
	uint64_t kcobj = dsl_dir_phys(dd)->dd_keychain_obj;
	
	/*
	 * we need a write lock here because we might load a keychain from disk if we don't have it
	 * in the keystore already. This could be a problem because this lock also allows the zio
	 * layer to access the keys, but this function should only be called during key loading,
	 * encrypted dataset mounting, encrypted dataset creation, etc. so this is probably ok. If it
	 * becomes a problem an RCU-like implementation could make sense here.
	 */
	rw_enter(&spa->spa_keystore.sk_kc_lock, RW_WRITER);
	
	//lookup the keychain in the tree of existing keychains
	ret = spa_keystore_keychain_hold_impl(spa, kcobj, tag, &kc);
	if(!ret){
		rw_exit(&spa->spa_keystore.sk_kc_lock);
		*kc_out = kc;
		return 0;
	}
	
	//lookup the wrapping key from the keystore
	ret = spa_keystore_wkey_hold_ddobj(spa, dd->dd_object, FTAG, &wkey);
	if(ret){
		ret = SET_ERROR(EPERM);
		goto error;
	}
	
	//read the keychain from disk
	ret = dsl_keychain_open(spa_get_dsl(spa)->dp_meta_objset, wkey, kcobj, tag, &kc);
	if(ret) goto error;
	
	//add the keychain to the keystore (this should always succeed since we made sure it didn't exist before)
	avl_find(&spa->spa_keystore.sk_keychains, kc, &where);
	avl_insert(&spa->spa_keystore.sk_keychains, kc, where);
	
	//release the wrapping key (the keychain now has a reference to it)
	dsl_wrapping_key_rele(wkey, FTAG);
	
	rw_exit(&spa->spa_keystore.sk_kc_lock);
	
	*kc_out = kc;
	return 0;
	
error:
	LOG_ERROR(ret, "");
	if(wkey) dsl_wrapping_key_rele(wkey, FTAG);
	rw_exit(&spa->spa_keystore.sk_kc_lock);
	
	*kc_out = NULL;
	return ret;
}

void spa_keystore_keychain_rele(spa_t *spa, dsl_keychain_t *kc, void *tag){
	rw_enter(&spa->spa_keystore.sk_kc_lock, RW_WRITER);
	
	if(refcount_remove(&kc->kc_refcnt, tag) == 0){
		avl_remove(&spa->spa_keystore.sk_keychains, kc);
		dsl_keychain_free(kc);
	}
	
	rw_exit(&spa->spa_keystore.sk_kc_lock);
}

int spa_keystore_load_wkey_impl(spa_t *spa, dsl_wrapping_key_t *wkey){
	int ret;
	avl_index_t where;
	dsl_wrapping_key_t *found_wkey;
	
	LOG_DEBUG("inserting wkey: ddobj = %u", (unsigned)wkey->wk_ddobj);
	
	rw_enter(&spa->spa_keystore.sk_wkeys_lock, RW_WRITER);
	
	//insert the wrapping key into the keystore
	found_wkey = avl_find(&spa->spa_keystore.sk_wkeys, wkey, &where);
	if(found_wkey){
		ret = SET_ERROR(EEXIST);
		goto error_unlock;
	}
	avl_insert(&spa->spa_keystore.sk_wkeys, wkey, where);
	
	rw_exit(&spa->spa_keystore.sk_wkeys_lock);
	
	return 0;
	
error_unlock:
	LOG_ERROR(ret, "");
	rw_exit(&spa->spa_keystore.sk_wkeys_lock);
	return ret;
}

int spa_keystore_load_wkey(spa_t *spa, const char *dsname, dsl_crypto_params_t *dcp){
	int ret;
	dsl_dir_t *dd = NULL;
	dsl_keychain_t *kc = NULL;
	dsl_wrapping_key_t *wkey = dcp->cp_wkey;
	dsl_pool_t *dp = spa_get_dsl(spa);
	
	if(!dcp->cp_wkey) return SET_ERROR(EINVAL);
	if(dcp->cp_crypt || dcp->cp_keysource || dcp->cp_salt) return SET_ERROR(EINVAL);
	if(dcp->cp_cmd) return SET_ERROR(EINVAL);
	
	//hold the dsl dir
	ret = dsl_dir_hold(dp, dsname, FTAG, &dd, NULL);
	if(ret) return ret;
	
	LOG_DEBUG("loading key ddobj = %u", (unsigned)dd->dd_object);
	
	//initialize the wkey's ddobj
	wkey->wk_ddobj = dd->dd_object;
	
	//verify that the keychain is correct by opening its keychain
	ret = dsl_keychain_open(dp->dp_meta_objset, wkey, dsl_dir_phys(dd)->dd_keychain_obj, FTAG, &kc);
	if(ret) goto error;
	
	//insert the wrapping key into the keystore
	ret = spa_keystore_load_wkey_impl(spa, wkey);
	if(ret) goto error;
	
	dsl_keychain_rele(kc, FTAG);
	dsl_dir_rele(dd, FTAG);
	
	return 0;
	
error:
	LOG_ERROR(ret, "");
	if(wkey) dsl_wrapping_key_free(wkey);
	if(kc) dsl_keychain_rele(kc, FTAG);
	dsl_dir_rele(dd, FTAG);
	
	return ret;
}

int spa_keystore_unload_wkey(spa_t *spa, const char *dsname){
	int ret = 0;
	dsl_dir_t *dd = NULL;
	dsl_wrapping_key_t search_wkey;
	dsl_wrapping_key_t *found_wkey;
	
	//hold the dsl dir
	ret = dsl_dir_hold(spa_get_dsl(spa), dsname, FTAG, &dd, NULL);
	if(ret) return ret;
	
	LOG_DEBUG("unloading key ddobj = %u", (unsigned)dd->dd_object);
	
	//init the search wrapping key
	search_wkey.wk_ddobj = dd->dd_object;
	
	rw_enter(&spa->spa_keystore.sk_wkeys_lock, RW_WRITER);
	
	//remove the wrapping key
	found_wkey = avl_find(&spa->spa_keystore.sk_wkeys, &search_wkey, NULL);
	if(!found_wkey){
		ret = SET_ERROR(ENOENT);
		goto error_unlock;
	}else if(refcount_count(&found_wkey->wk_refcnt) != 0){
		ret = SET_ERROR(EBUSY);
		goto error_unlock;
	}
	avl_remove(&spa->spa_keystore.sk_wkeys, found_wkey);
	
	LOG_DEBUG("unloaded key ddobj = %u", (unsigned)dd->dd_object);
	
	rw_exit(&spa->spa_keystore.sk_wkeys_lock);
	dsl_dir_rele(dd, FTAG);
	
	//free the wrapping key
	dsl_wrapping_key_free(found_wkey);
	
	return 0;

error_unlock:
	LOG_ERROR(ret, "");
	rw_exit(&spa->spa_keystore.sk_wkeys_lock);
	return ret;
}

static int dsl_keychain_check_impl(const char *dsname, boolean_t needs_root, dmu_tx_t *tx){
	int ret;
	dsl_dir_t *dd;
	dsl_keychain_t *kc = NULL;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	
	//hold the dd
	ret = dsl_dir_hold(dp, dsname, FTAG, &dd, NULL);
	if(ret) return ret;
	
	//check that this dd has a keychain
	if(dsl_dir_phys(dd)->dd_keychain_obj == 0){
		ret = SET_ERROR(EINVAL);
		goto error;
	}
	
	//make sure the keychain is loaded / loadable
	ret = spa_keystore_keychain_hold_dd(dp->dp_spa, dd, FTAG, &kc);
	if(ret) goto error;
	
	ASSERT(kc->kc_wkey != NULL);
	
	//make sure this is an encryption root if it is required
	if(needs_root && kc->kc_wkey->wk_ddobj != dd->dd_object){
		ret = SET_ERROR(EINVAL);
		goto error;
	}
	
	spa_keystore_keychain_rele(dp->dp_spa, kc, FTAG);
	dsl_dir_rele(dd, FTAG);
	
	return 0;
	
error:
	LOG_ERROR(ret, "");
	if(kc) spa_keystore_keychain_rele(dp->dp_spa, kc, FTAG);
	dsl_dir_rele(dd, FTAG);
	
	return ret;
}

static int dsl_keychain_add_key_check(void *arg, dmu_tx_t *tx){
	return dsl_keychain_check_impl((const char *)arg, B_FALSE, tx);
}

static void dsl_keychain_add_key_sync(void *arg, dmu_tx_t *tx){
	dsl_pool_t *dp = dmu_tx_pool(tx);
	const char *dsname = arg;
	dsl_dir_t *dd;
	dsl_keychain_t *kc;
	dsl_keychain_entry_t *kce;
		
	//find the keychain
	VERIFY0(dsl_dir_hold(dp, dsname, FTAG, &dd, NULL));
	VERIFY0(spa_keystore_keychain_hold_dd(dp->dp_spa, dd, FTAG, &kc));

	//generate and add a key to the keychain
	VERIFY0(dsl_keychain_add_key_sync_impl(kc, kc->kc_crypt, tx->tx_txg, tx));
	
	LOG_DEBUG("added keychain entry sucessfully");
	for(kce = list_head(&kc->kc_entries); kce; kce = list_next(&kc->kc_entries, kce)){
		LOG_DEBUG("kce %lu", (unsigned long)kce->ke_txgid);
	}
	
	spa_keystore_keychain_rele(dp->dp_spa, kc, FTAG);
	dsl_dir_rele(dd, FTAG);
}

int spa_keystore_keychain_add_key(spa_t *spa, const char *dsname){
	return dsl_sync_task(dsname, dsl_keychain_add_key_check, dsl_keychain_add_key_sync, (void *)dsname, 1, ZFS_SPACE_CHECK_NORMAL);
}

typedef struct spa_keystore_rewrap_args {
	const char *skra_dsname;
	dsl_crypto_params_t *skra_cp;
} spa_keystore_rewrap_args_t;

static int spa_keystore_rewrap_check(void *arg, dmu_tx_t *tx){
	spa_keystore_rewrap_args_t *skra = arg;
	
	if(skra->skra_cp->cp_crypt != ZIO_CRYPT_INHERIT) return SET_ERROR(EINVAL);
	if(!skra->skra_cp || !skra->skra_cp->cp_wkey) return SET_ERROR(EINVAL);
	if(skra->skra_cp->cp_cmd) return SET_ERROR(EINVAL);
	
	return dsl_keychain_check_impl(skra->skra_dsname, B_TRUE, tx);
}

static int spa_keystore_rewrap_sync_impl(uint64_t root_ddobj, uint64_t ddobj, dsl_wrapping_key_t *wkey, dmu_tx_t *tx){
	int ret;
	zap_cursor_t zc;
	zap_attribute_t za;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dir_t *dd = NULL, *inherit_dd = NULL;
	dsl_keychain_t *kc = NULL;
	dsl_keychain_entry_t *kce = NULL;
	
	ASSERT(RW_WRITE_HELD(&dp->dp_spa->spa_keystore.sk_wkeys_lock));
	
	//hold the dd
	ret = dsl_dir_hold_obj(dp, ddobj, NULL, FTAG, &dd);
	if(ret) return ret;
	
	//hold the dd we inherited the keysource from
	ret = dsl_dir_hold_keysource_source_dd(dd, FTAG, &inherit_dd);
	if(ret) goto error;
	
	//dont rewrap if this dsl dir didn't inherit from the root 
	if(inherit_dd->dd_object != root_ddobj){
		dsl_dir_rele(inherit_dd, FTAG);
		dsl_dir_rele(dd, FTAG);
		
		return 0;
	}
	
	LOG_DEBUG("rewrapping keychain ddobj = %u", (unsigned)ddobj);
	
	//get the keychain object for this dsl dir
	ret = spa_keystore_keychain_hold_dd(dp->dp_spa, dd, FTAG, &kc);
	if(ret) goto error;
	
	LOG_DEBUG("rewrapping keychain kcobj = %u", (unsigned)kc->kc_obj);
	
	//sync all keychain entries with the new wrapping key
	for(kce = list_head(&kc->kc_entries); kce; kce = list_next(&kc->kc_entries, kce)){
		LOG_DEBUG("rewrapping keychain entry txgid = %u", (unsigned)kce->ke_txgid);
		ret = dsl_keychain_entry_sync(kce, wkey, dsl_dir_phys(dd)->dd_keychain_obj, tx);
		if(ret) goto error;
	}
	
	LOG_DEBUG("replacing wkey ddobj = %u", (unsigned)wkey->wk_ddobj);
	
	//replace the wrapping key
	dsl_wrapping_key_hold(wkey, kc);
	dsl_wrapping_key_rele(kc->kc_wkey, kc);
	kc->kc_wkey = wkey;
	
	LOG_DEBUG("recursing");
	
	//recurse into all children
	for(zap_cursor_init(&zc, dp->dp_meta_objset, dsl_dir_phys(dd)->dd_child_dir_zapobj); zap_cursor_retrieve(&zc, &za) == 0; zap_cursor_advance(&zc)){
		ret = spa_keystore_rewrap_sync_impl(root_ddobj, za.za_first_integer, wkey, tx);
		if(ret) goto error;
	}
	zap_cursor_fini(&zc);
	
	LOG_DEBUG("done recursing");
	
	spa_keystore_keychain_rele(dp->dp_spa, kc, FTAG);
	dsl_dir_rele(inherit_dd, FTAG);
	dsl_dir_rele(dd, FTAG);
	
	return 0;
	
error:
	LOG_ERROR(ret, "");
	if(kc) spa_keystore_keychain_rele(dp->dp_spa, kc, FTAG);
	if(inherit_dd) dsl_dir_rele(inherit_dd, FTAG);
	if(dd) dsl_dir_rele(dd, FTAG);
	
	return ret;
}

static void spa_keystore_rewrap_sync(void *arg, dmu_tx_t *tx){
	dsl_dataset_t *ds;
	avl_index_t where;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	spa_t *spa = dp->dp_spa;
	spa_keystore_rewrap_args_t *skra = arg;
	dsl_wrapping_key_t *wkey = skra->skra_cp->cp_wkey;
	dsl_wrapping_key_t *found_wkey;
	const char *keysource = skra->skra_cp->cp_keysource;
	
	//create and initialize the wrapping key
	VERIFY0(dsl_dataset_hold(dp, skra->skra_dsname, FTAG, &ds));
	wkey->wk_ddobj = ds->ds_dir->dd_object;
	
	rw_enter(&spa->spa_keystore.sk_wkeys_lock, RW_WRITER);

	//recurse through all children under the dd and rewrap their keychain
	VERIFY0(spa_keystore_rewrap_sync_impl(ds->ds_dir->dd_object, ds->ds_dir->dd_object, wkey, tx));
	
	LOG_DEBUG("searching for key to replace ddobj = %u", (unsigned)ds->ds_dir->dd_object);
	
	//all references to the old wkey should be released now, replace the wrapping key
	found_wkey = avl_find(&spa->spa_keystore.sk_wkeys, wkey, NULL);
	avl_remove(&spa->spa_keystore.sk_wkeys, found_wkey);
	
	LOG_DEBUG("removed wkey from keystore ddobj = %u", (unsigned)found_wkey->wk_ddobj);
	
	avl_find(&spa->spa_keystore.sk_wkeys, wkey, &where);
	avl_insert(&spa->spa_keystore.sk_wkeys, wkey, where);
	
	LOG_DEBUG("inserted new wkey ddobj = %u", (unsigned)wkey->wk_ddobj);
	
	rw_exit(&spa->spa_keystore.sk_wkeys_lock);
	
	//set additional properties which can be sent along with this ioctl
	if(keysource) dsl_prop_set_sync_impl(ds, zfs_prop_to_name(ZFS_PROP_KEYSOURCE), ZPROP_SRC_LOCAL, 1, strlen(keysource) + 1, keysource, tx);
	dsl_prop_set_sync_impl(ds, zfs_prop_to_name(ZFS_PROP_SALT), ZPROP_SRC_LOCAL, 8, 1, &skra->skra_cp->cp_salt, tx);
	
	dsl_dataset_rele(ds, FTAG);
}

int spa_keystore_rewrap(spa_t *spa, const char *dsname, dsl_crypto_params_t *dcp){
	spa_keystore_rewrap_args_t skra;
	
	//initialize the args struct
	skra.skra_dsname = dsname;
	skra.skra_cp = dcp;
	
	//perform the actual work in syncing context
	return (dsl_sync_task(dsname, spa_keystore_rewrap_check, spa_keystore_rewrap_sync, &skra, 0, ZFS_SPACE_CHECK_NORMAL));
}

int spa_keystore_create_keychain_record(spa_t *spa, dsl_dataset_t *ds){
	int ret;
	avl_index_t where;
	dsl_keychain_record_t *kr = NULL, *found_kr;
	
	LOG_DEBUG("creating record for ddobj = %u", (unsigned)ds->ds_object);
	
	//allocate the record
	kr = kmem_alloc(sizeof(dsl_keychain_record_t), KM_SLEEP);
	if(!kr) return SET_ERROR(ENOMEM);
	
	//initialize the record
	ret = spa_keystore_keychain_hold_dd(spa, ds->ds_dir, kr, &kr->kr_keychain);
	if(ret) goto error;
	
	kr->kr_dsobj = ds->ds_object;
	
	rw_enter(&spa->spa_keystore.sk_kr_lock, RW_WRITER);
	
	//insert the wrapping key into the keystore
	found_kr = avl_find(&spa->spa_keystore.sk_keychain_recs, kr, &where);
	if(found_kr){
		ret = SET_ERROR(EEXIST);
		goto error_unlock;
	}
	avl_insert(&spa->spa_keystore.sk_keychain_recs, kr, where);
	
	rw_exit(&spa->spa_keystore.sk_kr_lock);
	
	return 0;
	
error_unlock:
	rw_exit(&spa->spa_keystore.sk_kr_lock);
error:
	LOG_ERROR(ret, "");
	if(kr) kmem_free(kr, sizeof(dsl_keychain_record_t));
	
	return ret;
}

int spa_keystore_remove_keychain_record(spa_t *spa, dsl_dataset_t *ds){
	int ret;
	dsl_keychain_record_t search_kr;
	dsl_keychain_record_t *found_kr;
	
	//init the search keychain record
	search_kr.kr_dsobj = ds->ds_object;
	
	rw_enter(&spa->spa_keystore.sk_kr_lock, RW_WRITER);
	
	//remove the record from the tree
	found_kr = avl_find(&spa->spa_keystore.sk_keychain_recs, &search_kr, NULL);
	if(found_kr == NULL){
		ret = SET_ERROR(ENOENT);
		goto error_unlock;
	}
	avl_remove(&spa->spa_keystore.sk_keychain_recs, found_kr);
	
	rw_exit(&spa->spa_keystore.sk_kr_lock);
	
	LOG_DEBUG("removed record for ddobj = %u", (unsigned)ds->ds_object);
	
	//destroy the keychain record
	spa_keystore_keychain_rele(spa, found_kr->kr_keychain, found_kr);
	kmem_free(found_kr, sizeof(dsl_keychain_record_t));
	
	return 0;

error_unlock:
	LOG_ERROR(ret, "");
	rw_exit(&spa->spa_keystore.sk_kr_lock);
	
	return ret;
}

int spa_keystore_hold_keychain_kr(spa_t *spa, uint64_t dsobj, dsl_keychain_t **kc_out){
	int ret;
	dsl_keychain_record_t search_kr;
	dsl_keychain_record_t *found_kr;
	
	//init the search keychain record
	search_kr.kr_dsobj = dsobj;
	
	rw_enter(&spa->spa_keystore.sk_kr_lock, RW_READER);
	
	//remove the record from the tree
	found_kr = avl_find(&spa->spa_keystore.sk_keychain_recs, &search_kr, NULL);
	if(found_kr == NULL){
		ret = SET_ERROR(ENOENT);
		goto error_unlock;
	}
	
	rw_exit(&spa->spa_keystore.sk_kr_lock);
	
	*kc_out = found_kr->kr_keychain;
	return 0;

error_unlock:
	LOG_ERROR(ret, "");
	rw_exit(&spa->spa_keystore.sk_kr_lock);
	
	*kc_out = NULL;
	return ret;
}

zfs_keystatus_t dsl_dataset_keystore_keystatus(dsl_dataset_t *ds){
	int ret;
	dsl_wrapping_key_t *wkey;
	
	//check if this dataset has a keychain
	if(dsl_dir_phys(ds->ds_dir)->dd_keychain_obj == 0) return ZFS_KEYSTATUS_NONE;
	
	//lookup the wrapping key. if it doesn't exist the key is unavailable
	ret = spa_keystore_wkey_hold_ddobj(ds->ds_dir->dd_pool->dp_spa, ds->ds_dir->dd_object, FTAG, &wkey);
	if(ret) return ZFS_KEYSTATUS_UNAVAILABLE;
	
	LOG_DEBUG("keystatus: ddobj = %u, refcount = %d", (unsigned)ds->ds_dir->dd_object, (int)refcount_count(&wkey->wk_refcnt));
	
	dsl_wrapping_key_rele(wkey, FTAG);
	
	return ZFS_KEYSTATUS_AVAILABLE;
}

int dmu_objset_create_encryption_check(dsl_dir_t *pdd, dsl_crypto_params_t *dcp){
	int ret;
	dsl_wrapping_key_t *wkey;
	uint64_t pcrypt;
	
	LOG_CRYPTO_PARAMS(dcp);
	
	ret = dsl_prop_get_dd(pdd, zfs_prop_to_name(ZFS_PROP_ENCRYPTION), 8, 1, &pcrypt, NULL, B_FALSE);
	if(ret) return ret;
	
	if(dcp->cp_crypt == ZIO_CRYPT_OFF && pcrypt != ZIO_CRYPT_OFF) return SET_ERROR(EINVAL);
	if(dcp->cp_crypt == ZIO_CRYPT_INHERIT && pcrypt == ZIO_CRYPT_OFF && (dcp->cp_salt || dcp->cp_keysource || dcp->cp_wkey)) return SET_ERROR(EINVAL);
	if(dcp->cp_crypt == ZIO_CRYPT_OFF && (dcp->cp_salt || dcp->cp_keysource || dcp->cp_wkey)) return SET_ERROR(EINVAL);
	if(dcp->cp_crypt != ZIO_CRYPT_INHERIT && dcp->cp_crypt != ZIO_CRYPT_OFF && pcrypt == ZIO_CRYPT_OFF && !dcp->cp_keysource) return SET_ERROR(EINVAL);
	if(dcp->cp_cmd) return SET_ERROR(EINVAL);
	
	if(!dcp->cp_wkey && pcrypt != ZIO_CRYPT_OFF){
		ret = spa_keystore_wkey_hold_ddobj(pdd->dd_pool->dp_spa, pdd->dd_object, FTAG, &wkey);
		if(ret) return (SET_ERROR(EPERM));
		
		dsl_wrapping_key_rele(wkey, FTAG);
	}
	
	return 0;
}

int dmu_objset_clone_encryption_check(dsl_dir_t *pdd, dsl_dir_t *odd, dsl_crypto_params_t *dcp){
	int ret;
	dsl_wrapping_key_t *wkey;
	uint64_t pcrypt = 0, ocrypt = 0;
	
	LOG_CRYPTO_PARAMS(dcp);
	
	ret = dsl_prop_get_dd(pdd, zfs_prop_to_name(ZFS_PROP_ENCRYPTION), 8, 1, &pcrypt, NULL, B_FALSE);
	if(ret) return ret;
	
	ret = dsl_prop_get_dd(odd, zfs_prop_to_name(ZFS_PROP_ENCRYPTION), 8, 1, &ocrypt, NULL, B_FALSE);
	if(ret) return ret;

	if(dcp->cp_crypt != ZIO_CRYPT_INHERIT) return SET_ERROR(EINVAL);
	if(pcrypt != ZIO_CRYPT_OFF && ocrypt == ZIO_CRYPT_OFF) return SET_ERROR(EINVAL);
	if(dcp->cp_cmd && dcp->cp_cmd != ZFS_IOC_CRYPTO_ADD_KEY) return SET_ERROR(EINVAL);
	
	if(!dcp->cp_wkey && pcrypt != ZIO_CRYPT_OFF){
		ret = spa_keystore_wkey_hold_ddobj(pdd->dd_pool->dp_spa, pdd->dd_object, FTAG, &wkey);
		if(ret) return (SET_ERROR(EPERM));
		
		dsl_wrapping_key_rele(wkey, FTAG);
	}
	
	return 0;
}

uint64_t dsl_keychain_create_sync(uint64_t crypt, dsl_wrapping_key_t *wkey, dmu_tx_t *tx){
	uint64_t kcobj;
	dsl_keychain_entry_t kce;
	
	//create the DSL Keychain zap object
	kcobj = zap_create_flags(tx->tx_pool->dp_meta_objset, 0, ZAP_FLAG_UINT64_KEY, DMU_OT_DSL_KEYCHAIN, 0, 0, DMU_OT_NONE, 0, tx);
	
	//initialize a keychain entry and sync it to disk
	VERIFY0(dsl_keychain_entry_init(&kce, crypt, tx->tx_txg));
	VERIFY0(dsl_keychain_entry_sync(&kce, wkey, kcobj, tx));
	
	//increment the encryption feature count
	spa_feature_incr(tx->tx_pool->dp_spa, SPA_FEATURE_ENCRYPTION, tx);

	return kcobj;
}

uint64_t dsl_keychain_clone_sync(dsl_dir_t *orig_dd, dsl_wrapping_key_t *wkey, boolean_t add_key, dmu_tx_t *tx){
	uint64_t kcobj;
	dsl_keychain_t *orig_kc;
	dsl_keychain_entry_t *kce, new_kce;
	spa_t *spa = orig_dd->dd_pool->dp_spa;

	//create the DSL Keychain zap object
	kcobj = zap_create_flags(tx->tx_pool->dp_meta_objset, 0, ZAP_FLAG_UINT64_KEY, DMU_OT_DSL_KEYCHAIN, 0, 0, DMU_OT_NONE, 0, tx);
	
	//get the original keychain
	VERIFY0(spa_keystore_keychain_hold_dd(spa, orig_dd, FTAG, &orig_kc));
	
	//add all the entries from the old keychain, wrapped with the new wkey
	for(kce = list_head(&orig_kc->kc_entries); kce; kce = list_next(&orig_kc->kc_entries, kce)){
		VERIFY0(dsl_keychain_entry_sync(kce, wkey, kcobj, tx));
	}
	
	//add a new key to the keychain if the option is specified
	if(add_key){
		VERIFY0(dsl_keychain_entry_init(&new_kce, orig_kc->kc_crypt, tx->tx_txg));
		VERIFY0(dsl_keychain_entry_sync(&new_kce, wkey, kcobj, tx));
	}
	
	//increment the encryption feature count
	spa_feature_incr(tx->tx_pool->dp_spa, SPA_FEATURE_ENCRYPTION, tx);
	
	spa_keystore_keychain_rele(spa, orig_kc, FTAG);
	
	return kcobj;
}

void dsl_keychain_destroy_sync(uint64_t kcobj, dmu_tx_t *tx){
	//destroy the keychain object
	VERIFY0(zap_destroy(tx->tx_pool->dp_meta_objset, kcobj, tx));
	
	//decrement the feature count
	spa_feature_decr(tx->tx_pool->dp_spa, SPA_FEATURE_ENCRYPTION, tx);
}