#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#define CHK(expr, errcode) if((expr)==errcode) perror(#expr), exit(EXIT_FAILURE)
int main(int argc, char** argv) {
  int fan;
  char buf[4096];
  char fdpath[32];
  char path[PATH_MAX + 1];
  ssize_t buflen, linklen;
  struct fanotify_event_metadata *metadata;
  CHK(fan = fanotify_init(FAN_CLASS_NOTIF, O_RDONLY), -1);
  CHK(fanotify_mark(fan, FAN_MARK_ADD | FAN_MARK_MOUNT,
                    FAN_OPEN | FAN_EVENT_ON_CHILD, AT_FDCWD, "/"), -1);
  for (;;) {
    CHK(buflen = read(fan, buf, sizeof(buf)), -1);
    metadata = (struct fanotify_event_metadata*)&buf;
    while(FAN_EVENT_OK(metadata, buflen)) {
      if (metadata->mask & FAN_Q_OVERFLOW) {
        printf("Queue overflow!\n");
        continue;
      }
      sprintf(fdpath, "/proc/self/fd/%d", metadata->fd);
      CHK(linklen = readlink(fdpath, path, sizeof(path) - 1), -1);
      path[linklen] = '\0';
      printf("%s opened by process %d.\n", path, (int)metadata->pid);
      close(metadata->fd);
      metadata = FAN_EVENT_NEXT(metadata, buflen);
    }
  }
}
