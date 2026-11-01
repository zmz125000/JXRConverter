#pragma once
#if defined(_WIN32) && !defined(JXRC_STATIC)
# if defined(JXRC_BUILDING_LIBRARY)
#  define JXRC_API __declspec(dllexport)
# else
#  define JXRC_API __declspec(dllimport)
# endif
#else
# define JXRC_API
#endif
