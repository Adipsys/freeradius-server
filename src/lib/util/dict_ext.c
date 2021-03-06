/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/** Extensions to dictionary structures
 *
 * @file src/lib/util/dict_attr_ext.c
 *
 * @copyright 2020 The FreeRADIUS server project
 * @copyright 2020 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 */
RCSID("$Id$")

#include <freeradius-devel/util/dict_priv.h>
#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/table.h>
#include <freeradius-devel/util/talloc.h>

/** Copy function for fixing up extensions after they're copy
 *
 */
typedef void *(* fr_dict_attr_ext_copy_t)(TALLOC_CTX *ctx, fr_dict_attr_t **da_out_p,
					  fr_dict_attr_ext_t ext, void *ext_ptr, size_t ext_len);

/** Additional information for a given extension
 */
typedef struct {
	size_t				min;		//!< Minimum size of extension.
	bool				has_hdr;	//!< Has additional metadata allocated before
							///< the extension data.
	bool				can_copy;	//!< Copying this extension between attributes is allowed.
	fr_dict_attr_ext_copy_t		copy;		//!< Override the normal copy operation with a callback.
} fr_dict_attr_ext_info_t;

static void *fr_dict_attr_ext_enumv_copy(TALLOC_CTX *ctx, fr_dict_attr_t **da_p,
					 fr_dict_attr_ext_t ext, void *ext_ptr, size_t ext_len);


static void *fr_dict_attr_ext_vendor_copy(TALLOC_CTX *ctx, fr_dict_attr_t **da_p,
					  fr_dict_attr_ext_t ext, void *ext_ptr, UNUSED size_t ext_len);

static fr_table_num_ordered_t const dict_attr_ext_table[] = {
	{ L("name"),		FR_DICT_ATTR_EXT_NAME		},
	{ L("children"),	FR_DICT_ATTR_EXT_CHILDREN	},
	{ L("vendor"),		FR_DICT_ATTR_EXT_REF		},
	{ L("enumv"),		FR_DICT_ATTR_EXT_VENDOR		},
	{ L("da_stack"),	FR_DICT_ATTR_EXT_ENUMV		},
	{ L("name"),		FR_DICT_ATTR_EXT_DA_STACK	}
};
static size_t dict_attr_ext_table_len = NUM_ELEMENTS(dict_attr_ext_table);

/** Holds additional information about extension structures
 *
 */
static fr_dict_attr_ext_info_t const fr_dict_ext_info[] = {
	[FR_DICT_ATTR_EXT_NAME]		= {
						.min = sizeof(char),
						.has_hdr = true,
						.can_copy = false,	/* Name may change, and we can only set it once */
					},
	[FR_DICT_ATTR_EXT_CHILDREN]	= {
						.min = sizeof(fr_dict_attr_ext_children_t),
						.can_copy = false,	/* Limitation in hashing scheme we use */
					},
	[FR_DICT_ATTR_EXT_REF]		= {
						.min = sizeof(fr_dict_attr_ext_ref_t),
						.can_copy = true,
					},
	[FR_DICT_ATTR_EXT_VENDOR]	= {
						.min = sizeof(fr_dict_attr_ext_vendor_t),
						.can_copy = true,
						.copy = fr_dict_attr_ext_vendor_copy
					},
	[FR_DICT_ATTR_EXT_ENUMV]	= {
						.min = sizeof(fr_dict_attr_ext_enumv_t),
						.can_copy = true,
						.copy = fr_dict_attr_ext_enumv_copy
					},
	[FR_DICT_ATTR_EXT_DA_STACK]	= {
						.min = sizeof(fr_dict_attr_ext_da_stack_t),
						.has_hdr = true,
						.can_copy = false	/* Reinitialised for each new attribute */
					},
	[FR_DICT_ATTR_EXT_MAX]		= {}
};

/** Optional extension header struct
 *
 */
typedef struct {
	size_t			len;		//!< Length of extension data.
	uint8_t			data[];		//!< Extension data
} CC_HINT(aligned(FR_DICT_ATTR_EXT_ALIGNMENT)) dict_attr_ext_hdr_t;

/** Copy all enumeration values from one attribute to another
 *
 */
static void *fr_dict_attr_ext_enumv_copy(TALLOC_CTX *ctx, fr_dict_attr_t **da_p,
					 fr_dict_attr_ext_t ext, void *ext_ptr, UNUSED size_t ext_len)
{
	fr_dict_attr_ext_enumv_t	*new_ext, *old_ext;
	fr_hash_iter_t			iter;
	fr_dict_enum_t			*enumv;
	bool				has_child = da_is_key_field(*da_p);

	new_ext = dict_attr_ext_alloc(ctx, da_p, ext);
	if (!new_ext) return NULL;

	old_ext = ext_ptr;

	if (!old_ext->value_by_name && !old_ext->name_by_value) {
		memset(new_ext, 0, sizeof(*new_ext));
		return new_ext;
	}

	/*
	 *	Add all the enumeration values from
	 *      the old attribute to the new attribute.
	 */
	for (enumv = fr_hash_table_iter_init(old_ext->value_by_name, &iter);
	     enumv;
	     enumv = fr_hash_table_iter_next(old_ext->value_by_name, &iter)) {
		fr_dict_attr_t const *child_struct;

		if (!has_child) {
			child_struct = NULL;
		} else {
			child_struct = enumv->child_struct[0];
		}

	     	/*
	     	 *	Fixme - Child struct copying is probably wrong
	     	 */
		if (dict_attr_enum_add_name(*da_p, enumv->name, enumv->value, true, true, child_struct) < 0) {
			return NULL;
		}
	}

	return new_ext;
}

/** Rediscover the parent of this attribute, and cache it
 *
 */
static void *fr_dict_attr_ext_vendor_copy(TALLOC_CTX *ctx, fr_dict_attr_t **da_p,
					  fr_dict_attr_ext_t ext, void *ext_ptr, UNUSED size_t ext_len)
{
	fr_dict_attr_ext_vendor_t	*new_ext, *old_ext = ext_ptr;
	fr_dict_attr_t const		**da_stack;
	fr_dict_attr_t const		*old_vendor = old_ext->vendor;
	fr_dict_attr_t const		*new_vendor, *da;

	/*
	 *	If we have a da stack, see if we can
	 *	find a vendor at the same depth as
	 *	the old depth.
	 */
	da_stack = fr_dict_attr_da_stack(*da_p);
	if (da_stack) {
		new_vendor = da_stack[old_vendor->depth];
		if ((new_vendor->type == old_vendor->type) && (new_vendor->attr == old_vendor->attr)) {
			new_ext = dict_attr_ext_alloc(ctx, da_p, ext);
			if (!new_ext) return NULL;

			new_ext->vendor = new_vendor;
			return new_ext;
		}
	}

	/*
	 *	Otherwise traverse the parent list
	 *	looking for the vendor.
	 *
	 *	Theoretically the attribute could
	 *	have been moved to a different depth.
	 */
	for (da = (*da_p)->parent; da; da = da->parent) {
		if ((da->type == old_vendor->type) && (da->attr == old_vendor->attr)) {
			new_ext = dict_attr_ext_alloc(ctx, da_p, ext);
			if (!new_ext) return NULL;

			new_ext->vendor = da;
			return new_ext;
		}
	}

	return NULL;
}

/** Add a variable length extension to a dictionary attribute
 *
 * Extensions are appended to the existing #fr_dict_attr_t memory chunk
 * using realloc.
 *
 * When a new extension is allocated it will not be initialised.
 *
 * @param[in] ctx	the dict attr was originally allocated in.
 * @param[in,out] da_p	The dictionary attribute to add an extension for.
 *			Under certain circumstances the value of *da_p will
 *			be changed to point to a new memory block.
 *			All cached copied of the previous pointer should be
 *			updated.  This means that attributes that have
 *			already been added to a dictionary should not have
 *			extensions allocated unless care is taken to update
 *			all references.
 * @param[in] ext	to alloc.
 * @param[in] ext_len	The length of the extension.
 * @return
 *	- NULL if we failed allocating an extension.
 *	- A pointer to the extension we allocated.
 */
void *dict_attr_ext_alloc_size(TALLOC_CTX *ctx, fr_dict_attr_t **da_p, fr_dict_attr_ext_t ext, size_t ext_len)
{
	size_t			aligned_len = ROUND_UP_POW2(ext_len, FR_DICT_ATTR_EXT_ALIGNMENT);
	size_t			da_len;
	size_t			hdr_len = 0;

	size_t			offset;

	fr_dict_attr_ext_info_t const *info;
	fr_dict_attr_t		*n_da, *da = *da_p;
	uint8_t			*ext_ptr;

	(void)talloc_get_type_abort(da, fr_dict_attr_t);

	if (da->ext[ext]) return fr_dict_attr_ext(da, ext);

	if (unlikely(da->dict && da->dict->read_only)) {
		fr_strerror_printf("%s dictionary has been marked as read only", fr_dict_root(da->dict)->name);
		return NULL;
	}

 	info = &fr_dict_ext_info[ext];
	if (info->has_hdr) hdr_len = sizeof(dict_attr_ext_hdr_t);	/* Add space for a length prefix */

	/*
	 *	Packing the offsets into a uint8_t means
	 *      the offset address of the final extension
	 *	must be less than or equal to
	 *	UINT8_MAX * FR_DICT_ATTR_EXT_ALIGNMENT
	 */
	da_len = talloc_length(da);
	offset = (da_len + hdr_len) / FR_DICT_ATTR_EXT_ALIGNMENT;
	if (unlikely(offset > UINT8_MAX)) {
		fr_strerror_printf("Insufficient space remaining for extensions");
		return NULL;
	}

	n_da = talloc_realloc_size(ctx, da, da_len + hdr_len + aligned_len);
	if (!n_da) {
		fr_strerror_printf("Failed in realloc for dictionary extension '%s'. "
				   "Tried to realloc %zu bytes -> %zu bytes",
				   fr_table_str_by_value(dict_attr_ext_table, ext, "<INVALID>"),
				   da_len, da_len + aligned_len);
		return NULL;
	}
	talloc_set_type(n_da, fr_dict_attr_t);
	*da_p = n_da;

	n_da->ext[ext] = (uint8_t)offset;
	ext_ptr = ((uint8_t *)n_da) + da_len;

	if (info->has_hdr) {
		dict_attr_ext_hdr_t *ext_hdr = (dict_attr_ext_hdr_t *)ext_ptr;

		ext_hdr->len = ext_len;		/* Record the real size */
		return &ext_hdr->data;		/* Pointer to the data portion */
	}

	return ext_ptr;
}

/** Add a fixed length extension to a dictionary attribute
 *
 * Extensions are appended to the existing #fr_dict_attr_t memory chunk
 * using realloc.
 *
 * When a new extension is allocated it will not be initialised.
 * In the majority of instances this is OK as its value will be set
 * immediately, but care should be taken to ensure it is initialised
 * as some point.
 *
 * @param[in] ctx	the dict attr was originally allocated in.
 * @param[in,out] da_p	The dictionary attribute to add an extension for.
 *			Under certain circumstances the value of *da_p will
 *			be changed to point to a new memory block.
 *			All cached copied of the previous pointer should be
 *			updated.  This means that attributes that have
 *			already been added to a dictionary should not have
 *			extensions allocated unless care is taken to update
 *			all references.
 * @param[in] ext	to alloc.
 * @return
 *	- NULL if we failed allocating an extension.
 *	- A pointer to the extension we allocated.
 */
void *dict_attr_ext_alloc(TALLOC_CTX *ctx, fr_dict_attr_t **da_p, fr_dict_attr_ext_t ext)
{
	return dict_attr_ext_alloc_size(ctx, da_p, ext, fr_dict_ext_info[ext].min);
}

/** Return the length of an extension
 *
 * @param[in] da	to return extension length for.
 * @param[in] ext	to return length for.
 * @return
 *	- 0 if no extension exists.
 *	- >0 the length of the extension.
 */
size_t dict_attr_ext_len(fr_dict_attr_t const *da, fr_dict_attr_ext_t ext)
{
	uint8_t				offset;
	fr_dict_attr_ext_info_t	const	*info;
	dict_attr_ext_hdr_t		*ext_hdr;

	offset = da->ext[ext];
	if (!offset) return 0;

	info = &fr_dict_ext_info[ext];
	if (!info->has_hdr) return info->min;	/* Fixed size */

	ext_hdr = (dict_attr_ext_hdr_t *)((uintptr_t)da) + ((offset * FR_DICT_ATTR_EXT_ALIGNMENT) - sizeof(dict_attr_ext_hdr_t));
	return ext_hdr->len;
}

/** Copy extension data from one attribute to another
 *
 * @param[in] ctx	to realloc da_out in.
 * @param[in] da_in	to copy extension from.
 * @param[in] ext	to copy.
 * @return
 *	- NULL if we failed to allocate an extension structure.
 *	- A pointer to the offset of the extension in da_out.
 */
void *dict_attr_ext_copy(TALLOC_CTX *ctx,
			 fr_dict_attr_t **da_out_p, fr_dict_attr_t const *da_in, fr_dict_attr_ext_t ext)
{
	void	*ext_ptr, *new_ext_ptr;
	size_t	ext_len;

	fr_dict_attr_ext_info_t const *info;

	if (unlikely((*da_out_p)->dict && (*da_out_p)->dict->read_only)) {
		fr_strerror_printf("%s dictionary has been marked as read only", fr_dict_root((*da_out_p)->dict)->name);
		return NULL;
	}

	info = &fr_dict_ext_info[ext];
	if (!info->can_copy) {
		fr_strerror_printf("Extension cannot be copied");
		return NULL;
	}

	ext_len = dict_attr_ext_len(da_in, ext);
	ext_ptr = fr_dict_attr_ext(da_in, ext);
	if (!ext_ptr) return NULL;

	/*
	 *	Use the special copy function.
	 *	Its responsible for allocating the extension in the
	 *      destination attribute.
	 */
	if (info->copy) return info->copy(ctx, da_out_p, ext, ext_ptr, ext_len);

	/*
	 *	If there's no special function
	 *	just memcpy the data over.
	 */
	new_ext_ptr = dict_attr_ext_alloc_size(ctx, da_out_p, ext, ext_len);
	if (!new_ext_ptr) return NULL;
	memcpy(new_ext_ptr, ext_ptr, ext_len);

	return new_ext_ptr;
}

/** Copy all the extensions from one attribute to another
 *
 */
int dict_attr_ext_copy_all(TALLOC_CTX *ctx,
			   fr_dict_attr_t **da_out_p, fr_dict_attr_t const *da_in)
{
	fr_dict_attr_ext_t i;

	for (i = 0; i < NUM_ELEMENTS(da_in->ext); i++) {
		if (!da_in->ext[i] || !fr_dict_ext_info[i].can_copy) continue;
		if (!dict_attr_ext_copy(ctx, da_out_p, da_in, i)) return -1;
	}

	return 0;
}

void dict_attr_ext_debug(fr_dict_attr_t const *da)
{
	fr_dict_attr_ext_t i;

	FR_FAULT_LOG("ext: %s - total len = %zu, ext len = %zu",
		     da->name, talloc_length(da), talloc_length(da) - sizeof(fr_dict_attr_t));

	for (i = 0; i < NUM_ELEMENTS(da->ext); i++) {
		if (fr_dict_attr_has_ext(da, i)) {
			void	*ext = fr_dict_attr_ext(da, i);
			size_t	ext_len = dict_attr_ext_len(da, i);

			if (ext_len > 1024) {
				FR_FAULT_LOG("ext: %s - bad length %zu - limiting to 1024",
					     fr_table_str_by_value(dict_attr_ext_table, i, "<INVALID>"),
					     ext_len);
				ext_len = 1024;
			}

			FR_FAULT_LOG("ext: %s - start=%p end=%p len=%zu",
				     fr_table_str_by_value(dict_attr_ext_table, i, "<INVALID>"),
				     ext, ((uint8_t *)ext) + ext_len, ext_len);
			FR_FAULT_LOG_HEX(ext, ext_len);
		}
	}
}
