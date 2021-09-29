/****************************************************************************
 *                  Subassignment to a SparseArray object                   *
 ****************************************************************************/
#include "SparseArray_subassignment.h"

#include "S4Vectors_interface.h"

#include "Rvector_utils.h"
#include "leaf_vector_utils.h"

#include <limits.h>  /* for INT_MAX */
//#include <time.h>


#define	COORD_IS_INVALID(coord, maxcoord) \
	((coord) == NA_INTEGER || (coord) < 1 || (coord) > (maxcoord))

static inline R_xlen_t get_Lidx(SEXP Lindex, long long atid_lloff)
{
	R_xlen_t Lidx;

	if (IS_INTEGER(Lindex)) {
		int i = INTEGER(Lindex)[atid_lloff];
		if (i == NA_INTEGER || i < 1)
			error("'Lindex' contains invalid linear indices");
		Lidx = (R_xlen_t) i;
	} else {
		double x = REAL(Lindex)[atid_lloff];
		if (R_IsNA(x) || R_IsNaN(x) || x < 1 ||
		    x >= 1.00 + R_XLEN_T_MAX)
			error("'Lindex' contains invalid linear indices");
		Lidx = (R_xlen_t) x;
	}
	return Lidx;
}


/****************************************************************************
 * Basic manipulation of "extended leaves"
 *
 * An "extended leaf" is used to temporarily attach a subset of the incoming
 * data (represented by 'Mindex' and 'vals', or by 'Lindex' and 'vals') to
 * a "bottom leaf" in the SVT. Note that a "bottom leaf" is a leaf located
 * at the deepest possible depth in the SVT, that is, at depth N - 1 where N
 * is the number of dimensions of the sparse array.
 * An "extended leaf" is **either**:
 *   - An Incoming Data Subset (IDS). An IDS is simply a set of offsets
 *     w.r.t. 'Mindex' (or 'Lindex') and 'vals'. These offsets get stored in
 *     an IntAE or LLongAE buffer placed behind an external pointer, and are
 *     referred to as "atid" offsets (offsets along the incoming data).
 *     Note that using an IntAE buffer would be ok for now because we're
 *     not dealing with _long_ incoming data yet. However, this will
 *     change when we start supporting _long_ incoming data e.g. when
 *     C_subassign_SVT_by_Lindex() will get passed a _long_ linear index.
 *   - An "extended leaf vector" i.e. a "leaf vector" with an IDS on it.
 *     This is represented as a list of length 3: the 2 list elements of a
 *     regular "leaf vector" (lv_offs and lv_vals) + an IDS.
 *
 * IMPORTANT NOTE: We don't allow the length of an IDS to be more than INT_MAX
 * at the moment. This is because we use sort_ints() in compute_offs_order()
 * below to sort a vector of 'IDS_len' integers and sort_ints() only handles
 * a vector of length <= INT_MAX!
 * Note however that 'IDS_len' > INT_MAX can't happen at the moment anyway
 * because 'IDS_len' is necessarily <= 'nrow(Mindex)' which is guaranteed
 * to be <= INT_MAX. However, this will change when we start supporting
 * _long_ incoming data e.g. when C_subassign_SVT_by_Lindex() is called
 * with a _long_ linear index (Lindex). Then it will be possible that more
 * than INT_MAX incoming values land on the same bottom leaf of the SVT but
 * only in some crazy and rather unlikely situations. More precisely this will
 * be possible only if the supplied Lindex is _long_ and contains duplicates.
 * Like here:
 *
 *     svt[sample(nrow(svt), 3e9, replace=TRUE)] <- 2.5
 *
 * where 3e9 incoming values are landing on the bottom leaf associated with
 * the first column of the sparse matrix! A very atypical situation.
 */

typedef SEXP (*NewIDS_FUNType)();

static SEXP new_IDS()
{
	IntAE *atid_offs_buf;

	atid_offs_buf = new_IntAE(1, 0, 0);
	return R_MakeExternalPtr(atid_offs_buf, R_NilValue, R_NilValue);
}
static SEXP new_llIDS()
{
	LLongAE *atid_lloffs_buf;

	atid_lloffs_buf = new_LLongAE(1, 0, 0);
	return R_MakeExternalPtr(atid_lloffs_buf, R_NilValue, R_NilValue);
}

static SEXP new_extended_leaf_vector(SEXP lv, NewIDS_FUNType new_IDS_FUN)
{
	SEXP lv_offs, lv_vals, IDS, ans;
	int lv_len;

	lv_len = _split_leaf_vector(lv, &lv_offs, &lv_vals);
	if (lv_len < 0)
		error("S4Arrays internal error in "
		      "new_extended_leaf_vector():\n"
		      "    unexpected error");
	IDS = PROTECT(new_IDS_FUN());
	ans = PROTECT(NEW_LIST(3));
	SET_VECTOR_ELT(ans, 0, lv_offs);
	SET_VECTOR_ELT(ans, 1, lv_vals);
	SET_VECTOR_ELT(ans, 2, IDS);
	UNPROTECT(2);
	return ans;
}

/* As a side effect the function also puts a new IDS on 'bottom_leaf' if
   it doesn't have one yet. More precisely:
   - If 'bottom_leaf' is NULL, it gets replaced with an IDS.
   - If 'bottom_leaf' is a "leaf vector", it gets replaced with an "extended
     leaf vector". */
static inline int get_IDS(SEXP bottom_leaf_parent, int i, SEXP bottom_leaf,
			  NewIDS_FUNType new_IDS_FUN, int *lv_len, SEXP *IDS)
{
	if (bottom_leaf == R_NilValue) {
		*lv_len = 0;
		*IDS = PROTECT(new_IDS_FUN());
		SET_VECTOR_ELT(bottom_leaf_parent, i, *IDS);
		UNPROTECT(1);
		return 0;
	}
	if (TYPEOF(bottom_leaf) == EXTPTRSXP) {
		/* 'bottom_leaf' is an IDS. */
		*lv_len = 0;
		*IDS = bottom_leaf;
		return 0;
	}
	if (!isVectorList(bottom_leaf))
		error("S4Arrays internal error in get_IDS():\n"
		      "    unexpected error");
	/* 'bottom_leaf' is a "leaf vector" or an "extended leaf vector". */
	if (LENGTH(bottom_leaf) == 2) {
		/* 'bottom_leaf' is a "leaf vector". */
		bottom_leaf = PROTECT(
			new_extended_leaf_vector(bottom_leaf, new_IDS_FUN)
		);
		SET_VECTOR_ELT(bottom_leaf_parent, i, bottom_leaf);
		UNPROTECT(1);
	} else if (LENGTH(bottom_leaf) != 3) {
		error("S4Arrays internal error in get_IDS():\n"
		      "    unexpected bottom leaf");
	}
	*lv_len = LENGTH(VECTOR_ELT(bottom_leaf, 0));
	*IDS = VECTOR_ELT(bottom_leaf, 2);
	return 0;
}

/* Returns IDS new length. */
static inline size_t append_atid_off_to_IDS(SEXP IDS, int atid_off)
{
	IntAE *atid_offs_buf;
	size_t IDS_len;

	atid_offs_buf = (IntAE *) R_ExternalPtrAddr(IDS);
	IDS_len = atid_offs_buf->_nelt;
	IntAE_insert_at(atid_offs_buf, IDS_len++, atid_off);
	return IDS_len;
}
static inline size_t append_atid_lloff_to_IDS(SEXP IDS, long long atid_lloff)
{
	LLongAE *atid_lloffs_buf;
	size_t IDS_len;

	atid_lloffs_buf = (LLongAE *) R_ExternalPtrAddr(IDS);
	IDS_len = atid_lloffs_buf->_nelt;
	LLongAE_insert_at(atid_lloffs_buf, IDS_len++, atid_lloff);
	return IDS_len;
}


/****************************************************************************
 * dispatch_vals_by_[M|L]index()
 *
 * This implements the 1st pass of C_subassign_SVT_by_[M|L]index().
 */

static SEXP shallow_copy_list(SEXP x)
{
	int x_len, i;
	SEXP ans;

	if (!isVectorList(x))  // IS_LIST() is broken
		error("S4Arrays internal error in shallow_copy_list():\n"
		      "    'x' is not a list");
	x_len = LENGTH(x);
	ans = PROTECT(NEW_LIST(x_len));
	for (i = 0; i < x_len; i++)
		SET_VECTOR_ELT(ans, i, VECTOR_ELT(x, i));
	UNPROTECT(1);
	return ans;
}

/* 'SVT' must be R_NilValue or a list of length 'd'.
   Always returns a list of length 'd'. Can be a newly allocated list
   or 'SVT' itself. */
static inline SEXP make_SVT_node(SEXP SVT, int d, SEXP SVT0)
{
	if (SVT == R_NilValue)
		return NEW_LIST(d);
	if (!isVectorList(SVT) || LENGTH(SVT) != d)
		error("S4Arrays internal error in make_SVT_node():\n"
		      "    unexpected error");
	/* Shallow copy **only** if 'SVT' == corresponding node in
	   original 'SVT0'. */
	if (SVT == SVT0)
		return shallow_copy_list(SVT);
	return SVT;
}

#define	MOVE_DOWN(SVT, SVT0, i, subSVT, subSVT0, subSVT_len)		\
{									\
	if ((SVT0) != R_NilValue)					\
		(subSVT0) = VECTOR_ELT(SVT0, i);			\
	SEXP new_subSVT = make_SVT_node(subSVT, subSVT_len, subSVT0);	\
	if (new_subSVT != (subSVT)) {					\
		PROTECT(new_subSVT);					\
		SET_VECTOR_ELT(SVT, i, new_subSVT);			\
		UNPROTECT(1);						\
	}								\
	(SVT) = new_subSVT;						\
	if ((SVT0) != R_NilValue)					\
		(SVT0) = (subSVT0);					\
}

/* Must be called with 'ndim' >= 2. */
static inline int descend_to_bottom_by_Mindex_row(SEXP SVT, SEXP SVT0,
		const int *dim, int ndim,
		const int *M, R_xlen_t vals_len,
		SEXP *bottom_leaf_parent, int *idx, SEXP *bottom_leaf)
{
	SEXP subSVT0, subSVT;
	const int *m_p;
	int along, d, m, i;

	subSVT0 = R_NilValue;
	m_p = M + vals_len * ndim;
	along = ndim - 1;
	do {
		d = dim[along];
		m_p -= vals_len;
		m = *m_p;
		if (COORD_IS_INVALID(m, d))
			error("'Mindex' contains invalid coordinates");
		i = m - 1;
		subSVT = VECTOR_ELT(SVT, i);
		if (along == 1)
			break;
		along--;
		MOVE_DOWN(SVT, SVT0, i, subSVT, subSVT0, dim[along]);
	} while (1);
	*bottom_leaf_parent = SVT;
	*idx = i;
	*bottom_leaf = subSVT;
	return 0;
}

/* Must be called with 'ndim' >= 2. */
static inline int descend_to_bottom_by_Lidx(SEXP SVT, SEXP SVT0,
		const int *dim, const R_xlen_t *dimcumprod, int ndim,
		R_xlen_t Lidx,
		SEXP *bottom_leaf_parent, int *idx, SEXP *bottom_leaf)
{
	SEXP subSVT0, subSVT;
	R_xlen_t idx0, p;
	int along, i;

	subSVT0 = R_NilValue;
	idx0 = Lidx - 1;
	along = ndim - 1;
	do {
		p = dimcumprod[along - 1];
		i = idx0 / p;  /* guaranteed to be >= 0 and < 'dim[along]'. */
		subSVT = VECTOR_ELT(SVT, i);
		if (along == 1)
			break;
		idx0 %= p;
		along--;
		MOVE_DOWN(SVT, SVT0, i, subSVT, subSVT0, dim[along]);
	} while (1);
	*bottom_leaf_parent = SVT;
	*idx = i;
	*bottom_leaf = subSVT;
	return 0;
}

#define	UPDATE_MAX_IDS_LEN(max_IDS_len)					\
{									\
	if (IDS_len > *(max_IDS_len))					\
		*(max_IDS_len) = IDS_len;				\
}
#define	UPDATE_MAX_POSTMERGE_LV_LEN(max_postmerge_lv_len)		\
{									\
	size_t worst_merged_len = lv_len + IDS_len;			\
	if (worst_merged_len > INT_MAX)					\
		worst_merged_len = INT_MAX;				\
	if (worst_merged_len > *(max_postmerge_lv_len))			\
		*(max_postmerge_lv_len) = (int) worst_merged_len;	\
}

static int dispatch_vals_by_Mindex(SEXP SVT, SEXP SVT0,
		const int *dim, int ndim,
		const int *Mindex, SEXP vals,
		size_t *max_IDS_len, int *max_postmerge_lv_len)
{
	R_xlen_t vals_len;
	int atid_off;  /* offset along the incoming data */
	SEXP bottom_leaf_parent, bottom_leaf, IDS;
	int i, lv_len, ret;
	size_t IDS_len;

	vals_len = XLENGTH(vals);
	for (atid_off = 0; atid_off < vals_len; atid_off++) {
		ret = descend_to_bottom_by_Mindex_row(SVT, SVT0,
				dim, ndim,
				Mindex + atid_off, vals_len,
				&bottom_leaf_parent, &i, &bottom_leaf);
		if (ret < 0)
			return -1;
		ret = get_IDS(bottom_leaf_parent, i, bottom_leaf,
			      new_IDS, &lv_len, &IDS);
		if (ret < 0)
			return -1;
		IDS_len = append_atid_off_to_IDS(IDS, atid_off);
		UPDATE_MAX_IDS_LEN(max_IDS_len);
		UPDATE_MAX_POSTMERGE_LV_LEN(max_postmerge_lv_len);
	}
	return 0;
}

static int dispatch_vals_by_Lindex(SEXP SVT, SEXP SVT0,
		const int *dim, const R_xlen_t *dimcumprod, int ndim,
		SEXP Lindex, SEXP vals,
		size_t *max_IDS_len, int *max_postmerge_lv_len)
{
	R_xlen_t vals_len, Lidx;
	long long atid_lloff;  /* offset along the incoming data */
	SEXP bottom_leaf_parent, bottom_leaf, IDS;
	int i, lv_len, ret;
	size_t IDS_len;

	vals_len = XLENGTH(vals);
	for (atid_lloff = 0; atid_lloff < vals_len; atid_lloff++) {
		Lidx = get_Lidx(Lindex, atid_lloff);
		if (Lidx > dimcumprod[ndim - 1])
			error("'Lindex' contains invalid linear indices");
		ret = descend_to_bottom_by_Lidx(SVT, SVT0,
				dim, dimcumprod, ndim, Lidx,
				&bottom_leaf_parent, &i, &bottom_leaf);
		if (ret < 0)
			return -1;
		ret = get_IDS(bottom_leaf_parent, i, bottom_leaf,
			      new_llIDS, &lv_len, &IDS);
		if (ret < 0)
			return -1;
		IDS_len = append_atid_lloff_to_IDS(IDS, atid_lloff);
		UPDATE_MAX_IDS_LEN(max_IDS_len);
		UPDATE_MAX_POSTMERGE_LV_LEN(max_postmerge_lv_len);
	}
	return 0;
}


/****************************************************************************
 * REC_absorb_vals_dispatched_by_[M|L]index()
 *
 * This implements the 2nd pass of C_subassign_SVT_by_[M|L]index().
 */

typedef struct sort_bufs_t {
	int *order;
	unsigned short int *rxbuf1;
	int *rxbuf2;
	int *offs;
} SortBufs;

/* All buffers are made of length 'max_IDS_len' except 'sort_bufs.offs'
   which we make of length 'max_postmerge_lv_len' so that we can use it
   in the call to _remove_zeros_from_leaf_vector() in the
   make_and_merge_leaf_vector_from_IDS_*_vals() functions below.
   Note that 'max_postmerge_lv_len' is guaranteed to be >= 'max_IDS_len'. */
static SortBufs alloc_sort_bufs(int max_IDS_len, int max_postmerge_lv_len)
{
	SortBufs sort_bufs;

	sort_bufs.order = (int *) R_alloc(max_IDS_len, sizeof(int));
	sort_bufs.rxbuf1 = (unsigned short int *)
			R_alloc(max_IDS_len, sizeof(unsigned short int));
	sort_bufs.rxbuf2 = (int *) R_alloc(max_IDS_len, sizeof(int));
	sort_bufs.offs = (int *) R_alloc(max_postmerge_lv_len, sizeof(int));
	return sort_bufs;
}

static void import_selected_Mindex_coord1_to_offs_buf(const int *coord1,
		const int *atid_offs, int n, int maxcoord1,
		int *offs_buf)
{
	int k, m;

	for (k = 0; k < n; k++, atid_offs++, offs_buf++) {
		m = coord1[*atid_offs];
		if (COORD_IS_INVALID(m, maxcoord1))
			error("'Mindex' contains invalid coordinates");
		*offs_buf = m - 1;
	}
	return;
}

static void import_selected_Lindex_elts_to_offs_buf(SEXP Lindex,
		const long long *atid_lloffs, int n, int d1,
		int *offs_buf)
{
	int k;
	R_xlen_t Lidx;

	for (k = 0; k < n; k++, atid_lloffs++, offs_buf++) {
		Lidx = get_Lidx(Lindex, *atid_lloffs);
		*offs_buf = (Lidx - 1) % d1;
	}
	return;
}

static void compute_offs_order(SortBufs *sort_bufs, int n)
{
	int k, ret;

	for (k = 0; k < n; k++)
		sort_bufs->order[k] = k;
	ret = sort_ints(sort_bufs->order, n, sort_bufs->offs, 0, 1,
			sort_bufs->rxbuf1, sort_bufs->rxbuf2);
	/* Note that ckecking the value returned by sort_ints() is not really
	   necessary here because sort_ints() should never fail when 'rxbuf1'
	   and 'rxbuf2' are supplied (see implementation of _sort_ints() in
	   S4Vectors/src/sort_utils.c for the details). We perform this check
	   nonetheless just to be on the safe side in case the implementation
	   of sort_ints() changes in the future. */
	if (ret < 0)
		error("S4Arrays internal error in compute_offs_order():\n"
		      "    sort_ints() returned an error");
	return;
}

/* Returns number of offsets after removal of the duplicates. */
static int remove_offs_dups(int *order_buf, int n, const int *offs)
{
	int *p1, k2;
	const int *p2;

	if (n <= 1)
		return n;
	p1 = order_buf;
	for (k2 = 1, p2 = p1 + 1; k2 < n; k2++, p2++) {
		if (offs[*p1] != offs[*p2])
			p1++;
		*p1 = *p2;
	}
	return p1 - order_buf + 1;
}

/* Returns a "leaf vector" of length 'lv_len'. */
static SEXP make_leaf_vector_from_selected_offsets(int lv_len,
		const int *order, const int *offs,
		const int *atid_offs, SEXP vals)
{
	SEXP ans_offs, ans_vals, ans;

	ans_offs = PROTECT(NEW_INTEGER(lv_len));
	_copy_selected_ints(offs, order, lv_len, INTEGER(ans_offs));
	ans_vals = PROTECT(allocVector(TYPEOF(vals), lv_len));
	_copy_Rvector_elts_from_selected_offsets(vals, atid_offs, order,
						 ans_vals);
	ans = _new_leaf_vector(ans_offs, ans_vals);
	UNPROTECT(2);
	return ans;
}
static SEXP make_leaf_vector_from_selected_lloffsets(int lv_len,
		const int *order, const int *offs,
		const long long *atid_lloffs, SEXP vals)
{
	SEXP ans_offs, ans_vals, ans;

	ans_offs = PROTECT(NEW_INTEGER(lv_len));
	_copy_selected_ints(offs, order, lv_len, INTEGER(ans_offs));
	ans_vals = PROTECT(allocVector(TYPEOF(vals), lv_len));
	_copy_Rvector_elts_from_selected_lloffsets(vals, atid_lloffs, order,
						   ans_vals);
	ans = _new_leaf_vector(ans_offs, ans_vals);
	UNPROTECT(2);
	return ans;
}

/* Does NOT drop offset/value pairs where the value is zero! This is done
   later. This means that the function always returns a "leaf vector"
   of length >= 1 and <= length(IDS) (length(IDS) should never be 0). */
static SEXP make_leaf_vector_from_IDS_Mindex_vals(SEXP IDS,
		SEXP Mindex, SEXP vals, int d, SortBufs *sort_bufs)
{
	IntAE *atid_offs_buf;
	int IDS_len, ans_len;

	atid_offs_buf = (IntAE *) R_ExternalPtrAddr(IDS);
	IDS_len = atid_offs_buf->_nelt;  /* guaranteed to be <= INT_MAX */
	import_selected_Mindex_coord1_to_offs_buf(INTEGER(Mindex),
			atid_offs_buf->elts, IDS_len, d, sort_bufs->offs);
	compute_offs_order(sort_bufs, IDS_len);
	ans_len = remove_offs_dups(sort_bufs->order, IDS_len, sort_bufs->offs);
	return make_leaf_vector_from_selected_offsets(ans_len,
				sort_bufs->order, sort_bufs->offs,
				atid_offs_buf->elts, vals);
}

/* Does NOT drop offset/value pairs where the value is zero! This is done
   later. This means that the function always returns a "leaf vector"
   of length >= 1 and <= length(IDS) (length(IDS) should never be 0). */
static SEXP make_leaf_vector_from_IDS_Lindex_vals(SEXP IDS,
		SEXP Lindex, SEXP vals, int d, SortBufs *sort_bufs)
{
	LLongAE *atid_lloffs_buf;
	int IDS_len, ans_len;

	atid_lloffs_buf = (LLongAE *) R_ExternalPtrAddr(IDS);
	IDS_len = atid_lloffs_buf->_nelt;  /* guaranteed to be <= INT_MAX */
	import_selected_Lindex_elts_to_offs_buf(Lindex,
			atid_lloffs_buf->elts, IDS_len, d, sort_bufs->offs);
	compute_offs_order(sort_bufs, IDS_len);
	ans_len = remove_offs_dups(sort_bufs->order, IDS_len, sort_bufs->offs);
	return make_leaf_vector_from_selected_lloffsets(ans_len,
				sort_bufs->order, sort_bufs->offs,
				atid_lloffs_buf->elts, vals);
}

/* Returns R_NilValue or a "leaf vector". */
static SEXP make_and_merge_leaf_vector_from_IDS_Mindex_vals(SEXP xlv,
		SEXP Mindex, SEXP vals, int d, SortBufs *sort_bufs)
{
	SEXP xlv_offs, xlv_vals, xlv_IDS, lv1, lv2, ans;

	xlv_offs = VECTOR_ELT(xlv, 0);
	xlv_vals = VECTOR_ELT(xlv, 1);
	xlv_IDS = VECTOR_ELT(xlv, 2);

	lv1 = PROTECT(_new_leaf_vector(xlv_offs, xlv_vals));
	lv2 = PROTECT(
		make_leaf_vector_from_IDS_Mindex_vals(xlv_IDS,
					Mindex, vals, d, sort_bufs)
	);

	/* The zero values must be removed **after** the merging. */
	ans = PROTECT(_merge_leaf_vectors(lv1, lv2));

	/* We've made sure that 'sort_bufs->offs' is big enough (its length
	   is 'max_postmerge_lv_len'). */
	ans = _remove_zeros_from_leaf_vector(ans, sort_bufs->offs);
	UNPROTECT(3);
	return ans;
}

static SEXP make_and_merge_leaf_vector_from_IDS_Lindex_vals(SEXP xlv,
		SEXP Lindex, SEXP vals, int d, SortBufs *sort_bufs)
{
	SEXP xlv_offs, xlv_vals, xlv_IDS, lv1, lv2, ans;

	xlv_offs = VECTOR_ELT(xlv, 0);
	xlv_vals = VECTOR_ELT(xlv, 1);
	xlv_IDS = VECTOR_ELT(xlv, 2);

	lv1 = PROTECT(_new_leaf_vector(xlv_offs, xlv_vals));
	lv2 = PROTECT(
		make_leaf_vector_from_IDS_Lindex_vals(xlv_IDS,
					Lindex, vals, d, sort_bufs)
	);

	/* The zero values must be removed **after** the merging. */
	ans = PROTECT(_merge_leaf_vectors(lv1, lv2));

	/* We've made sure that 'sort_bufs->offs' is big enough (its length
	   is 'max_postmerge_lv_len'). */
	ans = _remove_zeros_from_leaf_vector(ans, sort_bufs->offs);
	UNPROTECT(3);
	return ans;
}

/* Recursive. */
static SEXP REC_absorb_vals_dispatched_by_Mindex(SEXP SVT,
		const int *dim, int ndim, SEXP Mindex, SEXP vals,
		SortBufs *sort_bufs)
{
	int SVT_len, is_empty, i;
	SEXP ans, subSVT;

	if (SVT == R_NilValue)
		return R_NilValue;

	if (ndim == 1) {
		/* 'SVT' is a bottom leaf (IDS, "leaf vector", or
		   "extended leaf vector"). */
		if (TYPEOF(SVT) == EXTPTRSXP) {
			/* 'SVT' is an IDS. */
			ans = PROTECT(
				make_leaf_vector_from_IDS_Mindex_vals(SVT,
					Mindex, vals, dim[0], sort_bufs)
			);
			ans = _remove_zeros_from_leaf_vector(ans,
							     sort_bufs->offs);
			UNPROTECT(1);
			return ans;
		}
		SVT_len = LENGTH(SVT);
		if (SVT_len == 2) {
			/* 'SVT' is a "leaf vector". */
			return SVT;
		}
		if (SVT_len == 3) {
			/* 'SVT' is an "extended leaf vector". */
			return make_and_merge_leaf_vector_from_IDS_Mindex_vals(
					SVT, Mindex, vals, dim[0], sort_bufs);
		}
		error("S4Arrays internal error in "
		      "REC_absorb_vals_dispatched_by_Mindex():\n"
		      "    unexpected error");
	}

	/* 'SVT' is a regular node (list). */
	SVT_len = LENGTH(SVT);  /* should be equal to 'd = dim[ndim - 1]' */
	is_empty = 1;
	for (i = 0; i < SVT_len; i++) {
		subSVT = VECTOR_ELT(SVT, i);
		subSVT = REC_absorb_vals_dispatched_by_Mindex(subSVT,
					dim, ndim - 1, Mindex, vals,
					sort_bufs);
		if (subSVT != R_NilValue) {
			PROTECT(subSVT);
			SET_VECTOR_ELT(SVT, i, subSVT);
			UNPROTECT(1);
			is_empty = 0;
		} else {
			SET_VECTOR_ELT(SVT, i, subSVT);
		}
	}
	return is_empty ? R_NilValue : SVT;
}

/* Recursive. */
static SEXP REC_absorb_vals_dispatched_by_Lindex(SEXP SVT,
		const R_xlen_t *dimcumprod, int ndim, SEXP Lindex, SEXP vals,
		SortBufs *sort_bufs)
{
	int SVT_len, is_empty, i;
	SEXP ans, subSVT;

	if (SVT == R_NilValue)
		return R_NilValue;

	if (ndim == 1) {
		/* 'SVT' is a bottom leaf (IDS, "leaf vector", or
		   "extended leaf vector"). */
		if (TYPEOF(SVT) == EXTPTRSXP) {
			/* 'SVT' is an IDS. */
			ans = PROTECT(
				make_leaf_vector_from_IDS_Lindex_vals(SVT,
					Lindex, vals, (int) dimcumprod[0],
					sort_bufs)
			);
			ans = _remove_zeros_from_leaf_vector(ans,
							     sort_bufs->offs);
			UNPROTECT(1);
			return ans;
		}
		SVT_len = LENGTH(SVT);
		if (SVT_len == 2) {
			/* 'SVT' is a "leaf vector". */
			return SVT;
		}
		if (SVT_len == 3) {
			/* 'SVT' is an "extended leaf vector". */
			return make_and_merge_leaf_vector_from_IDS_Lindex_vals(
					SVT, Lindex, vals, (int) dimcumprod[0],
					sort_bufs);
		}
		error("S4Arrays internal error in "
		      "REC_absorb_vals_dispatched_by_Lindex():\n"
		      "    unexpected error");
	}

	/* 'SVT' is a regular node (list). */
	SVT_len = LENGTH(SVT);  /* should be equal to 'd = dim[ndim - 1]' */
	is_empty = 1;
	for (i = 0; i < SVT_len; i++) {
		subSVT = VECTOR_ELT(SVT, i);
		subSVT = REC_absorb_vals_dispatched_by_Lindex(subSVT,
					dimcumprod, ndim - 1, Lindex, vals,
					sort_bufs);
		if (subSVT != R_NilValue) {
			PROTECT(subSVT);
			SET_VECTOR_ELT(SVT, i, subSVT);
			UNPROTECT(1);
			is_empty = 0;
		} else {
			SET_VECTOR_ELT(SVT, i, subSVT);
		}
	}
	return is_empty ? R_NilValue : SVT;
}


/****************************************************************************
 * subassign_1D_SVT_by_Lindex()
 *
 * The 1D case needs special treatment.
 */

/* 'Lindex' and 'vals' are assumed to have the same length.
   This length is assumed to be >= 1 and <= INT_MAX.
   Returns a "leaf vector" of length >= 1 and <= length(vals). */
static SEXP make_leaf_vector_from_Lindex_vals(SEXP Lindex, SEXP vals, int d,
		SortBufs *sort_bufs)
{
	int vals_len, ans_len;
	R_xlen_t Lidx;
	int atid_off;  /* offset along the incoming data */
	SEXP ans_offs, ans_vals, ans;

	vals_len = LENGTH(vals);  /* we know 'length(vals)' is <= INT_MAX */
	for (atid_off = 0; atid_off < vals_len; atid_off++) {
		Lidx = get_Lidx(Lindex, atid_off);
		if (Lidx > d)
			error("subassignment subscript contains "
			      "invalid indices");
		sort_bufs->offs[atid_off] = Lidx - 1;
	}
	compute_offs_order(sort_bufs, vals_len);
	ans_len = remove_offs_dups(sort_bufs->order, vals_len, sort_bufs->offs);
	ans_offs = PROTECT(NEW_INTEGER(ans_len));
	_copy_selected_ints(sort_bufs->offs, sort_bufs->order, ans_len,
			    INTEGER(ans_offs));
	ans_vals = PROTECT(allocVector(TYPEOF(vals), ans_len));
	_copy_selected_Rsubvec_elts(vals, 0, sort_bufs->order, ans_vals);
	ans = _new_leaf_vector(ans_offs, ans_vals);
	UNPROTECT(2);
	return ans;
}

/* 'SVT' is either R_NilValue or a "leaf vector".
   'Lindex' and 'vals' are assumed to have the same nonzero length. */
static SEXP subassign_1D_SVT_by_Lindex(int d, SEXP SVT, SEXP Lindex, SEXP vals)
{
	R_xlen_t vals_len;
	size_t worst_merged_len;
	SortBufs sort_bufs;
	SEXP ans;

	vals_len = XLENGTH(vals);
	if (vals_len > INT_MAX)
		error("assigning more than INT_MAX values to "
		      "a monodimensional SVT_SparseArray object "
		      "is not supported");
	if (SVT == R_NilValue) {
		worst_merged_len = vals_len;
	} else {
		int lv_len = LENGTH(VECTOR_ELT(SVT, 0));
		worst_merged_len = lv_len + vals_len;
		if (worst_merged_len > INT_MAX)
			worst_merged_len = INT_MAX;
	}
	sort_bufs = alloc_sort_bufs((int) vals_len, (int) worst_merged_len);
	ans = PROTECT(
		make_leaf_vector_from_Lindex_vals(Lindex, vals, d, &sort_bufs)
	);
	if (SVT != R_NilValue)
		ans = PROTECT(_merge_leaf_vectors(SVT, ans));
	/* We've made sure that 'sort_bufs.offs' is big enough (its length
	   is 'worst_merged_len'). */
	ans = _remove_zeros_from_leaf_vector(ans, sort_bufs.offs);
	UNPROTECT(SVT != R_NilValue ? 2 : 1);
	return ans;
}


/****************************************************************************
 * C_subassign_SVT_by_[M|L]index()
 */

static SEXP check_Mindex_dim(SEXP Mindex, R_xlen_t vals_len, int ndim,
		const char *what1, const char *what2, const char *what3)
{
	SEXP Mindex_dim;

	Mindex_dim = GET_DIM(Mindex);
	if (Mindex_dim == R_NilValue || LENGTH(Mindex_dim) != 2)
		error("'%s' must be a matrix", what1);
	if (!IS_INTEGER(Mindex))
		error("'%s' must be an integer matrix", what1);
	if (INTEGER(Mindex_dim)[0] != vals_len)
		error("nrow(%s) != %s", what1, what2);
	if (INTEGER(Mindex_dim)[1] != ndim)
		error("ncol(%s) != %s", what1, what3);
	return Mindex_dim;
}

/* --- .Call ENTRY POINT --- */
SEXP C_subassign_SVT_by_Mindex(SEXP x_dim, SEXP x_type, SEXP x_SVT,
		SEXP Mindex, SEXP vals)
{
	SEXPTYPE Rtype;
	int x_ndim, d, max_postmerge_lv_len, ret;
	R_xlen_t vals_len;
	SEXP ans;
	size_t max_IDS_len;
	SortBufs sort_bufs;

	Rtype = _get_Rtype_from_Rstring(x_type);
	if (Rtype == 0)
		error("S4Arrays internal error in "
		      "C_subassign_SVT_by_Mindex():\n"
		      "    SVT_SparseArray object has invalid type");
	if (TYPEOF(vals) != Rtype)
		error("S4Arrays internal error in "
		      "C_subassign_SVT_by_Mindex():\n"
		      "    SVT_SparseArray object and 'vals' "
		      "must have the same type");

	x_ndim = LENGTH(x_dim);
	vals_len = XLENGTH(vals);
	check_Mindex_dim(Mindex, vals_len, x_ndim,
			 "Mindex", "length(vals)", "length(dim(x))");
	if (vals_len == 0)
		return x_SVT;  /* no-op */

	if (x_ndim == 1)
		return subassign_1D_SVT_by_Lindex(INTEGER(x_dim)[0],
						  x_SVT, Mindex, vals);

	// FIXME: Bad things will happen if some of the dimensions are 0!

	/* 1st pass */
	//clock_t t0 = clock();
	d = INTEGER(x_dim)[x_ndim - 1];
	ans = PROTECT(make_SVT_node(x_SVT, d, R_NilValue));
	max_IDS_len = 0;
	max_postmerge_lv_len = 0;
	ret = dispatch_vals_by_Mindex(ans, x_SVT,
			INTEGER(x_dim), LENGTH(x_dim),
			INTEGER(Mindex), vals,
			&max_IDS_len, &max_postmerge_lv_len);
	if (ret < 0) {
		UNPROTECT(1);
		error("S4Arrays internal error in "
		      "C_subassign_SVT_by_Mindex():\n"
		      "    dispatch_vals_by_Mindex() returned an error");
	}

	//printf("max_IDS_len = %lu -- max_postmerge_lv_len = %d\n",
	//       max_IDS_len, max_postmerge_lv_len);
	if (max_IDS_len > INT_MAX) {
		UNPROTECT(1);
		error("assigning more than INT_MAX values to "
		      "the same column is not supported");
	}
	/* Sanity check (should never fail). */
	if (max_postmerge_lv_len < max_IDS_len) {
		UNPROTECT(1);
		error("S4Arrays internal error in "
		      "C_subassign_SVT_by_Mindex():\n"
		      "    max_postmerge_lv_len < max_IDS_len");
	}
	//double dt = (1.0 * clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	//printf("1st pass: %2.3f ms\n", dt);

	/* 2nd pass */
	//t0 = clock();
	sort_bufs = alloc_sort_bufs((int) max_IDS_len, max_postmerge_lv_len);
	ans = REC_absorb_vals_dispatched_by_Mindex(ans,
			INTEGER(x_dim), LENGTH(x_dim), Mindex, vals,
			&sort_bufs);
	//dt = (1.0 * clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	//printf("2nd pass: %2.3f ms\n", dt);
	UNPROTECT(1);
	return ans;
}

/* --- .Call ENTRY POINT --- */
SEXP C_subassign_SVT_by_Lindex(SEXP x_dim, SEXP x_type, SEXP x_SVT,
		SEXP Lindex, SEXP vals)
{
	SEXPTYPE Rtype;
	int x_ndim, along, d, max_postmerge_lv_len, ret;
	R_xlen_t vals_len, *dimcumprod, p;
	SEXP ans;
	size_t max_IDS_len;
	SortBufs sort_bufs;

	Rtype = _get_Rtype_from_Rstring(x_type);
	if (Rtype == 0)
		error("S4Arrays internal error in "
		      "C_subassign_SVT_by_Lindex():\n"
		      "    SVT_SparseArray object has invalid type");
	if (TYPEOF(vals) != Rtype)
		error("S4Arrays internal error in "
		      "C_subassign_SVT_by_Lindex():\n"
		      "    SVT_SparseArray object and 'vals' "
		      "must have the same type");

	x_ndim = LENGTH(x_dim);
	vals_len = XLENGTH(vals);
	if (!IS_INTEGER(Lindex) && !IS_NUMERIC(Lindex))
		error("'Lindex' must be an integer or numeric vector");
	if (XLENGTH(Lindex) != vals_len)
		error("length(Lindex) != length(vals)");
	if (vals_len == 0)
		return x_SVT;  /* no-op */

	if (x_ndim == 1)
		return subassign_1D_SVT_by_Lindex(INTEGER(x_dim)[0],
						  x_SVT, Lindex, vals);

	dimcumprod = (R_xlen_t *) R_alloc(x_ndim, sizeof(R_xlen_t));
	p = 1;
	for (along = 0; along < x_ndim; along++) {
		p *= INTEGER(x_dim)[along];
		dimcumprod[along] = p;
	}

	// FIXME: Bad things will happen if some of the dimensions are 0 i.e.
	// if p == 0!

	/* 1st pass */
	//clock_t t0 = clock();
	d = INTEGER(x_dim)[x_ndim - 1];
	ans = PROTECT(make_SVT_node(x_SVT, d, R_NilValue));
	max_IDS_len = 0;
	max_postmerge_lv_len = 0;
	ret = dispatch_vals_by_Lindex(ans, x_SVT,
			INTEGER(x_dim), dimcumprod, LENGTH(x_dim),
			Lindex, vals,
			&max_IDS_len, &max_postmerge_lv_len);
	if (ret < 0) {
		UNPROTECT(1);
		error("S4Arrays internal error in "
		      "C_subassign_SVT_by_Lindex():\n"
		      "    dispatch_vals_by_Lindex() returned an error");
	}

	//printf("max_IDS_len = %lu -- max_postmerge_lv_len = %d\n",
	//       max_IDS_len, max_postmerge_lv_len);
	if (max_IDS_len > INT_MAX) {
		UNPROTECT(1);
		error("assigning more than INT_MAX values to "
		      "the same column is not supported");
	}
	/* Sanity check (should never fail). */
	if (max_postmerge_lv_len < max_IDS_len) {
		UNPROTECT(1);
		error("S4Arrays internal error in "
		      "C_subassign_SVT_by_Lindex():\n"
		      "    max_postmerge_lv_len < max_IDS_len");
	}
	//double dt = (1.0 * clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	//printf("1st pass: %2.3f ms\n", dt);

	/* 2nd pass */
	//t0 = clock();
	sort_bufs = alloc_sort_bufs((int) max_IDS_len, max_postmerge_lv_len);
	ans = REC_absorb_vals_dispatched_by_Lindex(ans,
			dimcumprod, LENGTH(x_dim), Lindex, vals,
			&sort_bufs);
	//dt = (1.0 * clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	//printf("2nd pass: %2.3f ms\n", dt);
	UNPROTECT(1);
	return ans;
}


/****************************************************************************
 * C_subassign_SVT_with_short_Rvector()
 */

/* --- .Call ENTRY POINT --- */
SEXP C_subassign_SVT_with_short_Rvector(
		SEXP x_dim, SEXP x_type, SEXP x_SVT, SEXP index,
		SEXP Rvector)
{
	error("not ready yet");
	return R_NilValue;
}


/****************************************************************************
 * C_subassign_SVT_with_Rarray() and C_subassign_SVT_with_SVT()
 */

/* --- .Call ENTRY POINT --- */
SEXP C_subassign_SVT_with_Rarray(
		SEXP x_dim, SEXP x_type, SEXP x_SVT, SEXP index,
		SEXP Rarray)
{
	error("not ready yet");
	return R_NilValue;
}

/* --- .Call ENTRY POINT --- */
SEXP C_subassign_SVT_with_SVT(
		SEXP x_dim, SEXP x_type, SEXP x_SVT, SEXP index,
		SEXP v_dim, SEXP v_type, SEXP v_SVT)
{
	error("not ready yet");
	return R_NilValue;
}

