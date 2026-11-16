/* SH_QUEST: small POSIX shims for Windows names the port calls. */
#if defined(__ANDROID__)
#include <sys/stat.h>
#include <sys/types.h>

int _mkdir(const char* dirname)
{
    return mkdir(dirname, 0777);
}
#endif
