#ifndef __BSTRING_H__
#define __BSTRING_H__
#ifdef __cplusplus
extern "C" {
#endif

/*
 * bstring(3C) -- byte string operations
 *
 * Copyright 1990, Silicon Graphics, Inc. 
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Silicon Graphics, Inc.;
 * the contents of this file may not be disclosed to third parties, copied or 
 * duplicated in any form, in whole or in part, without the prior written 
 * permission of Silicon Graphics, Inc.
 *
 * RESTRICTED RIGHTS LEGEND:
 * Use, duplication or disclosure by the Government is subject to restrictions 
 * as set forth in subdivision (c)(1)(ii) of the Rights in Technical Data
 * and Computer Software clause at DFARS 252.227-7013, and/or in similar or 
 * successor clauses in the FAR, DOD or NASA FAR Supplement. Unpublished - 
 * rights reserved under the Copyright Laws of the United States.
 */

#ident "$Revision: 1.4 $"

#if defined(PORT) && !defined(_WIN32)
/* 64-bit port: PR/os.h takes the host <strings.h> declarations (size_t
 * sizes) under PORT; agree with it rather than redeclare the N64 int forms
 * (clang: conflicting types). docs/port-ios/04-round1-prereqs.md. */
#include <strings.h>
#else
extern void    bcopy(const void *, void *, int);
extern int    bcmp(const void *, const void *, int);
extern void    bzero(void *, int);
#endif

#ifdef __cplusplus
}
#endif
#endif /* !__BSTRING_H__ */
