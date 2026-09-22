#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <Carbon/Carbon.h>

static gid_t probe_gid;
static uid_t probe_uid;
static pid_t probe_pid;
static mode_t probe_mode;
static CFStringRef probe_string;
static CFAbsoluteTime probe_time;

int main(void)
{
    int result;

    result = 0;
    result += (int)probe_gid;
    result += (int)probe_uid;
    result += (int)probe_pid;
    result += (int)probe_mode;
    result += EILSEQ;

    if (probe_string != NULL) {
        result++;
    }

    if (probe_time != 0) {
        result++;
    }

    return result == -1;
}
