#ifndef COPYFAST_SIGNALS_H
#define COPYFAST_SIGNALS_H

#include "copyfast.h"

void signals_init(void);
void signals_block_in_thread(void);
bool signals_is_interrupted(void);

#endif /* COPYFAST_SIGNALS_H */
