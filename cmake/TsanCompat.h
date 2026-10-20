#pragma once
// GCC 11 libtsan predates glibc pthread_cond_clockwait interception. Force the
// equivalent portable libstdc++ timed-wait implementation, in the engine AND
// instrumented dependencies. No race/deadlock detector is disabled/suppressed.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 11
#include <bits/c++config.h>
#undef _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT
#endif
