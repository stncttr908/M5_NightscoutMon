#ifndef _M5NSNIGHTSCOUT_H
#define _M5NSNIGHTSCOUT_H

#include "M5NSconfig.h"

// Fetches the latest glucose readings and properties from a Nightscout instance.
// Returns 0 on success, or an HTTP error / parse error code.
int readNightscout(tConfig *cfg, struct NSinfo *ns);

#endif
