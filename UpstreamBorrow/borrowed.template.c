// This file is processed by UpstreamBorrow.py. To see the generated output:
// buck build --out=- fbcode//cinderx/UpstreamBorrow:gen_borrowed.c

#include "cinderx/UpstreamBorrow/borrowed.h"

// @Borrow CPP directives from Objects/genobject.c

// Internal dependencies for _PyGen_yf which only exist in 3.12.
// @Borrow function is_resume from Objects/genobject.c [3.12]
// @Borrow function _PyGen_GetCode from Objects/genobject.c [3.12]
// End internal dependencies for _PyGen_yf.

#define _PyGen_yf Cix_PyGen_yf
// @Borrow function _PyGen_yf from Objects/genobject.c
