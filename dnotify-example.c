/*
 *   File:   dnotify-example.c
 *   Date:   Thu Feb 24 17:45:48 2011
 *   Author: Aleksander Morgado <aleksander@lanedo.com>
 *
 *   A simple tester of dnotify in the Linux kernel.
 *
 *   This program is released in the Public Domain.
 *
 *   Compile with:
 *     $> gcc -o dnotify-example dnotify-example.c
 *
 *   Run as:
 *     $> ./dnotify-example /path/to/monitor /another/path/to/monitor ...
 */

/* Define _GNU_SOURCE, Otherwise we don't get the DN_ symbols */
#define _GNU_SOURCE

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <errno.h>
#include <sys/signalfd.h>

#include <fcntl.h>

/* Structure to keep track of monitored directories */
typedef struct {
  /* Path of the directory */
  char *path;
  /* file descriptor */
  int fd;
} monitored_t;

/* We'll use SIGRTMIN+1 as SIGRTMIN is usually blocked */
#define DNOTIFY_SIGNAL (SIGRTMIN + 1)

/* Enumerate list of FDs to poll */
enum {
  FD_POLL_SIGNAL = 0,
  FD_POLL_MAX
};

/* Setup dnotify notifications (DN) mask. All these defined in fcntl.h */
static const int event_mask =
  (DN_ACCESS |    /* File accessed */
   DN_ATTRIB |    /* File attributes changed */
   DN_CREATE |    /* File created */
   DN_DELETE |    /* File deleted */
   DN_MODIFY |    /* File modified */
   DN_RENAME |    /* File renamed */
   DN_MULTISHOT); /* Don't remove notifier after first event,
                   * keep it */

/* Array of directories being monitored */
static monitored_t *monitors;
static int n_monitors;

static void
__event_process (struct signalfd_siginfo *event)
{
  int i;

  /* Need to loop all registered monitors to find the one corresponding to the
   * file descriptor in the event. A hash table here would be quite a better
   * approach.
   *
   * Note also that in the dnotify case, as we get the FD of the directory
   * where the event happened, we could also readlink() /proc/self/fd/##
   * in order to know the path of the directory corresponding to the FD.
   */
  for (i = 0; i < n_monitors; ++i)
    {
      /* If file descriptors match, we found our directory */
      if (monitors[i].fd == event->ssi_fd)
        {
          printf ("Received event in directory '%s'\n",
                  monitors[i].path);
          fflush (stdout);
          return;
        }
    }
}

static int
__shutdown_dnotify (void)
{
  int i;

  for (i = 0; i < n_monitors; ++i)
    {
      free (monitors[i].path);
      /* Stop monitoring (not actually needed if we just want to close
       * the FD, but anyway) */
      fcntl (monitors[i].fd, F_NOTIFY, 0);
      /* And close file descriptor */
      close (monitors[i].fd);
    }
  free (monitors);
}

static void
__initialize_dnotify (int          argc,
                      const char **argv)
{
  int i;

  /* Allocate array of monitor setups */
  n_monitors = argc - 1;
  monitors = malloc (n_monitors * sizeof (monitored_t));

  /* Loop all input directories, opening FDs for dnotify */
  for (i = 0; i < n_monitors; ++i)
    {
      monitors[i].path = strdup (argv[i + 1]);

      /* Open the directory */
      if ((monitors[i].fd = open (monitors[i].path,
                                  O_RDONLY)) < 0)
        {
          fprintf (stderr,
                   "Couldn't open directory '%s': '%s'\n",
                   monitors[i].path,
                   strerror (errno));
          exit (EXIT_FAILURE);
        }

      /* Initialize directory notifications using SIGRTMIN+1 instead of
       * the default SIGIO */
      if (fcntl (monitors[i].fd, F_SETSIG, DNOTIFY_SIGNAL) < 0 ||
          fcntl (monitors[i].fd, F_NOTIFY, event_mask) < 0)
        {
          fprintf (stderr,
                   "Couldn't setup directory notifications in '%s': '%s'\n",
                   monitors[i].path,
                   strerror (errno));
          exit (EXIT_FAILURE);
        }

      printf ("Started monitoring directory '%s'...\n",
              monitors[i].path);
    }
}

static void
__shutdown_signals (int signal_fd)
{
  close (signal_fd);
}

static int
__initialize_signals (void)
{
  int signal_fd;
  sigset_t sigmask;

  /* We want to handle SIGINT, SIGTERM and SIGRTMIN+1 in the signal_fd,
   * so we block them. */
  sigemptyset (&sigmask);
  sigaddset (&sigmask, SIGINT);
  sigaddset (&sigmask, SIGTERM);
  sigaddset (&sigmask, DNOTIFY_SIGNAL);

  if (sigprocmask (SIG_BLOCK, &sigmask, NULL) < 0)
    {
      fprintf (stderr,
               "Couldn't block signals: '%s'\n",
               strerror (errno));
      return -1;
    }

  /* Get new FD to read signals from it */
  if ((signal_fd = signalfd (-1, &sigmask, 0)) < 0)
    {
      fprintf (stderr,
               "Couldn't setup signal FD: '%s'\n",
               strerror (errno));
      return -1;
    }

  return signal_fd;
}

int main (int          argc,
          const char **argv)
{
  int signal_fd;
  struct pollfd fds[FD_POLL_MAX];

  /* Input arguments... */
  if (argc < 2)
    {
      fprintf (stderr, "Usage: %s directory1 [directory2 ...]\n", argv[0]);
      exit (EXIT_FAILURE);
    }


  /* Initialize signals FD */
  if ((signal_fd = __initialize_signals ()) < 0)
    {
      fprintf (stderr, "Couldn't initialize signals\n");
      exit (EXIT_FAILURE);
    }

  /* Initialize dnotify and the file descriptors */
  __initialize_dnotify (argc, argv);

  /* Setup polling */
  fds[FD_POLL_SIGNAL].fd = signal_fd;
  fds[FD_POLL_SIGNAL].events = POLLIN;

  /* Now loop */
  for (;;)
    {
      /* Block until there is something to be read */
      if (poll (fds, FD_POLL_MAX, -1) < 0)
        {
          fprintf (stderr,
                   "Couldn't poll(): '%s'\n",
                   strerror (errno));
          exit (EXIT_FAILURE);
        }

      /* Signal received? */
      if (fds[FD_POLL_SIGNAL].revents & POLLIN)
        {
          struct signalfd_siginfo fdsi;

          if (read (fds[FD_POLL_SIGNAL].fd,
                    &fdsi,
                    sizeof (fdsi)) != sizeof (fdsi))
            {
              fprintf (stderr,
                       "Couldn't read signal, wrong size read\n");
              exit (EXIT_FAILURE);
            }

          /* Break loop if we got the expected signal */
          if (fdsi.ssi_signo == SIGINT ||
              fdsi.ssi_signo == SIGTERM)
            {
              break;
            }

          /* Process event if dnotify signal received */
          if (fdsi.ssi_signo == DNOTIFY_SIGNAL)
            {
              __event_process (&fdsi);
              continue;
            }

          fprintf (stderr,
                   "Received unexpected signal\n");
        }
    }

  /* Clean exit */
  __shutdown_dnotify ();
  __shutdown_signals (signal_fd);

  printf ("Exiting dnotify example...\n");

  return 0;
}

