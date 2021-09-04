#ifndef _SVTSPARSEARRAY_CLASS_H_
#define _SVTSPARSEARRAY_CLASS_H_

#include <Rdefines.h>

SEXP C_get_SVTSparseArray_nzdata_length(
	SEXP x_dim,
	SEXP x_svtree
);

SEXP C_from_SVTSparseArray_to_COOSparseArray(
	SEXP x_dim,
	SEXP x_type,
	SEXP x_svtree
);

SEXP C_from_COOSparseArray_to_SVTSparseArray(
	SEXP x_dim,
	SEXP x_nzindex,
        SEXP x_nzdata
);

#endif  /* _SVTSPARSEARRAY_CLASS_H_ */
