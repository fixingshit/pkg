// strlcat(3) is meant to be checked for returned value.
//
// Confidence: High
// Copyright: (C) The pkgng project, see COPYING.
// URL: https://github.com/freebsd/pkg/tree/master/tests/cocci/pkg/strlcat.cocci

@@
expression E, E1, S;
@@

- strlcat(E, E1, S);
+ assert(strlcat(E, E1, S) < S)/* FIXME: strlcat.cocci */;
