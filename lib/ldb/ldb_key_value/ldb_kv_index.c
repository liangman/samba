/*
   ldb database library

   Copyright (C) Andrew Tridgell  2004-2009

     ** NOTE! The following LGPL license applies to the ldb
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

/*
 *  Name: ldb
 *
 *  Component: ldb key value backend - indexing
 *
 *  Description: indexing routines for ldb key value backend
 *
 *  Author: Andrew Tridgell
 */

/*

LDB Index design and choice of key:
=======================================

LDB has index records held as LDB objects with a special record like:

dn: @INDEX:attr:value

value may be base64 encoded, if it is deemed not printable:

dn: @INDEX:attr::base64-value

In each record, there is two possible formats:

The original format is:
-----------------------

dn: @INDEX:NAME:DNSUPDATEPROXY
@IDXVERSION: 2
@IDX: CN=DnsUpdateProxy,CN=Users,DC=addom,DC=samba,DC=example,DC=com

In this format, @IDX is multi-valued, one entry for each match

The corrosponding entry is stored in a TDB record with key:

DN=CN=DNSUPDATEPROXY,CN=USERS,DC=ADDOM,DC=SAMBA,DC=EXAMPLE,DC=COM

(This allows a scope BASE search to directly find the record via
a simple casefold of the DN).

The original mixed-case DN is stored in the entry iself.


The new 'GUID index' format is:
-------------------------------

dn: @INDEX:NAME:DNSUPDATEPROXY
@IDXVERSION: 3
@IDX: <binary GUID>[<binary GUID>[...]]

The binary guid is 16 bytes, as bytes and not expanded as hexidecimal
or pretty-printed.  The GUID is chosen from the message to be stored
by the @IDXGUID attribute on @INDEXLIST.

If there are multiple values the @IDX value simply becomes longer,
in multiples of 16.

The corrosponding entry is stored in a TDB record with key:

GUID=<binary GUID>

This allows a very quick translation between the fixed-length index
values and the TDB key, while seperating entries from other data
in the TDB, should they be unlucky enough to start with the bytes of
the 'DN=' prefix.

Additionally, this allows a scope BASE search to directly find the
record via a simple match on a GUID= extended DN, controlled via
@IDX_DN_GUID on @INDEXLIST

Exception for special @ DNs:

@BASEINFO, @INDEXLIST and all other special DNs are stored as per the
original format, as they are never referenced in an index and are used
to bootstrap the database.


Control points for choice of index mode
---------------------------------------

The choice of index and TDB key mode is made based (for example, from
Samba) on entries in the @INDEXLIST DN:

dn: @INDEXLIST
@IDXGUID: objectGUID
@IDX_DN_GUID: GUID

By default, the original DN format is used.


Control points for choosing indexed attributes
----------------------------------------------

@IDXATTR controls if an attribute is indexed

dn: @INDEXLIST
@IDXATTR: samAccountName
@IDXATTR: nETBIOSName


C Override functions
--------------------

void ldb_schema_set_override_GUID_index(struct ldb_context *ldb,
                                        const char *GUID_index_attribute,
                                        const char *GUID_index_dn_component)

This is used, particularly in combination with the below, instead of
the @IDXGUID and @IDX_DN_GUID values in @INDEXLIST.

void ldb_schema_set_override_indexlist(struct ldb_context *ldb,
                                       bool one_level_indexes);
void ldb_schema_attribute_set_override_handler(struct ldb_context *ldb,
                                               ldb_attribute_handler_override_fn_t override,
                                               void *private_data);

When the above two functions are called in combination, the @INDEXLIST
values are not read from the DB, so
ldb_schema_set_override_GUID_index() must be called.

*/

#include "ldb_kv.h"
#include "../ldb_tdb/ldb_tdb.h"
#include "ldb_private.h"
#include "lib/util/binsearch.h"

struct dn_list {
	unsigned int count;
	struct ldb_val *dn;
	/*
	 * Do not optimise the intersection of this list,
	 * we must never return an entry not in this
	 * list.  This allows the index for
	 * SCOPE_ONELEVEL to be trusted.
	 */
	bool strict;
};

struct ldb_kv_idxptr {
	struct tdb_context *itdb;
	int error;
};

enum key_truncation {
	KEY_NOT_TRUNCATED,
	KEY_TRUNCATED,
};

static int ldb_kv_write_index_dn_guid(struct ldb_module *module,
				      const struct ldb_message *msg,
				      int add);
static int ldb_kv_index_dn_base_dn(struct ldb_module *module,
				   struct ldb_kv_private *ldb_kv,
				   struct ldb_dn *base_dn,
				   struct dn_list *dn_list,
				   enum key_truncation *truncation);

static void ldb_kv_dn_list_sort(struct ldb_kv_private *ldb_kv,
				struct dn_list *list);

/* we put a @IDXVERSION attribute on index entries. This
   allows us to tell if it was written by an older version
*/
#define LDB_KV_INDEXING_VERSION 2

#define LDB_KV_GUID_INDEXING_VERSION 3

static unsigned ldb_kv_max_key_length(struct ldb_kv_private *ldb_kv)
{
	if (ldb_kv->max_key_length == 0) {
		return UINT_MAX;
	}
	return ldb_kv->max_key_length;
}

/* enable the idxptr mode when transactions start */
int ldb_kv_index_transaction_start(struct ldb_module *module)
{
	struct ldb_kv_private *ldb_kv = talloc_get_type(
	    ldb_module_get_private(module), struct ldb_kv_private);
	ldb_kv->idxptr = talloc_zero(ldb_kv, struct ldb_kv_idxptr);
	if (ldb_kv->idxptr == NULL) {
		return ldb_oom(ldb_module_get_ctx(module));
	}

	return LDB_SUCCESS;
}

/*
  see if two ldb_val structures contain exactly the same data
  return -1 or 1 for a mismatch, 0 for match
*/
static int ldb_val_equal_exact_for_qsort(const struct ldb_val *v1,
					 const struct ldb_val *v2)
{
	if (v1->length > v2->length) {
		return -1;
	}
	if (v1->length < v2->length) {
		return 1;
	}
	return memcmp(v1->data, v2->data, v1->length);
}

/*
  see if two ldb_val structures contain exactly the same data
  return -1 or 1 for a mismatch, 0 for match
*/
static int ldb_val_equal_exact_ordered(const struct ldb_val v1,
				       const struct ldb_val *v2)
{
	if (v1.length > v2->length) {
		return -1;
	}
	if (v1.length < v2->length) {
		return 1;
	}
	return memcmp(v1.data, v2->data, v1.length);
}


/*
  find a entry in a dn_list, using a ldb_val. Uses a case sensitive
  binary-safe comparison for the 'dn' returns -1 if not found

  This is therefore safe when the value is a GUID in the future
 */
static int ldb_kv_dn_list_find_val(struct ldb_kv_private *ldb_kv,
				   const struct dn_list *list,
				   const struct ldb_val *v)
{
	unsigned int i;
	struct ldb_val *exact = NULL, *next = NULL;

	if (ldb_kv->cache->GUID_index_attribute == NULL) {
		for (i=0; i<list->count; i++) {
			if (ldb_val_equal_exact(&list->dn[i], v) == 1) {
				return i;
			}
		}
		return -1;
	}

	BINARY_ARRAY_SEARCH_GTE(list->dn, list->count,
				*v, ldb_val_equal_exact_ordered,
				exact, next);
	if (exact == NULL) {
		return -1;
	}
	/* Not required, but keeps the compiler quiet */
	if (next != NULL) {
		return -1;
	}

	i = exact - list->dn;
	return i;
}

/*
  find a entry in a dn_list. Uses a case sensitive comparison with the dn
  returns -1 if not found
 */
static int ldb_kv_dn_list_find_msg(struct ldb_kv_private *ldb_kv,
				   struct dn_list *list,
				   const struct ldb_message *msg)
{
	struct ldb_val v;
	const struct ldb_val *key_val;
	if (ldb_kv->cache->GUID_index_attribute == NULL) {
		const char *dn_str = ldb_dn_get_linearized(msg->dn);
		v.data = discard_const_p(unsigned char, dn_str);
		v.length = strlen(dn_str);
	} else {
		key_val = ldb_msg_find_ldb_val(
		    msg, ldb_kv->cache->GUID_index_attribute);
		if (key_val == NULL) {
			return -1;
		}
		v = *key_val;
	}
	return ldb_kv_dn_list_find_val(ldb_kv, list, &v);
}

/*
  this is effectively a cast function, but with lots of paranoia
  checks and also copes with CPUs that are fussy about pointer
  alignment
 */
static struct dn_list *ldb_kv_index_idxptr(struct ldb_module *module,
					   TDB_DATA rec,
					   bool check_parent)
{
	struct dn_list *list;
	if (rec.dsize != sizeof(void *)) {
		ldb_asprintf_errstring(ldb_module_get_ctx(module),
				       "Bad data size for idxptr %u", (unsigned)rec.dsize);
		return NULL;
	}
	/* note that we can't just use a cast here, as rec.dptr may
	   not be aligned sufficiently for a pointer. A cast would cause
	   platforms like some ARM CPUs to crash */
	memcpy(&list, rec.dptr, sizeof(void *));
	list = talloc_get_type(list, struct dn_list);
	if (list == NULL) {
		ldb_asprintf_errstring(ldb_module_get_ctx(module),
				       "Bad type '%s' for idxptr",
				       talloc_get_name(list));
		return NULL;
	}
	if (check_parent && list->dn && talloc_parent(list->dn) != list) {
		ldb_asprintf_errstring(ldb_module_get_ctx(module),
				       "Bad parent '%s' for idxptr",
				       talloc_get_name(talloc_parent(list->dn)));
		return NULL;
	}
	return list;
}

/*
  return the @IDX list in an index entry for a dn as a
  struct dn_list
 */
static int ldb_kv_dn_list_load(struct ldb_module *module,
			       struct ldb_kv_private *ldb_kv,
			       struct ldb_dn *dn,
			       struct dn_list *list)
{
	struct ldb_message *msg;
	int ret, version;
	struct ldb_message_element *el;
	TDB_DATA rec;
	struct dn_list *list2;
	TDB_DATA key;

	list->dn = NULL;
	list->count = 0;

	/* see if we have any in-memory index entries */
	if (ldb_kv->idxptr == NULL || ldb_kv->idxptr->itdb == NULL) {
		goto normal_index;
	}

	key.dptr = discard_const_p(unsigned char, ldb_dn_get_linearized(dn));
	key.dsize = strlen((char *)key.dptr);

	rec = tdb_fetch(ldb_kv->idxptr->itdb, key);
	if (rec.dptr == NULL) {
		goto normal_index;
	}

	/* we've found an in-memory index entry */
	list2 = ldb_kv_index_idxptr(module, rec, true);
	if (list2 == NULL) {
		free(rec.dptr);
		return LDB_ERR_OPERATIONS_ERROR;
	}
	free(rec.dptr);

	*list = *list2;
	return LDB_SUCCESS;

normal_index:
	msg = ldb_msg_new(list);
	if (msg == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	ret = ldb_kv_search_dn1(module,
				dn,
				msg,
				LDB_UNPACK_DATA_FLAG_NO_DATA_ALLOC |
				    LDB_UNPACK_DATA_FLAG_NO_DN);
	if (ret != LDB_SUCCESS) {
		talloc_free(msg);
		return ret;
	}

	el = ldb_msg_find_element(msg, LDB_KV_IDX);
	if (!el) {
		talloc_free(msg);
		return LDB_SUCCESS;
	}

	version = ldb_msg_find_attr_as_int(msg, LDB_KV_IDXVERSION, 0);

	/*
	 * we avoid copying the strings by stealing the list.  We have
	 * to steal msg onto el->values (which looks odd) because we
	 * asked for the memory to be allocated on msg, not on each
	 * value with LDB_UNPACK_DATA_FLAG_NO_DATA_ALLOC above
	 */
	if (ldb_kv->cache->GUID_index_attribute == NULL) {
		/* check indexing version number */
		if (version != LDB_KV_INDEXING_VERSION) {
			ldb_debug_set(ldb_module_get_ctx(module),
				      LDB_DEBUG_ERROR,
				      "Wrong DN index version %d "
				      "expected %d for %s",
				      version, LDB_KV_INDEXING_VERSION,
				      ldb_dn_get_linearized(dn));
			talloc_free(msg);
			return LDB_ERR_OPERATIONS_ERROR;
		}

		talloc_steal(el->values, msg);
		list->dn = talloc_steal(list, el->values);
		list->count = el->num_values;
	} else {
		unsigned int i;
		if (version != LDB_KV_GUID_INDEXING_VERSION) {
			/* This is quite likely during the DB startup
			   on first upgrade to using a GUID index */
			ldb_debug_set(ldb_module_get_ctx(module),
				      LDB_DEBUG_ERROR,
				      "Wrong GUID index version %d "
				      "expected %d for %s",
				      version, LDB_KV_GUID_INDEXING_VERSION,
				      ldb_dn_get_linearized(dn));
			talloc_free(msg);
			return LDB_ERR_OPERATIONS_ERROR;
		}

		if (el->num_values == 0) {
			talloc_free(msg);
			return LDB_ERR_OPERATIONS_ERROR;
		}

		if ((el->values[0].length % LDB_KV_GUID_SIZE) != 0) {
			talloc_free(msg);
			return LDB_ERR_OPERATIONS_ERROR;
		}

		list->count = el->values[0].length / LDB_KV_GUID_SIZE;
		list->dn = talloc_array(list, struct ldb_val, list->count);
		if (list->dn == NULL) {
			talloc_free(msg);
			return LDB_ERR_OPERATIONS_ERROR;
		}

		/*
		 * The actual data is on msg, due to
		 * LDB_UNPACK_DATA_FLAG_NO_DATA_ALLOC
		 */
		talloc_steal(list->dn, msg);
		for (i = 0; i < list->count; i++) {
			list->dn[i].data
				= &el->values[0].data[i * LDB_KV_GUID_SIZE];
			list->dn[i].length = LDB_KV_GUID_SIZE;
		}
	}

	/* We don't need msg->elements any more */
	talloc_free(msg->elements);
	return LDB_SUCCESS;
}

int ldb_kv_key_dn_from_idx(struct ldb_module *module,
			   struct ldb_kv_private *ldb_kv,
			   TALLOC_CTX *mem_ctx,
			   struct ldb_dn *dn,
			   struct ldb_val *ldb_key)
{
	struct ldb_context *ldb = ldb_module_get_ctx(module);
	int ret;
	int index = 0;
	enum key_truncation truncation = KEY_NOT_TRUNCATED;
	struct dn_list *list = talloc(mem_ctx, struct dn_list);
	if (list == NULL) {
		ldb_oom(ldb);
		return LDB_ERR_OPERATIONS_ERROR;
	}

	ret = ldb_kv_index_dn_base_dn(module, ldb_kv, dn, list, &truncation);
	if (ret != LDB_SUCCESS) {
		TALLOC_FREE(list);
		return ret;
	}

	if (list->count == 0) {
		TALLOC_FREE(list);
		return LDB_ERR_NO_SUCH_OBJECT;
	}

	if (list->count > 1 && truncation == KEY_NOT_TRUNCATED)  {
		const char *dn_str = ldb_dn_get_linearized(dn);
		ldb_asprintf_errstring(ldb_module_get_ctx(module),
				       __location__
				       ": Failed to read DN index "
				       "against %s for %s: too many "
				       "values (%u > 1)",
				       ldb_kv->cache->GUID_index_attribute,
				       dn_str,
				       list->count);
		TALLOC_FREE(list);
		return LDB_ERR_CONSTRAINT_VIOLATION;
	}

	if (list->count > 0 && truncation == KEY_TRUNCATED)  {
		/*
		 * DN key has been truncated, need to inspect the actual
		 * records to locate the actual DN
		 */
		int i;
		index = -1;
		for (i=0; i < list->count; i++) {
			uint8_t guid_key[LDB_KV_GUID_KEY_SIZE];
			struct ldb_val key = {
				.data = guid_key,
				.length = sizeof(guid_key)
			};
			const int flags = LDB_UNPACK_DATA_FLAG_NO_ATTRS;
			struct ldb_message *rec = ldb_msg_new(ldb);
			if (rec == NULL) {
				TALLOC_FREE(list);
				return LDB_ERR_OPERATIONS_ERROR;
			}

			ret = ldb_kv_idx_to_key(
			    module, ldb_kv, ldb, &list->dn[i], &key);
			if (ret != LDB_SUCCESS) {
				TALLOC_FREE(list);
				TALLOC_FREE(rec);
				return ret;
			}

			ret =
			    ldb_kv_search_key(module, ldb_kv, key, rec, flags);
			if (key.data != guid_key) {
				TALLOC_FREE(key.data);
			}
			if (ret == LDB_ERR_NO_SUCH_OBJECT) {
				/*
				 * the record has disappeared?
				 * yes, this can happen
				 */
				TALLOC_FREE(rec);
				continue;
			}

			if (ret != LDB_SUCCESS) {
				/* an internal error */
				TALLOC_FREE(rec);
				TALLOC_FREE(list);
				return LDB_ERR_OPERATIONS_ERROR;
			}

			/*
			 * We found the actual DN that we wanted from in the
			 * multiple values that matched the index
			 * (due to truncation), so return that.
			 *
			 */
			if (ldb_dn_compare(dn, rec->dn) == 0) {
				index = i;
				TALLOC_FREE(rec);
				break;
			}
		}

		/*
		 * We matched the index but the actual DN we wanted
		 * was not here.
		 */
		if (index == -1) {
			TALLOC_FREE(list);
			return LDB_ERR_NO_SUCH_OBJECT;
		}
	}

	/* The ldb_key memory is allocated by the caller */
	ret = ldb_kv_guid_to_key(module, ldb_kv, &list->dn[index], ldb_key);
	TALLOC_FREE(list);

	if (ret != LDB_SUCCESS) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	return LDB_SUCCESS;
}



/*
  save a dn_list into a full @IDX style record
 */
static int ldb_kv_dn_list_store_full(struct ldb_module *module,
				     struct ldb_kv_private *ldb_kv,
				     struct ldb_dn *dn,
				     struct dn_list *list)
{
	struct ldb_message *msg;
	int ret;

	msg = ldb_msg_new(module);
	if (!msg) {
		return ldb_module_oom(module);
	}

	msg->dn = dn;

	if (list->count == 0) {
		ret = ldb_kv_delete_noindex(module, msg);
		if (ret == LDB_ERR_NO_SUCH_OBJECT) {
			ret = LDB_SUCCESS;
		}
		talloc_free(msg);
		return ret;
	}

	if (ldb_kv->cache->GUID_index_attribute == NULL) {
		ret = ldb_msg_add_fmt(msg, LDB_KV_IDXVERSION, "%u",
				      LDB_KV_INDEXING_VERSION);
		if (ret != LDB_SUCCESS) {
			talloc_free(msg);
			return ldb_module_oom(module);
		}
	} else {
		ret = ldb_msg_add_fmt(msg, LDB_KV_IDXVERSION, "%u",
				      LDB_KV_GUID_INDEXING_VERSION);
		if (ret != LDB_SUCCESS) {
			talloc_free(msg);
			return ldb_module_oom(module);
		}
	}

	if (list->count > 0) {
		struct ldb_message_element *el;

		ret = ldb_msg_add_empty(msg, LDB_KV_IDX, LDB_FLAG_MOD_ADD, &el);
		if (ret != LDB_SUCCESS) {
			talloc_free(msg);
			return ldb_module_oom(module);
		}

		if (ldb_kv->cache->GUID_index_attribute == NULL) {
			el->values = list->dn;
			el->num_values = list->count;
		} else {
			struct ldb_val v;
			unsigned int i;
			el->values = talloc_array(msg,
						  struct ldb_val, 1);
			if (el->values == NULL) {
				talloc_free(msg);
				return ldb_module_oom(module);
			}

			v.data = talloc_array_size(el->values,
						   list->count,
						   LDB_KV_GUID_SIZE);
			if (v.data == NULL) {
				talloc_free(msg);
				return ldb_module_oom(module);
			}

			v.length = talloc_get_size(v.data);

			for (i = 0; i < list->count; i++) {
				if (list->dn[i].length !=
				    LDB_KV_GUID_SIZE) {
					talloc_free(msg);
					return ldb_module_operr(module);
				}
				memcpy(&v.data[LDB_KV_GUID_SIZE*i],
				       list->dn[i].data,
				       LDB_KV_GUID_SIZE);
			}
			el->values[0] = v;
			el->num_values = 1;
		}
	}

	ret = ldb_kv_store(module, msg, TDB_REPLACE);
	talloc_free(msg);
	return ret;
}

/*
  save a dn_list into the database, in either @IDX or internal format
 */
static int ldb_kv_dn_list_store(struct ldb_module *module,
				struct ldb_dn *dn,
				struct dn_list *list)
{
	struct ldb_kv_private *ldb_kv = talloc_get_type(
	    ldb_module_get_private(module), struct ldb_kv_private);
	TDB_DATA rec, key;
	int ret;
	struct dn_list *list2;

	if (ldb_kv->idxptr == NULL) {
		return ldb_kv_dn_list_store_full(module, ldb_kv, dn, list);
	}

	if (ldb_kv->idxptr->itdb == NULL) {
		ldb_kv->idxptr->itdb =
		    tdb_open(NULL, 1000, TDB_INTERNAL, O_RDWR, 0);
		if (ldb_kv->idxptr->itdb == NULL) {
			return LDB_ERR_OPERATIONS_ERROR;
		}
	}

	key.dptr = discard_const_p(unsigned char, ldb_dn_get_linearized(dn));
	if (key.dptr == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}
	key.dsize = strlen((char *)key.dptr);

	rec = tdb_fetch(ldb_kv->idxptr->itdb, key);
	if (rec.dptr != NULL) {
		list2 = ldb_kv_index_idxptr(module, rec, false);
		if (list2 == NULL) {
			free(rec.dptr);
			return LDB_ERR_OPERATIONS_ERROR;
		}
		free(rec.dptr);
		list2->dn = talloc_steal(list2, list->dn);
		list2->count = list->count;
		return LDB_SUCCESS;
	}

	list2 = talloc(ldb_kv->idxptr, struct dn_list);
	if (list2 == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}
	list2->dn = talloc_steal(list2, list->dn);
	list2->count = list->count;

	rec.dptr = (uint8_t *)&list2;
	rec.dsize = sizeof(void *);


	/*
	 * This is not a store into the main DB, but into an in-memory
	 * TDB, so we don't need a guard on ltdb->read_only
	 */
	ret = tdb_store(ldb_kv->idxptr->itdb, key, rec, TDB_INSERT);
	if (ret != 0) {
		return ltdb_err_map(tdb_error(ldb_kv->idxptr->itdb));
	}
	return LDB_SUCCESS;
}

/*
  traverse function for storing the in-memory index entries on disk
 */
static int ldb_kv_index_traverse_store(struct tdb_context *tdb,
				       TDB_DATA key,
				       TDB_DATA data,
				       void *state)
{
	struct ldb_module *module = state;
	struct ldb_kv_private *ldb_kv = talloc_get_type(
	    ldb_module_get_private(module), struct ldb_kv_private);
	struct ldb_dn *dn;
	struct ldb_context *ldb = ldb_module_get_ctx(module);
	struct ldb_val v;
	struct dn_list *list;

	list = ldb_kv_index_idxptr(module, data, true);
	if (list == NULL) {
		ldb_kv->idxptr->error = LDB_ERR_OPERATIONS_ERROR;
		return -1;
	}

	v.data = key.dptr;
	v.length = strnlen((char *)key.dptr, key.dsize);

	dn = ldb_dn_from_ldb_val(module, ldb, &v);
	if (dn == NULL) {
		ldb_asprintf_errstring(ldb, "Failed to parse index key %*.*s as an LDB DN", (int)v.length, (int)v.length, (const char *)v.data);
		ldb_kv->idxptr->error = LDB_ERR_OPERATIONS_ERROR;
		return -1;
	}

	ldb_kv->idxptr->error =
	    ldb_kv_dn_list_store_full(module, ldb_kv, dn, list);
	talloc_free(dn);
	if (ldb_kv->idxptr->error != 0) {
		return -1;
	}
	return 0;
}

/* cleanup the idxptr mode when transaction commits */
int ldb_kv_index_transaction_commit(struct ldb_module *module)
{
	struct ldb_kv_private *ldb_kv = talloc_get_type(
	    ldb_module_get_private(module), struct ldb_kv_private);
	int ret;

	struct ldb_context *ldb = ldb_module_get_ctx(module);

	ldb_reset_err_string(ldb);

	if (ldb_kv->idxptr->itdb) {
		tdb_traverse(
		    ldb_kv->idxptr->itdb, ldb_kv_index_traverse_store, module);
		tdb_close(ldb_kv->idxptr->itdb);
	}

	ret = ldb_kv->idxptr->error;
	if (ret != LDB_SUCCESS) {
		if (!ldb_errstring(ldb)) {
			ldb_set_errstring(ldb, ldb_strerror(ret));
		}
		ldb_asprintf_errstring(ldb, "Failed to store index records in transaction commit: %s", ldb_errstring(ldb));
	}

	talloc_free(ldb_kv->idxptr);
	ldb_kv->idxptr = NULL;
	return ret;
}

/* cleanup the idxptr mode when transaction cancels */
int ldb_kv_index_transaction_cancel(struct ldb_module *module)
{
	struct ldb_kv_private *ldb_kv = talloc_get_type(
	    ldb_module_get_private(module), struct ldb_kv_private);
	if (ldb_kv->idxptr && ldb_kv->idxptr->itdb) {
		tdb_close(ldb_kv->idxptr->itdb);
	}
	talloc_free(ldb_kv->idxptr);
	ldb_kv->idxptr = NULL;
	return LDB_SUCCESS;
}


/*
  return the dn key to be used for an index
  the caller is responsible for freeing
*/
static struct ldb_dn *ldb_kv_index_key(struct ldb_context *ldb,
				       struct ldb_kv_private *ldb_kv,
				       const char *attr,
				       const struct ldb_val *value,
				       const struct ldb_schema_attribute **ap,
				       enum key_truncation *truncation)
{
	struct ldb_dn *ret;
	struct ldb_val v;
	const struct ldb_schema_attribute *a = NULL;
	char *attr_folded = NULL;
	const char *attr_for_dn = NULL;
	int r;
	bool should_b64_encode;

	unsigned int max_key_length = ldb_kv_max_key_length(ldb_kv);
	size_t key_len = 0;
	size_t attr_len = 0;
	const size_t indx_len = sizeof(LDB_KV_INDEX) - 1;
	unsigned frmt_len = 0;
	const size_t additional_key_length = 4;
	unsigned int num_separators = 3; /* Estimate for overflow check */
	const size_t min_data = 1;
	const size_t min_key_length = additional_key_length
		+ indx_len + num_separators + min_data;

	if (attr[0] == '@') {
		attr_for_dn = attr;
		v = *value;
		if (ap != NULL) {
			*ap = NULL;
		}
	} else {
		attr_folded = ldb_attr_casefold(ldb, attr);
		if (!attr_folded) {
			return NULL;
		}

		attr_for_dn = attr_folded;

		a = ldb_schema_attribute_by_name(ldb, attr);
		if (ap) {
			*ap = a;
		}
		r = a->syntax->canonicalise_fn(ldb, ldb, value, &v);
		if (r != LDB_SUCCESS) {
			const char *errstr = ldb_errstring(ldb);
			/* canonicalisation can be refused. For
			   example, a attribute that takes wildcards
			   will refuse to canonicalise if the value
			   contains a wildcard */
			ldb_asprintf_errstring(ldb,
					       "Failed to create index "
					       "key for attribute '%s':%s%s%s",
					       attr, ldb_strerror(r),
					       (errstr?":":""),
					       (errstr?errstr:""));
			talloc_free(attr_folded);
			return NULL;
		}
	}
	attr_len = strlen(attr_for_dn);

	/*
	 * Check if there is any hope this will fit into the DB.
	 * Overflow here is not actually critical the code below
	 * checks again to make the printf and the DB does another
	 * check for too long keys
	 */
	if (max_key_length - attr_len < min_key_length) {
		ldb_asprintf_errstring(
			ldb,
			__location__ ": max_key_length "
			"is too small (%u) < (%u)",
			max_key_length,
			(unsigned)(min_key_length + attr_len));
		talloc_free(attr_folded);
		return NULL;
	}

	/*
	 * ltdb_key_dn() makes something 4 bytes longer, it adds a leading
	 * "DN=" and a trailing string terminator
	 */
	max_key_length -= additional_key_length;

	/*
	 * We do not base 64 encode a DN in a key, it has already been
	 * casefold and lineraized, that is good enough.  That already
	 * avoids embedded NUL etc.
	 */
	if (ldb_kv->cache->GUID_index_attribute != NULL) {
		if (strcmp(attr, LDB_KV_IDXDN) == 0) {
			should_b64_encode = false;
		} else if (strcmp(attr, LDB_KV_IDXONE) == 0) {
			/*
			 * We can only change the behaviour for IDXONE
			 * when the GUID index is enabled
			 */
			should_b64_encode = false;
		} else {
			should_b64_encode
				= ldb_should_b64_encode(ldb, &v);
		}
	} else {
		should_b64_encode = ldb_should_b64_encode(ldb, &v);
	}

	if (should_b64_encode) {
		size_t vstr_len = 0;
		char *vstr = ldb_base64_encode(ldb, (char *)v.data, v.length);
		if (!vstr) {
			talloc_free(attr_folded);
			return NULL;
		}
		vstr_len = strlen(vstr);
		/*
		 * Overflow here is not critical as we only use this
		 * to choose the printf truncation
		 */
		key_len = num_separators + indx_len + attr_len + vstr_len;
		if (key_len > max_key_length) {
			size_t excess = key_len - max_key_length;
			frmt_len = vstr_len - excess;
			*truncation = KEY_TRUNCATED;
			/*
			* Truncated keys are placed in a separate key space
			* from the non truncated keys
			* Note: the double hash "##" is not a typo and
			* indicates that the following value is base64 encoded
			*/
			ret = ldb_dn_new_fmt(ldb, ldb, "%s#%s##%.*s",
					     LDB_KV_INDEX, attr_for_dn,
					     frmt_len, vstr);
		} else {
			frmt_len = vstr_len;
			*truncation = KEY_NOT_TRUNCATED;
			/*
			 * Note: the double colon "::" is not a typo and
			 * indicates that the following value is base64 encoded
			 */
			ret = ldb_dn_new_fmt(ldb, ldb, "%s:%s::%.*s",
					     LDB_KV_INDEX, attr_for_dn,
					     frmt_len, vstr);
		}
		talloc_free(vstr);
	} else {
		/* Only need two seperators */
		num_separators = 2;

		/*
		 * Overflow here is not critical as we only use this
		 * to choose the printf truncation
		 */
		key_len = num_separators + indx_len + attr_len + (int)v.length;
		if (key_len > max_key_length) {
			size_t excess = key_len - max_key_length;
			frmt_len = v.length - excess;
			*truncation = KEY_TRUNCATED;
			/*
			 * Truncated keys are placed in a separate key space
			 * from the non truncated keys
			 */
			ret = ldb_dn_new_fmt(ldb, ldb, "%s#%s#%.*s",
					     LDB_KV_INDEX, attr_for_dn,
					     frmt_len, (char *)v.data);
		} else {
			frmt_len = v.length;
			*truncation = KEY_NOT_TRUNCATED;
			ret = ldb_dn_new_fmt(ldb, ldb, "%s:%s:%.*s",
					     LDB_KV_INDEX, attr_for_dn,
					     frmt_len, (char *)v.data);
		}
	}

	if (v.data != value->data) {
		talloc_free(v.data);
	}
	talloc_free(attr_folded);

	return ret;
}

/*
  see if a attribute value is in the list of indexed attributes
*/
static bool ldb_kv_is_indexed(struct ldb_module *module,
			      struct ldb_kv_private *ldb_kv,
			      const char *attr)
{
	struct ldb_context *ldb = ldb_module_get_ctx(module);
	unsigned int i;
	struct ldb_message_element *el;

	if ((ldb_kv->cache->GUID_index_attribute != NULL) &&
	    (ldb_attr_cmp(attr, ldb_kv->cache->GUID_index_attribute) == 0)) {
		/* Implicity covered, this is the index key */
		return false;
	}
	if (ldb->schema.index_handler_override) {
		const struct ldb_schema_attribute *a
			= ldb_schema_attribute_by_name(ldb, attr);

		if (a == NULL) {
			return false;
		}

		if (a->flags & LDB_ATTR_FLAG_INDEXED) {
			return true;
		} else {
			return false;
		}
	}

	if (!ldb_kv->cache->attribute_indexes) {
		return false;
	}

	el = ldb_msg_find_element(ldb_kv->cache->indexlist, LDB_KV_IDXATTR);
	if (el == NULL) {
		return false;
	}

	/* TODO: this is too expensive! At least use a binary search */
	for (i=0; i<el->num_values; i++) {
		if (ldb_attr_cmp((char *)el->values[i].data, attr) == 0) {
			return true;
		}
	}
	return false;
}

/*
  in the following logic functions, the return value is treated as
  follows:

     LDB_SUCCESS: we found some matching index values

     LDB_ERR_NO_SUCH_OBJECT: we know for sure that no object matches

     LDB_ERR_OPERATIONS_ERROR: indexing could not answer the call,
                               we'll need a full search
 */

/*
  return a list of dn's that might match a simple indexed search (an
  equality search only)
 */
static int ldb_kv_index_dn_simple(struct ldb_module *module,
				  struct ldb_kv_private *ldb_kv,
				  const struct ldb_parse_tree *tree,
				  struct dn_list *list)
{
	struct ldb_context *ldb;
	struct ldb_dn *dn;
	int ret;
	enum key_truncation truncation = KEY_NOT_TRUNCATED;

	ldb = ldb_module_get_ctx(module);

	list->count = 0;
	list->dn = NULL;

	/* if the attribute isn't in the list of indexed attributes then
	   this node needs a full search */
	if (!ldb_kv_is_indexed(module, ldb_kv, tree->u.equality.attr)) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	/* the attribute is indexed. Pull the list of DNs that match the
	   search criterion */
	dn = ldb_kv_index_key(ldb,
			      ldb_kv,
			      tree->u.equality.attr,
			      &tree->u.equality.value,
			      NULL,
			      &truncation);
	/*
	 * We ignore truncation here and allow multi-valued matches
	 * as ltdb_search_indexed will filter out the wrong one in
	 * ltdb_index_filter() which calls ldb_match_message().
	 */
	if (!dn) return LDB_ERR_OPERATIONS_ERROR;

	ret = ldb_kv_dn_list_load(module, ldb_kv, dn, list);
	talloc_free(dn);
	return ret;
}

static bool list_union(struct ldb_context *ldb,
		       struct ldb_kv_private *ldb_kv,
		       struct dn_list *list,
		       struct dn_list *list2);

/*
  return a list of dn's that might match a leaf indexed search
 */
static int ldb_kv_index_dn_leaf(struct ldb_module *module,
				struct ldb_kv_private *ldb_kv,
				const struct ldb_parse_tree *tree,
				struct dn_list *list)
{
	if (ldb_kv->disallow_dn_filter &&
	    (ldb_attr_cmp(tree->u.equality.attr, "dn") == 0)) {
		/* in AD mode we do not support "(dn=...)" search filters */
		list->dn = NULL;
		list->count = 0;
		return LDB_SUCCESS;
	}
	if (tree->u.equality.attr[0] == '@') {
		/* Do not allow a indexed search against an @ */
		list->dn = NULL;
		list->count = 0;
		return LDB_SUCCESS;
	}
	if (ldb_attr_dn(tree->u.equality.attr) == 0) {
		enum key_truncation truncation = KEY_NOT_TRUNCATED;
		bool valid_dn = false;
		struct ldb_dn *dn
			= ldb_dn_from_ldb_val(list,
					      ldb_module_get_ctx(module),
					      &tree->u.equality.value);
		if (dn == NULL) {
			/* If we can't parse it, no match */
			list->dn = NULL;
			list->count = 0;
			return LDB_SUCCESS;
		}

		valid_dn = ldb_dn_validate(dn);
		if (valid_dn == false) {
			/* If we can't parse it, no match */
			list->dn = NULL;
			list->count = 0;
			return LDB_SUCCESS;
		}

		/*
		 * Re-use the same code we use for a SCOPE_BASE
		 * search
		 *
		 * We can't call TALLOC_FREE(dn) as this must belong
		 * to list for the memory to remain valid.
		 */
		return ldb_kv_index_dn_base_dn(
		    module, ldb_kv, dn, list, &truncation);
		/*
		 * We ignore truncation here and allow multi-valued matches
		 * as ltdb_search_indexed will filter out the wrong one in
		 * ltdb_index_filter() which calls ldb_match_message().
		 */

	} else if ((ldb_kv->cache->GUID_index_attribute != NULL) &&
		   (ldb_attr_cmp(tree->u.equality.attr,
				 ldb_kv->cache->GUID_index_attribute) == 0)) {
		int ret;
		struct ldb_context *ldb = ldb_module_get_ctx(module);
		list->dn = talloc_array(list, struct ldb_val, 1);
		if (list->dn == NULL) {
			ldb_module_oom(module);
			return LDB_ERR_OPERATIONS_ERROR;
		}
		/*
		 * We need to go via the canonicalise_fn() to
		 * ensure we get the index in binary, rather
		 * than a string
		 */
		ret = ldb_kv->GUID_index_syntax->canonicalise_fn(
		    ldb, list->dn, &tree->u.equality.value, &list->dn[0]);
		if (ret != LDB_SUCCESS) {
			return LDB_ERR_OPERATIONS_ERROR;
		}
		list->count = 1;
		return LDB_SUCCESS;
	}

	return ldb_kv_index_dn_simple(module, ldb_kv, tree, list);
}


/*
  list intersection
  list = list & list2
*/
static bool list_intersect(struct ldb_context *ldb,
			   struct ldb_kv_private *ldb_kv,
			   struct dn_list *list,
			   const struct dn_list *list2)
{
	const struct dn_list *short_list, *long_list;
	struct dn_list *list3;
	unsigned int i;

	if (list->count == 0) {
		/* 0 & X == 0 */
		return true;
	}
	if (list2->count == 0) {
		/* X & 0 == 0 */
		list->count = 0;
		list->dn = NULL;
		return true;
	}

	/*
	 * In both of the below we check for strict and in that
	 * case do not optimise the intersection of this list,
	 * we must never return an entry not in this
	 * list.  This allows the index for
	 * SCOPE_ONELEVEL to be trusted.
	 */

	/* the indexing code is allowed to return a longer list than
	   what really matches, as all results are filtered by the
	   full expression at the end - this shortcut avoids a lot of
	   work in some cases */
	if (list->count < 2 && list2->count > 10 && list2->strict == false) {
		return true;
	}
	if (list2->count < 2 && list->count > 10 && list->strict == false) {
		list->count = list2->count;
		list->dn = list2->dn;
		/* note that list2 may not be the parent of list2->dn,
		   as list2->dn may be owned by ltdb->idxptr. In that
		   case we expect this reparent call to fail, which is
		   OK */
		talloc_reparent(list2, list, list2->dn);
		return true;
	}

	if (list->count > list2->count) {
		short_list = list2;
		long_list = list;
	} else {
		short_list = list;
		long_list = list2;
	}

	list3 = talloc_zero(list, struct dn_list);
	if (list3 == NULL) {
		return false;
	}

	list3->dn = talloc_array(list3, struct ldb_val,
				 MIN(list->count, list2->count));
	if (!list3->dn) {
		talloc_free(list3);
		return false;
	}
	list3->count = 0;

	for (i=0;i<short_list->count;i++) {
		/* For the GUID index case, this is a binary search */
		if (ldb_kv_dn_list_find_val(
			ldb_kv, long_list, &short_list->dn[i]) != -1) {
			list3->dn[list3->count] = short_list->dn[i];
			list3->count++;
		}
	}

	list->strict |= list2->strict;
	list->dn = talloc_steal(list, list3->dn);
	list->count = list3->count;
	talloc_free(list3);

	return true;
}


/*
  list union
  list = list | list2
*/
static bool list_union(struct ldb_context *ldb,
		       struct ldb_kv_private *ldb_kv,
		       struct dn_list *list,
		       struct dn_list *list2)
{
	struct ldb_val *dn3;
	unsigned int i = 0, j = 0, k = 0;

	if (list2->count == 0) {
		/* X | 0 == X */
		return true;
	}

	if (list->count == 0) {
		/* 0 | X == X */
		list->count = list2->count;
		list->dn = list2->dn;
		/* note that list2 may not be the parent of list2->dn,
		   as list2->dn may be owned by ltdb->idxptr. In that
		   case we expect this reparent call to fail, which is
		   OK */
		talloc_reparent(list2, list, list2->dn);
		return true;
	}

	/*
	 * Sort the lists (if not in GUID DN mode) so we can do
	 * the de-duplication during the merge
	 *
	 * NOTE: This can sort the in-memory index values, as list or
	 * list2 might not be a copy!
	 */
	ldb_kv_dn_list_sort(ldb_kv, list);
	ldb_kv_dn_list_sort(ldb_kv, list2);

	dn3 = talloc_array(list, struct ldb_val, list->count + list2->count);
	if (!dn3) {
		ldb_oom(ldb);
		return false;
	}

	while (i < list->count || j < list2->count) {
		int cmp;
		if (i >= list->count) {
			cmp = 1;
		} else if (j >= list2->count) {
			cmp = -1;
		} else {
			cmp = ldb_val_equal_exact_ordered(list->dn[i],
							  &list2->dn[j]);
		}

		if (cmp < 0) {
			/* Take list */
			dn3[k] = list->dn[i];
			i++;
			k++;
		} else if (cmp > 0) {
			/* Take list2 */
			dn3[k] = list2->dn[j];
			j++;
			k++;
		} else {
			/* Equal, take list */
			dn3[k] = list->dn[i];
			i++;
			j++;
			k++;
		}
	}

	list->dn = dn3;
	list->count = k;

	return true;
}

static int ldb_kv_index_dn(struct ldb_module *module,
			   struct ldb_kv_private *ldb_kv,
			   const struct ldb_parse_tree *tree,
			   struct dn_list *list);

/*
  process an OR list (a union)
 */
static int ldb_kv_index_dn_or(struct ldb_module *module,
			      struct ldb_kv_private *ldb_kv,
			      const struct ldb_parse_tree *tree,
			      struct dn_list *list)
{
	struct ldb_context *ldb;
	unsigned int i;

	ldb = ldb_module_get_ctx(module);

	list->dn = NULL;
	list->count = 0;

	for (i=0; i<tree->u.list.num_elements; i++) {
		struct dn_list *list2;
		int ret;

		list2 = talloc_zero(list, struct dn_list);
		if (list2 == NULL) {
			return LDB_ERR_OPERATIONS_ERROR;
		}

		ret = ldb_kv_index_dn(
		    module, ldb_kv, tree->u.list.elements[i], list2);

		if (ret == LDB_ERR_NO_SUCH_OBJECT) {
			/* X || 0 == X */
			talloc_free(list2);
			continue;
		}

		if (ret != LDB_SUCCESS) {
			/* X || * == * */
			talloc_free(list2);
			return ret;
		}

		if (!list_union(ldb, ldb_kv, list, list2)) {
			talloc_free(list2);
			return LDB_ERR_OPERATIONS_ERROR;
		}
	}

	if (list->count == 0) {
		return LDB_ERR_NO_SUCH_OBJECT;
	}

	return LDB_SUCCESS;
}


/*
  NOT an index results
 */
static int ldb_kv_index_dn_not(struct ldb_module *module,
			       struct ldb_kv_private *ldb_kv,
			       const struct ldb_parse_tree *tree,
			       struct dn_list *list)
{
	/* the only way to do an indexed not would be if we could
	   negate the not via another not or if we knew the total
	   number of database elements so we could know that the
	   existing expression covered the whole database.

	   instead, we just give up, and rely on a full index scan
	   (unless an outer & manages to reduce the list)
	*/
	return LDB_ERR_OPERATIONS_ERROR;
}

/*
 * These things are unique, so avoid a full scan if this is a search
 * by GUID, DN or a unique attribute
 */
static bool ldb_kv_index_unique(struct ldb_context *ldb,
				struct ldb_kv_private *ldb_kv,
				const char *attr)
{
	const struct ldb_schema_attribute *a;
	if (ldb_kv->cache->GUID_index_attribute != NULL) {
		if (ldb_attr_cmp(attr, ldb_kv->cache->GUID_index_attribute) ==
		    0) {
			return true;
		}
	}
	if (ldb_attr_dn(attr) == 0) {
		return true;
	}

	a = ldb_schema_attribute_by_name(ldb, attr);
	if (a->flags & LDB_ATTR_FLAG_UNIQUE_INDEX) {
		return true;
	}
	return false;
}

/*
  process an AND expression (intersection)
 */
static int ldb_kv_index_dn_and(struct ldb_module *module,
			       struct ldb_kv_private *ldb_kv,
			       const struct ldb_parse_tree *tree,
			       struct dn_list *list)
{
	struct ldb_context *ldb;
	unsigned int i;
	bool found;

	ldb = ldb_module_get_ctx(module);

	list->dn = NULL;
	list->count = 0;

	/* in the first pass we only look for unique simple
	   equality tests, in the hope of avoiding having to look
	   at any others */
	for (i=0; i<tree->u.list.num_elements; i++) {
		const struct ldb_parse_tree *subtree = tree->u.list.elements[i];
		int ret;

		if (subtree->operation != LDB_OP_EQUALITY ||
		    !ldb_kv_index_unique(
			ldb, ldb_kv, subtree->u.equality.attr)) {
			continue;
		}

		ret = ldb_kv_index_dn(module, ldb_kv, subtree, list);
		if (ret == LDB_ERR_NO_SUCH_OBJECT) {
			/* 0 && X == 0 */
			return LDB_ERR_NO_SUCH_OBJECT;
		}
		if (ret == LDB_SUCCESS) {
			/* a unique index match means we can
			 * stop. Note that we don't care if we return
			 * a few too many objects, due to later
			 * filtering */
			return LDB_SUCCESS;
		}
	}

	/* now do a full intersection */
	found = false;

	for (i=0; i<tree->u.list.num_elements; i++) {
		const struct ldb_parse_tree *subtree = tree->u.list.elements[i];
		struct dn_list *list2;
		int ret;

		list2 = talloc_zero(list, struct dn_list);
		if (list2 == NULL) {
			return ldb_module_oom(module);
		}

		ret = ldb_kv_index_dn(module, ldb_kv, subtree, list2);

		if (ret == LDB_ERR_NO_SUCH_OBJECT) {
			/* X && 0 == 0 */
			list->dn = NULL;
			list->count = 0;
			talloc_free(list2);
			return LDB_ERR_NO_SUCH_OBJECT;
		}

		if (ret != LDB_SUCCESS) {
			/* this didn't adding anything */
			talloc_free(list2);
			continue;
		}

		if (!found) {
			talloc_reparent(list2, list, list->dn);
			list->dn = list2->dn;
			list->count = list2->count;
			found = true;
		} else if (!list_intersect(ldb, ldb_kv, list, list2)) {
			talloc_free(list2);
			return LDB_ERR_OPERATIONS_ERROR;
		}

		if (list->count == 0) {
			list->dn = NULL;
			return LDB_ERR_NO_SUCH_OBJECT;
		}

		if (list->count < 2) {
			/* it isn't worth loading the next part of the tree */
			return LDB_SUCCESS;
		}
	}

	if (!found) {
		/* none of the attributes were indexed */
		return LDB_ERR_OPERATIONS_ERROR;
	}

	return LDB_SUCCESS;
}

/*
  return a list of matching objects using a one-level index
 */
static int ldb_kv_index_dn_attr(struct ldb_module *module,
				struct ldb_kv_private *ldb_kv,
				const char *attr,
				struct ldb_dn *dn,
				struct dn_list *list,
				enum key_truncation *truncation)
{
	struct ldb_context *ldb;
	struct ldb_dn *key;
	struct ldb_val val;
	int ret;

	ldb = ldb_module_get_ctx(module);

	/* work out the index key from the parent DN */
	val.data = (uint8_t *)((uintptr_t)ldb_dn_get_casefold(dn));
	if (val.data == NULL) {
		const char *dn_str = ldb_dn_get_linearized(dn);
		ldb_asprintf_errstring(ldb_module_get_ctx(module),
				       __location__
				       ": Failed to get casefold DN "
				       "from: %s",
				       dn_str);
		return LDB_ERR_OPERATIONS_ERROR;
	}
	val.length = strlen((char *)val.data);
	key = ldb_kv_index_key(ldb, ldb_kv, attr, &val, NULL, truncation);
	if (!key) {
		ldb_oom(ldb);
		return LDB_ERR_OPERATIONS_ERROR;
	}

	ret = ldb_kv_dn_list_load(module, ldb_kv, key, list);
	talloc_free(key);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	if (list->count == 0) {
		return LDB_ERR_NO_SUCH_OBJECT;
	}

	return LDB_SUCCESS;
}

/*
  return a list of matching objects using a one-level index
 */
static int ldb_kv_index_dn_one(struct ldb_module *module,
			       struct ldb_kv_private *ldb_kv,
			       struct ldb_dn *parent_dn,
			       struct dn_list *list,
			       enum key_truncation *truncation)
{
	/*
	 * Ensure we do not shortcut on intersection for this list.
	 * We must never be lazy and return an entry not in this
	 * list.  This allows the index for
	 * SCOPE_ONELEVEL to be trusted.
	 */

	list->strict = true;
	return ldb_kv_index_dn_attr(
	    module, ldb_kv, LDB_KV_IDXONE, parent_dn, list, truncation);
}

/*
  return a list of matching objects using the DN index
 */
static int ldb_kv_index_dn_base_dn(struct ldb_module *module,
				   struct ldb_kv_private *ldb_kv,
				   struct ldb_dn *base_dn,
				   struct dn_list *dn_list,
				   enum key_truncation *truncation)
{
	const struct ldb_val *guid_val = NULL;
	if (ldb_kv->cache->GUID_index_attribute == NULL) {
		dn_list->dn = talloc_array(dn_list, struct ldb_val, 1);
		if (dn_list->dn == NULL) {
			return ldb_module_oom(module);
		}
		dn_list->dn[0].data = discard_const_p(unsigned char,
						      ldb_dn_get_linearized(base_dn));
		if (dn_list->dn[0].data == NULL) {
			talloc_free(dn_list->dn);
			return ldb_module_oom(module);
		}
		dn_list->dn[0].length = strlen((char *)dn_list->dn[0].data);
		dn_list->count = 1;

		return LDB_SUCCESS;
	}

	if (ldb_kv->cache->GUID_index_dn_component != NULL) {
		guid_val = ldb_dn_get_extended_component(
		    base_dn, ldb_kv->cache->GUID_index_dn_component);
	}

	if (guid_val != NULL) {
		dn_list->dn = talloc_array(dn_list, struct ldb_val, 1);
		if (dn_list->dn == NULL) {
			return ldb_module_oom(module);
		}
		dn_list->dn[0].data = guid_val->data;
		dn_list->dn[0].length = guid_val->length;
		dn_list->count = 1;

		return LDB_SUCCESS;
	}

	return ldb_kv_index_dn_attr(
	    module, ldb_kv, LDB_KV_IDXDN, base_dn, dn_list, truncation);
}

/*
  return a list of dn's that might match a indexed search or
  an error. return LDB_ERR_NO_SUCH_OBJECT for no matches, or LDB_SUCCESS for matches
 */
static int ldb_kv_index_dn(struct ldb_module *module,
			   struct ldb_kv_private *ldb_kv,
			   const struct ldb_parse_tree *tree,
			   struct dn_list *list)
{
	int ret = LDB_ERR_OPERATIONS_ERROR;

	switch (tree->operation) {
	case LDB_OP_AND:
		ret = ldb_kv_index_dn_and(module, ldb_kv, tree, list);
		break;

	case LDB_OP_OR:
		ret = ldb_kv_index_dn_or(module, ldb_kv, tree, list);
		break;

	case LDB_OP_NOT:
		ret = ldb_kv_index_dn_not(module, ldb_kv, tree, list);
		break;

	case LDB_OP_EQUALITY:
		ret = ldb_kv_index_dn_leaf(module, ldb_kv, tree, list);
		break;

	case LDB_OP_SUBSTRING:
	case LDB_OP_GREATER:
	case LDB_OP_LESS:
	case LDB_OP_PRESENT:
	case LDB_OP_APPROX:
	case LDB_OP_EXTENDED:
		/* we can't index with fancy bitops yet */
		ret = LDB_ERR_OPERATIONS_ERROR;
		break;
	}

	return ret;
}

/*
  filter a candidate dn_list from an indexed search into a set of results
  extracting just the given attributes
*/
static int ldb_kv_index_filter(struct ldb_kv_private *ldb_kv,
			       const struct dn_list *dn_list,
			       struct ldb_kv_context *ac,
			       uint32_t *match_count,
			       enum key_truncation scope_one_truncation)
{
	struct ldb_context *ldb = ldb_module_get_ctx(ac->module);
	struct ldb_message *msg;
	struct ldb_message *filtered_msg;
	unsigned int i;
	unsigned int num_keys = 0;
	uint8_t previous_guid_key[LDB_KV_GUID_KEY_SIZE] = {};
	struct ldb_val *keys = NULL;

	/*
	 * We have to allocate the key list (rather than just walk the
	 * caller supplied list) as the callback could change the list
	 * (by modifying an indexed attribute hosted in the in-memory
	 * index cache!)
	 */
	keys = talloc_array(ac, struct ldb_val, dn_list->count);
	if (keys == NULL) {
		return ldb_module_oom(ac->module);
	}

	if (ldb_kv->cache->GUID_index_attribute != NULL) {
		/*
		 * We speculate that the keys will be GUID based and so
		 * pre-fill in enough space for a GUID (avoiding a pile of
		 * small allocations)
		 */
		struct guid_tdb_key {
			uint8_t guid_key[LDB_KV_GUID_KEY_SIZE];
		} *key_values = NULL;

		key_values = talloc_array(keys,
					  struct guid_tdb_key,
					  dn_list->count);

		if (key_values == NULL) {
			talloc_free(keys);
			return ldb_module_oom(ac->module);
		}
		for (i = 0; i < dn_list->count; i++) {
			keys[i].data = key_values[i].guid_key;
			keys[i].length = sizeof(key_values[i].guid_key);
		}
	} else {
		for (i = 0; i < dn_list->count; i++) {
			keys[i].data = NULL;
			keys[i].length = 0;
		}
	}

	for (i = 0; i < dn_list->count; i++) {
		int ret;

		ret = ldb_kv_idx_to_key(
		    ac->module, ldb_kv, keys, &dn_list->dn[i], &keys[num_keys]);
		if (ret != LDB_SUCCESS) {
			talloc_free(keys);
			return ret;
		}

		if (ldb_kv->cache->GUID_index_attribute != NULL) {
			/*
			 * If we are in GUID index mode, then the dn_list is
			 * sorted.  If we got a duplicate, forget about it, as
			 * otherwise we would send the same entry back more
			 * than once.
			 *
			 * This is needed in the truncated DN case, or if a
			 * duplicate was forced in via
			 * LDB_FLAG_INTERNAL_DISABLE_SINGLE_VALUE_CHECK
			 */

			if (memcmp(previous_guid_key,
				   keys[num_keys].data,
				   sizeof(previous_guid_key)) == 0) {
				continue;
			}

			memcpy(previous_guid_key,
			       keys[num_keys].data,
			       sizeof(previous_guid_key));
		}
		num_keys++;
	}


	/*
	 * Now that the list is a safe copy, send the callbacks
	 */
	for (i = 0; i < num_keys; i++) {
		int ret;
		bool matched;
		msg = ldb_msg_new(ac);
		if (!msg) {
			talloc_free(keys);
			return LDB_ERR_OPERATIONS_ERROR;
		}

		ret =
		    ldb_kv_search_key(ac->module,
				      ldb_kv,
				      keys[i],
				      msg,
				      LDB_UNPACK_DATA_FLAG_NO_DATA_ALLOC |
					  LDB_UNPACK_DATA_FLAG_NO_VALUES_ALLOC);
		if (ret == LDB_ERR_NO_SUCH_OBJECT) {
			/*
			 * the record has disappeared? yes, this can
			 * happen if the entry is deleted by something
			 * operating in the callback (not another
			 * process, as we have a read lock)
			 */
			talloc_free(msg);
			continue;
		}

		if (ret != LDB_SUCCESS && ret != LDB_ERR_NO_SUCH_OBJECT) {
			/* an internal error */
			talloc_free(keys);
			talloc_free(msg);
			return LDB_ERR_OPERATIONS_ERROR;
		}

		/*
		 * We trust the index for LDB_SCOPE_ONELEVEL
		 * unless the index key has been truncated.
		 *
		 * LDB_SCOPE_BASE is not passed in by our only caller.
		 */
		if (ac->scope == LDB_SCOPE_ONELEVEL &&
		    ldb_kv->cache->one_level_indexes &&
		    scope_one_truncation == KEY_NOT_TRUNCATED) {
			ret = ldb_match_message(ldb, msg, ac->tree,
						ac->scope, &matched);
		} else {
			ret = ldb_match_msg_error(ldb, msg,
						  ac->tree, ac->base,
						  ac->scope, &matched);
		}

		if (ret != LDB_SUCCESS) {
			talloc_free(keys);
			talloc_free(msg);
			return ret;
		}
		if (!matched) {
			talloc_free(msg);
			continue;
		}

		/* filter the attributes that the user wants */
		ret = ldb_kv_filter_attrs(ac, msg, ac->attrs, &filtered_msg);

		talloc_free(msg);

		if (ret == -1) {
			talloc_free(keys);
			return LDB_ERR_OPERATIONS_ERROR;
		}

		ret = ldb_module_send_entry(ac->req, filtered_msg, NULL);
		if (ret != LDB_SUCCESS) {
			/* Regardless of success or failure, the msg
			 * is the callbacks responsiblity, and should
			 * not be talloc_free()'ed */
			ac->request_terminated = true;
			talloc_free(keys);
			return ret;
		}

		(*match_count)++;
	}

	TALLOC_FREE(keys);
	return LDB_SUCCESS;
}

/*
  sort a DN list
 */
static void ldb_kv_dn_list_sort(struct ldb_kv_private *ltdb,
				struct dn_list *list)
{
	if (list->count < 2) {
		return;
	}

	/* We know the list is sorted when using the GUID index */
	if (ltdb->cache->GUID_index_attribute != NULL) {
		return;
	}

	TYPESAFE_QSORT(list->dn, list->count,
		       ldb_val_equal_exact_for_qsort);
}

/*
  search the database with a LDAP-like expression using indexes
  returns -1 if an indexed search is not possible, in which
  case the caller should call ltdb_search_full()
*/
int ldb_kv_search_indexed(struct ldb_kv_context *ac, uint32_t *match_count)
{
	struct ldb_context *ldb = ldb_module_get_ctx(ac->module);
	struct ldb_kv_private *ldb_kv = talloc_get_type(
	    ldb_module_get_private(ac->module), struct ldb_kv_private);
	struct dn_list *dn_list;
	int ret;
	enum ldb_scope index_scope;
	enum key_truncation scope_one_truncation = KEY_NOT_TRUNCATED;

	/* see if indexing is enabled */
	if (!ldb_kv->cache->attribute_indexes &&
	    !ldb_kv->cache->one_level_indexes && ac->scope != LDB_SCOPE_BASE) {
		/* fallback to a full search */
		return LDB_ERR_OPERATIONS_ERROR;
	}

	dn_list = talloc_zero(ac, struct dn_list);
	if (dn_list == NULL) {
		return ldb_module_oom(ac->module);
	}

	/*
	 * For the purposes of selecting the switch arm below, if we
	 * don't have a one-level index then treat it like a subtree
	 * search
	 */
	if (ac->scope == LDB_SCOPE_ONELEVEL &&
	    !ldb_kv->cache->one_level_indexes) {
		index_scope = LDB_SCOPE_SUBTREE;
	} else {
		index_scope = ac->scope;
	}

	switch (index_scope) {
	case LDB_SCOPE_BASE:
		/*
		 * The only caller will have filtered the operation out
		 * so we should never get here
		 */
		return ldb_operr(ldb);

	case LDB_SCOPE_ONELEVEL:

		/*
		 * First, load all the one-level child objects (regardless of
		 * whether they match the search filter or not). The database
		 * maintains a one-level index, so retrieving this is quick.
		 */
		ret = ldb_kv_index_dn_one(ac->module,
					  ldb_kv,
					  ac->base,
					  dn_list,
					  &scope_one_truncation);
		if (ret != LDB_SUCCESS) {
			talloc_free(dn_list);
			return ret;
		}

		/*
		 * If we have too many children, running ldb_kv_index_filter()
		 * over all the child objects can be quite expensive. So next
		 * we do a separate indexed query using the search filter.
		 *
		 * This should be quick, but it may return objects that are not
		 * the direct one-level child objects we're interested in.
		 *
		 * We only do this in the GUID index mode, which is
		 * O(n*log(m)) otherwise the intersection below will
		 * be too costly at O(n*m).
		 *
		 * We don't set a heuristic for 'too many' but instead
		 * do it always and rely on the index lookup being
		 * fast enough in the small case.
		 */
		if (ldb_kv->cache->GUID_index_attribute != NULL) {
			struct dn_list *indexed_search_result
				= talloc_zero(ac, struct dn_list);
			if (indexed_search_result == NULL) {
				talloc_free(dn_list);
				return ldb_module_oom(ac->module);
			}

			if (!ldb_kv->cache->attribute_indexes) {
				talloc_free(indexed_search_result);
				talloc_free(dn_list);
				return LDB_ERR_OPERATIONS_ERROR;
			}

			/*
			 * Try to do an indexed database search
			 */
			ret = ldb_kv_index_dn(
			    ac->module, ldb_kv, ac->tree,
			    indexed_search_result);

			/*
			 * We can stop if we're sure the object doesn't exist
			 */
			if (ret == LDB_ERR_NO_SUCH_OBJECT) {
				talloc_free(indexed_search_result);
				talloc_free(dn_list);
				return LDB_ERR_NO_SUCH_OBJECT;
			}

			/*
			 * Once we have a successful search result, we
			 * intersect it with the one-level children (dn_list).
			 * This should give us exactly the result we're after
			 * (we still need to run ldb_kv_index_filter() to
			 * handle potential index truncation cases).
			 *
			 * The indexed search may fail because we don't support
			 * indexing on that type of search operation, e.g.
			 * matching against '*'. In which case we fall through
			 * and run ldb_kv_index_filter() over all the one-level
			 * children (which is still better than bailing out here
			 * and falling back to a full DB scan).
			 */
			if (ret == LDB_SUCCESS) {
				if (!list_intersect(ldb,
						    ldb_kv,
						    dn_list,
						    indexed_search_result)) {
					talloc_free(indexed_search_result);
					talloc_free(dn_list);
					return LDB_ERR_OPERATIONS_ERROR;
				}
			}
		}
		break;

	case LDB_SCOPE_SUBTREE:
	case LDB_SCOPE_DEFAULT:
		if (!ldb_kv->cache->attribute_indexes) {
			talloc_free(dn_list);
			return LDB_ERR_OPERATIONS_ERROR;
		}
		/*
		 * Here we load the index for the tree.  We have no
		 * index for the subtree.
		 */
		ret = ldb_kv_index_dn(ac->module, ldb_kv, ac->tree, dn_list);
		if (ret != LDB_SUCCESS) {
			talloc_free(dn_list);
			return ret;
		}
		break;
	}

	/*
	 * It is critical that this function do the re-filter even
	 * on things found by the index as the index can over-match
	 * in cases of truncation (as well as when it decides it is
	 * not worth further filtering)
	 *
	 * If this changes, then the index code above would need to
	 * pass up a flag to say if any index was truncated during
	 * processing as the truncation here refers only to the
	 * SCOPE_ONELEVEL index.
	 */
	ret = ldb_kv_index_filter(
	    ldb_kv, dn_list, ac, match_count, scope_one_truncation);
	talloc_free(dn_list);
	return ret;
}

/**
 * @brief Add a DN in the index list of a given attribute name/value pair
 *
 * This function will add the DN in the index list for the index for
 * the given attribute name and value.
 *
 * @param[in]  module       A ldb_module structure
 *
 * @param[in]  dn           The string representation of the DN as it
 *                          will be stored in the index entry
 *
 * @param[in]  el           A ldb_message_element array, one of the entry
 *                          referred by the v_idx is the attribute name and
 *                          value pair which will be used to construct the
 *                          index name
 *
 * @param[in]  v_idx        The index of element in the el array to use
 *
 * @return                  An ldb error code
 */
static int ldb_kv_index_add1(struct ldb_module *module,
			     struct ldb_kv_private *ldb_kv,
			     const struct ldb_message *msg,
			     struct ldb_message_element *el,
			     int v_idx)
{
	struct ldb_context *ldb;
	struct ldb_dn *dn_key;
	int ret;
	const struct ldb_schema_attribute *a;
	struct dn_list *list;
	unsigned alloc_len;
	enum key_truncation truncation = KEY_TRUNCATED;


	ldb = ldb_module_get_ctx(module);

	list = talloc_zero(module, struct dn_list);
	if (list == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	dn_key = ldb_kv_index_key(
	    ldb, ldb_kv, el->name, &el->values[v_idx], &a, &truncation);
	if (!dn_key) {
		talloc_free(list);
		return LDB_ERR_OPERATIONS_ERROR;
	}
	/*
	 * Samba only maintains unique indexes on the objectSID and objectGUID
	 * so if a unique index key exceeds the maximum length there is a
	 * problem.
	 */
	if ((truncation == KEY_TRUNCATED) && (a != NULL &&
		(a->flags & LDB_ATTR_FLAG_UNIQUE_INDEX ||
		(el->flags & LDB_FLAG_INTERNAL_FORCE_UNIQUE_INDEX)))) {

		ldb_asprintf_errstring(
		    ldb,
		    __location__ ": unique index key on %s in %s, "
				 "exceeds maximum key length of %u (encoded).",
		    el->name,
		    ldb_dn_get_linearized(msg->dn),
		    ldb_kv->max_key_length);
		talloc_free(list);
		return LDB_ERR_CONSTRAINT_VIOLATION;
	}
	talloc_steal(list, dn_key);

	ret = ldb_kv_dn_list_load(module, ldb_kv, dn_key, list);
	if (ret != LDB_SUCCESS && ret != LDB_ERR_NO_SUCH_OBJECT) {
		talloc_free(list);
		return ret;
	}

	/*
	 * Check for duplicates in the @IDXDN DN -> GUID record
	 *
	 * This is very normal, it just means a duplicate DN creation
	 * was attempted, so don't set the error string or print scary
	 * messages.
	 */
	if (list->count > 0 &&
	    ldb_attr_cmp(el->name, LDB_KV_IDXDN) == 0 &&
	    truncation == KEY_NOT_TRUNCATED) {

		talloc_free(list);
		return LDB_ERR_CONSTRAINT_VIOLATION;

	} else if (list->count > 0
		   && ldb_attr_cmp(el->name, LDB_KV_IDXDN) == 0) {

		/*
		 * At least one existing entry in the DN->GUID index, which
		 * arises when the DN indexes have been truncated
		 *
		 * So need to pull the DN's to check if it's really a duplicate
		 */
		int i;
		for (i=0; i < list->count; i++) {
			uint8_t guid_key[LDB_KV_GUID_KEY_SIZE];
			struct ldb_val key = {
				.data = guid_key,
				.length = sizeof(guid_key)
			};
			const int flags = LDB_UNPACK_DATA_FLAG_NO_ATTRS;
			struct ldb_message *rec = ldb_msg_new(ldb);
			if (rec == NULL) {
				return LDB_ERR_OPERATIONS_ERROR;
			}

			ret = ldb_kv_idx_to_key(
			    module, ldb_kv, ldb, &list->dn[i], &key);
			if (ret != LDB_SUCCESS) {
				TALLOC_FREE(list);
				TALLOC_FREE(rec);
				return ret;
			}

			ret =
			    ldb_kv_search_key(module, ldb_kv, key, rec, flags);
			if (key.data != guid_key) {
				TALLOC_FREE(key.data);
			}
			if (ret == LDB_ERR_NO_SUCH_OBJECT) {
				/*
				 * the record has disappeared?
				 * yes, this can happen
				 */
				talloc_free(rec);
				continue;
			}

			if (ret != LDB_SUCCESS) {
				/* an internal error */
				TALLOC_FREE(rec);
				TALLOC_FREE(list);
				return LDB_ERR_OPERATIONS_ERROR;
			}
			/*
			 * The DN we are trying to add to the DB and index
			 * is already here, so we must deny the addition
			 */
			if (ldb_dn_compare(msg->dn, rec->dn) == 0) {
				TALLOC_FREE(rec);
				TALLOC_FREE(list);
				return LDB_ERR_CONSTRAINT_VIOLATION;
			}
		}
	}

	/*
	 * Check for duplicates in unique indexes
	 *
	 * We don't need to do a loop test like the @IDXDN case
	 * above as we have a ban on long unique index values
	 * at the start of this function.
	 */
	if (list->count > 0 &&
	    ((a != NULL
	      && (a->flags & LDB_ATTR_FLAG_UNIQUE_INDEX ||
		  (el->flags & LDB_FLAG_INTERNAL_FORCE_UNIQUE_INDEX))))) {
		/*
		 * We do not want to print info about a possibly
		 * confidential DN that the conflict was with in the
		 * user-visible error string
		 */

		if (ldb_kv->cache->GUID_index_attribute == NULL) {
			ldb_debug(ldb, LDB_DEBUG_WARNING,
				  __location__
				  ": unique index violation on %s in %s, "
				  "conficts with %*.*s in %s",
				  el->name, ldb_dn_get_linearized(msg->dn),
				  (int)list->dn[0].length,
				  (int)list->dn[0].length,
				  list->dn[0].data,
				  ldb_dn_get_linearized(dn_key));
		} else {
			/* This can't fail, gives a default at worst */
			const struct ldb_schema_attribute *attr =
			    ldb_schema_attribute_by_name(
				ldb, ldb_kv->cache->GUID_index_attribute);
			struct ldb_val v;
			ret = attr->syntax->ldif_write_fn(ldb, list,
							  &list->dn[0], &v);
			if (ret == LDB_SUCCESS) {
				ldb_debug(ldb,
					  LDB_DEBUG_WARNING,
					  __location__
					  ": unique index violation on %s in "
					  "%s, conficts with %s %*.*s in %s",
					  el->name,
					  ldb_dn_get_linearized(msg->dn),
					  ldb_kv->cache->GUID_index_attribute,
					  (int)v.length,
					  (int)v.length,
					  v.data,
					  ldb_dn_get_linearized(dn_key));
			}
		}
		ldb_asprintf_errstring(ldb,
				       __location__ ": unique index violation "
				       "on %s in %s",
				       el->name,
				       ldb_dn_get_linearized(msg->dn));
		talloc_free(list);
		return LDB_ERR_CONSTRAINT_VIOLATION;
	}

	/* overallocate the list a bit, to reduce the number of
	 * realloc trigered copies */
	alloc_len = ((list->count+1)+7) & ~7;
	list->dn = talloc_realloc(list, list->dn, struct ldb_val, alloc_len);
	if (list->dn == NULL) {
		talloc_free(list);
		return LDB_ERR_OPERATIONS_ERROR;
	}

	if (ldb_kv->cache->GUID_index_attribute == NULL) {
		const char *dn_str = ldb_dn_get_linearized(msg->dn);
		list->dn[list->count].data
			= (uint8_t *)talloc_strdup(list->dn, dn_str);
		if (list->dn[list->count].data == NULL) {
			talloc_free(list);
			return LDB_ERR_OPERATIONS_ERROR;
		}
		list->dn[list->count].length = strlen(dn_str);
	} else {
		const struct ldb_val *key_val;
		struct ldb_val *exact = NULL, *next = NULL;
		key_val = ldb_msg_find_ldb_val(
		    msg, ldb_kv->cache->GUID_index_attribute);
		if (key_val == NULL) {
			talloc_free(list);
			return ldb_module_operr(module);
		}

		if (key_val->length != LDB_KV_GUID_SIZE) {
			talloc_free(list);
			return ldb_module_operr(module);
		}

		BINARY_ARRAY_SEARCH_GTE(list->dn, list->count,
					*key_val, ldb_val_equal_exact_ordered,
					exact, next);

		/*
		 * Give a warning rather than fail, this could be a
		 * duplicate value in the record allowed by a caller
		 * forcing in the value with
		 * LDB_FLAG_INTERNAL_DISABLE_SINGLE_VALUE_CHECK
		 */
		if (exact != NULL && truncation == KEY_NOT_TRUNCATED) {
			/* This can't fail, gives a default at worst */
			const struct ldb_schema_attribute *attr =
			    ldb_schema_attribute_by_name(
				ldb, ldb_kv->cache->GUID_index_attribute);
			struct ldb_val v;
			ret = attr->syntax->ldif_write_fn(ldb, list,
							  exact, &v);
			if (ret == LDB_SUCCESS) {
				ldb_debug(ldb,
					  LDB_DEBUG_WARNING,
					  __location__
					  ": duplicate attribute value in %s "
					  "for index on %s, "
					  "duplicate of %s %*.*s in %s",
					  ldb_dn_get_linearized(msg->dn),
					  el->name,
					  ldb_kv->cache->GUID_index_attribute,
					  (int)v.length,
					  (int)v.length,
					  v.data,
					  ldb_dn_get_linearized(dn_key));
			}
		}

		if (next == NULL) {
			next = &list->dn[list->count];
		} else {
			memmove(&next[1], next,
				sizeof(*next) * (list->count - (next - list->dn)));
		}
		*next = ldb_val_dup(list->dn, key_val);
		if (next->data == NULL) {
			talloc_free(list);
			return ldb_module_operr(module);
		}
	}
	list->count++;

	ret = ldb_kv_dn_list_store(module, dn_key, list);

	talloc_free(list);

	return ret;
}

/*
  add index entries for one elements in a message
 */
static int ldb_kv_index_add_el(struct ldb_module *module,
			       struct ldb_kv_private *ldb_kv,
			       const struct ldb_message *msg,
			       struct ldb_message_element *el)
{
	unsigned int i;
	for (i = 0; i < el->num_values; i++) {
		int ret = ldb_kv_index_add1(module, ldb_kv, msg, el, i);
		if (ret != LDB_SUCCESS) {
			return ret;
		}
	}

	return LDB_SUCCESS;
}

/*
  add index entries for all elements in a message
 */
static int ldb_kv_index_add_all(struct ldb_module *module,
				struct ldb_kv_private *ldb_kv,
				const struct ldb_message *msg)
{
	struct ldb_message_element *elements = msg->elements;
	unsigned int i;
	const char *dn_str;
	int ret;

	if (ldb_dn_is_special(msg->dn)) {
		return LDB_SUCCESS;
	}

	dn_str = ldb_dn_get_linearized(msg->dn);
	if (dn_str == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	ret = ldb_kv_write_index_dn_guid(module, msg, 1);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	if (!ldb_kv->cache->attribute_indexes) {
		/* no indexed fields */
		return LDB_SUCCESS;
	}

	for (i = 0; i < msg->num_elements; i++) {
		if (!ldb_kv_is_indexed(module, ldb_kv, elements[i].name)) {
			continue;
		}
		ret = ldb_kv_index_add_el(module, ldb_kv, msg, &elements[i]);
		if (ret != LDB_SUCCESS) {
			struct ldb_context *ldb = ldb_module_get_ctx(module);
			ldb_asprintf_errstring(ldb,
					       __location__ ": Failed to re-index %s in %s - %s",
					       elements[i].name, dn_str,
					       ldb_errstring(ldb));
			return ret;
		}
	}

	return LDB_SUCCESS;
}


/*
  insert a DN index for a message
*/
static int ldb_kv_modify_index_dn(struct ldb_module *module,
				  struct ldb_kv_private *ldb_kv,
				  const struct ldb_message *msg,
				  struct ldb_dn *dn,
				  const char *index,
				  int add)
{
	struct ldb_message_element el;
	struct ldb_val val;
	int ret;

	val.data = (uint8_t *)((uintptr_t)ldb_dn_get_casefold(dn));
	if (val.data == NULL) {
		const char *dn_str = ldb_dn_get_linearized(dn);
		ldb_asprintf_errstring(ldb_module_get_ctx(module),
				       __location__ ": Failed to modify %s "
						    "against %s in %s: failed "
						    "to get casefold DN",
				       index,
				       ldb_kv->cache->GUID_index_attribute,
				       dn_str);
		return LDB_ERR_OPERATIONS_ERROR;
	}

	val.length = strlen((char *)val.data);
	el.name = index;
	el.values = &val;
	el.num_values = 1;

	if (add) {
		ret = ldb_kv_index_add1(module, ldb_kv, msg, &el, 0);
	} else { /* delete */
		ret = ldb_kv_index_del_value(module, ldb_kv, msg, &el, 0);
	}

	if (ret != LDB_SUCCESS) {
		struct ldb_context *ldb = ldb_module_get_ctx(module);
		const char *dn_str = ldb_dn_get_linearized(dn);
		ldb_asprintf_errstring(ldb,
				       __location__ ": Failed to modify %s "
						    "against %s in %s - %s",
				       index,
				       ldb_kv->cache->GUID_index_attribute,
				       dn_str,
				       ldb_errstring(ldb));
		return ret;
	}
	return ret;
}

/*
  insert a one level index for a message
*/
static int ldb_kv_index_onelevel(struct ldb_module *module,
				 const struct ldb_message *msg,
				 int add)
{
	struct ldb_kv_private *ldb_kv = talloc_get_type(
	    ldb_module_get_private(module), struct ldb_kv_private);
	struct ldb_dn *pdn;
	int ret;

	/* We index for ONE Level only if requested */
	if (!ldb_kv->cache->one_level_indexes) {
		return LDB_SUCCESS;
	}

	pdn = ldb_dn_get_parent(module, msg->dn);
	if (pdn == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}
	ret =
	    ldb_kv_modify_index_dn(module, ldb_kv, msg, pdn, LDB_KV_IDXONE, add);

	talloc_free(pdn);

	return ret;
}

/*
  insert a one level index for a message
*/
static int ldb_kv_write_index_dn_guid(struct ldb_module *module,
				      const struct ldb_message *msg,
				      int add)
{
	int ret;
	struct ldb_kv_private *ldb_kv = talloc_get_type(
	    ldb_module_get_private(module), struct ldb_kv_private);

	/* We index for DN only if using a GUID index */
	if (ldb_kv->cache->GUID_index_attribute == NULL) {
		return LDB_SUCCESS;
	}

	ret = ldb_kv_modify_index_dn(
	    module, ldb_kv, msg, msg->dn, LDB_KV_IDXDN, add);

	if (ret == LDB_ERR_CONSTRAINT_VIOLATION) {
		ldb_asprintf_errstring(ldb_module_get_ctx(module),
				       "Entry %s already exists",
				       ldb_dn_get_linearized(msg->dn));
		ret = LDB_ERR_ENTRY_ALREADY_EXISTS;
	}
	return ret;
}

/*
  add the index entries for a new element in a record
  The caller guarantees that these element values are not yet indexed
*/
int ldb_kv_index_add_element(struct ldb_module *module,
			     struct ldb_kv_private *ldb_kv,
			     const struct ldb_message *msg,
			     struct ldb_message_element *el)
{
	if (ldb_dn_is_special(msg->dn)) {
		return LDB_SUCCESS;
	}
	if (!ldb_kv_is_indexed(module, ldb_kv, el->name)) {
		return LDB_SUCCESS;
	}
	return ldb_kv_index_add_el(module, ldb_kv, msg, el);
}

/*
  add the index entries for a new record
*/
int ldb_kv_index_add_new(struct ldb_module *module,
			 struct ldb_kv_private *ldb_kv,
			 const struct ldb_message *msg)
{
	int ret;

	if (ldb_dn_is_special(msg->dn)) {
		return LDB_SUCCESS;
	}

	ret = ldb_kv_index_add_all(module, ldb_kv, msg);
	if (ret != LDB_SUCCESS) {
		/*
		 * Because we can't trust the caller to be doing
		 * transactions properly, clean up any index for this
		 * entry rather than relying on a transaction
		 * cleanup
		 */

		ldb_kv_index_delete(module, msg);
		return ret;
	}

	ret = ldb_kv_index_onelevel(module, msg, 1);
	if (ret != LDB_SUCCESS) {
		/*
		 * Because we can't trust the caller to be doing
		 * transactions properly, clean up any index for this
		 * entry rather than relying on a transaction
		 * cleanup
		 */
		ldb_kv_index_delete(module, msg);
		return ret;
	}
	return ret;
}


/*
  delete an index entry for one message element
*/
int ldb_kv_index_del_value(struct ldb_module *module,
			   struct ldb_kv_private *ldb_kv,
			   const struct ldb_message *msg,
			   struct ldb_message_element *el,
			   unsigned int v_idx)
{
	struct ldb_context *ldb;
	struct ldb_dn *dn_key;
	const char *dn_str;
	int ret, i;
	unsigned int j;
	struct dn_list *list;
	struct ldb_dn *dn = msg->dn;
	enum key_truncation truncation = KEY_NOT_TRUNCATED;

	ldb = ldb_module_get_ctx(module);

	dn_str = ldb_dn_get_linearized(dn);
	if (dn_str == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	if (dn_str[0] == '@') {
		return LDB_SUCCESS;
	}

	dn_key = ldb_kv_index_key(
	    ldb, ldb_kv, el->name, &el->values[v_idx], NULL, &truncation);
	/*
	 * We ignore key truncation in ltdb_index_add1() so
	 * match that by ignoring it here as well
	 *
	 * Multiple values are legitimate and accepted
	 */
	if (!dn_key) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	list = talloc_zero(dn_key, struct dn_list);
	if (list == NULL) {
		talloc_free(dn_key);
		return LDB_ERR_OPERATIONS_ERROR;
	}

	ret = ldb_kv_dn_list_load(module, ldb_kv, dn_key, list);
	if (ret == LDB_ERR_NO_SUCH_OBJECT) {
		/* it wasn't indexed. Did we have an earlier error? If we did then
		   its gone now */
		talloc_free(dn_key);
		return LDB_SUCCESS;
	}

	if (ret != LDB_SUCCESS) {
		talloc_free(dn_key);
		return ret;
	}

	/*
	 * Find one of the values matching this message to remove
	 */
	i = ldb_kv_dn_list_find_msg(ldb_kv, list, msg);
	if (i == -1) {
		/* nothing to delete */
		talloc_free(dn_key);
		return LDB_SUCCESS;
	}

	j = (unsigned int) i;
	if (j != list->count - 1) {
		memmove(&list->dn[j], &list->dn[j+1], sizeof(list->dn[0])*(list->count - (j+1)));
	}
	list->count--;
	if (list->count == 0) {
		talloc_free(list->dn);
		list->dn = NULL;
	} else {
		list->dn = talloc_realloc(list, list->dn, struct ldb_val, list->count);
	}

	ret = ldb_kv_dn_list_store(module, dn_key, list);

	talloc_free(dn_key);

	return ret;
}

/*
  delete the index entries for a element
  return -1 on failure
*/
int ldb_kv_index_del_element(struct ldb_module *module,
			     struct ldb_kv_private *ldb_kv,
			     const struct ldb_message *msg,
			     struct ldb_message_element *el)
{
	const char *dn_str;
	int ret;
	unsigned int i;

	if (!ldb_kv->cache->attribute_indexes) {
		/* no indexed fields */
		return LDB_SUCCESS;
	}

	dn_str = ldb_dn_get_linearized(msg->dn);
	if (dn_str == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	if (dn_str[0] == '@') {
		return LDB_SUCCESS;
	}

	if (!ldb_kv_is_indexed(module, ldb_kv, el->name)) {
		return LDB_SUCCESS;
	}
	for (i = 0; i < el->num_values; i++) {
		ret = ldb_kv_index_del_value(module, ldb_kv, msg, el, i);
		if (ret != LDB_SUCCESS) {
			return ret;
		}
	}

	return LDB_SUCCESS;
}

/*
  delete the index entries for a record
  return -1 on failure
*/
int ldb_kv_index_delete(struct ldb_module *module,
			const struct ldb_message *msg)
{
	struct ldb_kv_private *ldb_kv = talloc_get_type(
	    ldb_module_get_private(module), struct ldb_kv_private);
	int ret;
	unsigned int i;

	if (ldb_dn_is_special(msg->dn)) {
		return LDB_SUCCESS;
	}

	ret = ldb_kv_index_onelevel(module, msg, 0);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	ret = ldb_kv_write_index_dn_guid(module, msg, 0);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	if (!ldb_kv->cache->attribute_indexes) {
		/* no indexed fields */
		return LDB_SUCCESS;
	}

	for (i = 0; i < msg->num_elements; i++) {
		ret = ldb_kv_index_del_element(
		    module, ldb_kv, msg, &msg->elements[i]);
		if (ret != LDB_SUCCESS) {
			return ret;
		}
	}

	return LDB_SUCCESS;
}


/*
  traversal function that deletes all @INDEX records in the in-memory
  TDB.

  This does not touch the actual DB, that is done at transaction
  commit, which in turn greatly reduces DB churn as we will likely
  be able to do a direct update into the old record.
*/
static int delete_index(struct ldb_kv_private *ldb_kv,
			struct ldb_val key,
			struct ldb_val data,
			void *state)
{
	struct ldb_module *module = state;
	const char *dnstr = "DN=" LDB_KV_INDEX ":";
	struct dn_list list;
	struct ldb_dn *dn;
	struct ldb_val v;
	int ret;

	if (strncmp((char *)key.data, dnstr, strlen(dnstr)) != 0) {
		return 0;
	}
	/* we need to put a empty list in the internal tdb for this
	 * index entry */
	list.dn = NULL;
	list.count = 0;

	/* the offset of 3 is to remove the DN= prefix. */
	v.data = key.data + 3;
	v.length = strnlen((char *)key.data, key.length) - 3;

	dn = ldb_dn_from_ldb_val(ldb_kv, ldb_module_get_ctx(module), &v);

	/*
	 * This does not actually touch the DB quite yet, just
         * the in-memory index cache
	 */
	ret = ldb_kv_dn_list_store(module, dn, &list);
	if (ret != LDB_SUCCESS) {
		ldb_asprintf_errstring(ldb_module_get_ctx(module),
				       "Unable to store null index for %s\n",
						ldb_dn_get_linearized(dn));
		talloc_free(dn);
		return -1;
	}
	talloc_free(dn);
	return 0;
}

/*
  traversal function that adds @INDEX records during a re index TODO wrong comment
*/
static int re_key(struct ldb_kv_private *ldb_kv,
		  struct ldb_val key,
		  struct ldb_val val,
		  void *state)
{
	struct ldb_context *ldb;
	struct ldb_kv_reindex_context *ctx =
	    (struct ldb_kv_reindex_context *)state;
	struct ldb_module *module = ctx->module;
	struct ldb_message *msg;
	unsigned int nb_elements_in_db;
	int ret;
	struct ldb_val key2;
	bool is_record;

	ldb = ldb_module_get_ctx(module);

	if (key.length > 4 &&
	    memcmp(key.data, "DN=@", 4) == 0) {
		return 0;
	}

	is_record = ldb_kv_key_is_record(key);
	if (is_record == false) {
		return 0;
	}

	msg = ldb_msg_new(module);
	if (msg == NULL) {
		return -1;
	}

	ret = ldb_unpack_data_only_attr_list_flags(ldb, &val,
						   msg,
						   NULL, 0,
						   LDB_UNPACK_DATA_FLAG_NO_DATA_ALLOC,
						   &nb_elements_in_db);
	if (ret != 0) {
		ldb_debug(ldb, LDB_DEBUG_ERROR, "Invalid data for index %s\n",
						ldb_dn_get_linearized(msg->dn));
		ctx->error = ret;
		talloc_free(msg);
		return -1;
	}

	if (msg->dn == NULL) {
		ldb_debug(ldb, LDB_DEBUG_ERROR,
			  "Refusing to re-index as GUID "
			  "key %*.*s with no DN\n",
			  (int)key.length, (int)key.length,
			  (char *)key.data);
		talloc_free(msg);
		return -1;
	}

	/* check if the DN key has changed, perhaps due to the case
	   insensitivity of an element changing, or a change from DN
	   to GUID keys */
	key2 = ldb_kv_key_msg(module, msg, msg);
	if (key2.data == NULL) {
		/* probably a corrupt record ... darn */
		ldb_debug(ldb, LDB_DEBUG_ERROR, "Invalid DN in re_index: %s",
						ldb_dn_get_linearized(msg->dn));
		talloc_free(msg);
		return 0;
	}
	if (key.length != key2.length ||
	    (memcmp(key.data, key2.data, key.length) != 0)) {
		ldb_kv->kv_ops->update_in_iterate(
		    ldb_kv, key, key2, val, ctx);
	}
	talloc_free(key2.data);

	talloc_free(msg);

	ctx->count++;
	if (ctx->count % 10000 == 0) {
		ldb_debug(ldb, LDB_DEBUG_WARNING,
			  "Reindexing: re-keyed %u records so far",
			  ctx->count);
	}

	return 0;
}

/*
  traversal function that adds @INDEX records during a re index
*/
static int re_index(struct ldb_kv_private *ldb_kv,
		    struct ldb_val key,
		    struct ldb_val val,
		    void *state)
{
	struct ldb_context *ldb;
	struct ldb_kv_reindex_context *ctx =
	    (struct ldb_kv_reindex_context *)state;
	struct ldb_module *module = ctx->module;
	struct ldb_message *msg;
	unsigned int nb_elements_in_db;
	int ret;
	bool is_record;

	ldb = ldb_module_get_ctx(module);

	if (key.length > 4 &&
	    memcmp(key.data, "DN=@", 4) == 0) {
		return 0;
	}

	is_record = ldb_kv_key_is_record(key);
	if (is_record == false) {
		return 0;
	}

	msg = ldb_msg_new(module);
	if (msg == NULL) {
		return -1;
	}

	ret = ldb_unpack_data_only_attr_list_flags(ldb, &val,
						   msg,
						   NULL, 0,
						   LDB_UNPACK_DATA_FLAG_NO_DATA_ALLOC,
						   &nb_elements_in_db);
	if (ret != 0) {
		ldb_debug(ldb, LDB_DEBUG_ERROR, "Invalid data for index %s\n",
						ldb_dn_get_linearized(msg->dn));
		ctx->error = ret;
		talloc_free(msg);
		return -1;
	}

	if (msg->dn == NULL) {
		ldb_debug(ldb, LDB_DEBUG_ERROR,
			  "Refusing to re-index as GUID "
			  "key %*.*s with no DN\n",
			  (int)key.length, (int)key.length,
			  (char *)key.data);
		talloc_free(msg);
		return -1;
	}

	ret = ldb_kv_index_onelevel(module, msg, 1);
	if (ret != LDB_SUCCESS) {
		ldb_debug(ldb, LDB_DEBUG_ERROR,
			  "Adding special ONE LEVEL index failed (%s)!",
						ldb_dn_get_linearized(msg->dn));
		talloc_free(msg);
		return -1;
	}

	ret = ldb_kv_index_add_all(module, ldb_kv, msg);

	if (ret != LDB_SUCCESS) {
		ctx->error = ret;
		talloc_free(msg);
		return -1;
	}

	talloc_free(msg);

	ctx->count++;
	if (ctx->count % 10000 == 0) {
		ldb_debug(ldb, LDB_DEBUG_WARNING,
			  "Reindexing: re-indexed %u records so far",
			  ctx->count);
	}

	return 0;
}

/*
  force a complete reindex of the database
*/
int ldb_kv_reindex(struct ldb_module *module)
{
	struct ldb_kv_private *ldb_kv = talloc_get_type(
	    ldb_module_get_private(module), struct ldb_kv_private);
	int ret;
	struct ldb_kv_reindex_context ctx;

	/*
	 * Only triggered after a modification, but make clear we do
	 * not re-index a read-only DB
	 */
	if (ldb_kv->read_only) {
		return LDB_ERR_UNWILLING_TO_PERFORM;
	}

	if (ldb_kv_cache_reload(module) != 0) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	/*
	 * Ensure we read (and so remove) the entries from the real
	 * DB, no values stored so far are any use as we want to do a
	 * re-index
	 */
	ldb_kv_index_transaction_cancel(module);

	ret = ldb_kv_index_transaction_start(module);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	/* first traverse the database deleting any @INDEX records by
	 * putting NULL entries in the in-memory tdb
	 */
	ret = ldb_kv->kv_ops->iterate(ldb_kv, delete_index, module);
	if (ret < 0) {
		struct ldb_context *ldb = ldb_module_get_ctx(module);
		ldb_asprintf_errstring(ldb, "index deletion traverse failed: %s",
				       ldb_errstring(ldb));
		return LDB_ERR_OPERATIONS_ERROR;
	}

	ctx.module = module;
	ctx.error = 0;
	ctx.count = 0;

	ret = ldb_kv->kv_ops->iterate(ldb_kv, re_key, &ctx);
	if (ret < 0) {
		struct ldb_context *ldb = ldb_module_get_ctx(module);
		ldb_asprintf_errstring(ldb, "key correction traverse failed: %s",
				       ldb_errstring(ldb));
		return LDB_ERR_OPERATIONS_ERROR;
	}

	if (ctx.error != LDB_SUCCESS) {
		struct ldb_context *ldb = ldb_module_get_ctx(module);
		ldb_asprintf_errstring(ldb, "reindexing failed: %s", ldb_errstring(ldb));
		return ctx.error;
	}

	ctx.error = 0;
	ctx.count = 0;

	/* now traverse adding any indexes for normal LDB records */
	ret = ldb_kv->kv_ops->iterate(ldb_kv, re_index, &ctx);
	if (ret < 0) {
		struct ldb_context *ldb = ldb_module_get_ctx(module);
		ldb_asprintf_errstring(ldb, "reindexing traverse failed: %s",
				       ldb_errstring(ldb));
		return LDB_ERR_OPERATIONS_ERROR;
	}

	if (ctx.error != LDB_SUCCESS) {
		struct ldb_context *ldb = ldb_module_get_ctx(module);
		ldb_asprintf_errstring(ldb, "reindexing failed: %s", ldb_errstring(ldb));
		return ctx.error;
	}

	if (ctx.count > 10000) {
		ldb_debug(ldb_module_get_ctx(module),
			  LDB_DEBUG_WARNING,
			  "Reindexing: re_index successful on %s, "
			  "final index write-out will be in transaction commit",
			  ldb_kv->kv_ops->name(ldb_kv));
	}
	return LDB_SUCCESS;
}
