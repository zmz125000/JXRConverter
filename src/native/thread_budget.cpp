#include "hdr_parallel.h"
namespace { thread_local unsigned budget = 0; }
unsigned jxrc_thread_budget() { return budget; }
void jxrc_set_thread_budget(unsigned value) { budget = value; }
