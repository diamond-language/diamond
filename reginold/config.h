#ifndef REGINOLD_CONFIG_H
#define REGINOLD_CONFIG_H

#include "vendor/onigmo/config.h"

#ifndef RB_GNUC_EXTENSION
# define RB_GNUC_EXTENSION __extension__
#endif

#ifndef RB_GNUC_EXTENSION_BLOCK
# define RB_GNUC_EXTENSION_BLOCK(x) __extension__ ({ x; })
#endif

#ifndef UNREACHABLE_RETURN
# define UNREACHABLE_RETURN(x) return (x)
#endif

#endif
