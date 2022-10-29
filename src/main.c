/* vim: set ts=2 et sw=2 : */
/** @file main.c */
/*
 *  T50 - Experimental Mixed Packet Injector
 *
 *  Copyright (C) 2010 - 2019 - T50 developers
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <locale.h>
#include <termios.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h> /* POSIX.1 compliant */
#include <configuration.h>
#include <t50_defines.h>
#include <t50_typedefs.h>
#include <t50_config.h>
#include <t50_netio.h>
#include <t50_errors.h>
#include <t50_cidr.h>
#include <t50_memalloc.h>
#include <t50_modules.h>
#include <t50_randomizer.h>
#include <t50_shuffle.h>
#include <t50_help.h>

static pid_t pid = -1;                 /* -1 is a trick used when __HAVE_TURBO__ isn't defined. */
static sig_atomic_t child_is_dead = 0; /* Used to kill child process if necessary. */
static double t0;                      /* Used to calcualte time spent on T50. */
static int echo_enabled = 1;

_NOINLINE static void               initialize ( const config_options_T * );
_NOINLINE static modules_table_T   *selectProtocol ( const config_options_T *restrict, int *restrict );
static void                         show_statistics ( void );

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

int main ( int argc, char *argv[] )
{
  config_options_T *co;
  struct cidr      *cidr_ptr;
  modules_table_T  *ptbl;
  int              proto;
  time_t           lt;
  struct timeval   tv;

  setlocale ( LC_ALL, "C" );

  show_version();

  /* Parse_command_line() returns ONLY if there are no errors.
     This must be called before testing user privileges. */
  co = parse_command_line ( argv );

  /* If user don't have root privilege, abort. */
  if ( getuid() )
    fatal_error ( "User must have root privilege to run." );

  initialize ( co );
  create_socket();

  /* Calculates CIDR for destination address. */
  if ( ! ( cidr_ptr = config_cidr ( co ) ) )
    return EXIT_FAILURE;

#ifdef  __HAVE_TURBO__

  /* Creates the forked process only if turbo is turned on. */
  if ( co->turbo )
  {
    if ( ( co->ip.protocol == IPPROTO_T50 && co->threshold > number_of_modules ) ||
         ( co->ip.protocol != IPPROTO_T50 && co->threshold > 1 ) )
    {
      threshold_T new_threshold;

      if ( ( pid = fork() ) == -1 )
        fatal_error ( "Cannot create child process" );

      /* Distribute the number of packets between processes */
      new_threshold = co->threshold / 2;

      if ( !IS_CHILD_PID ( pid ) )
        new_threshold += ( co->threshold & 1 );

      co->threshold = new_threshold;
    }
  }

  printf ( INFO "PID=%u\n", getpid() );

#endif  /* __HAVE_TURBO__ */

  /* This process must have higher priority. */
  if ( setpriority ( PRIO_PROCESS, PRIO_PROCESS, -15 )  == -1 )
    fatal_error ( "Cannot set process priority" );

  /* Show launch info only for parent process. */
  if ( !IS_CHILD_PID ( pid ) && !co->quiet )
  {
    lt = time ( NULL );

    printf ( INFO "" PACKAGE_NAME " successfully launched at %s\n",
             ctime ( &lt ) );
  }

  // SRANDOM is here because each process must have its own
  // random seed.
  SRANDOM();

  // Initialize indices used for IPPROTO_T50 shuffling.
  build_proto_indices();

  /* Preallocate packet buffer.
     Register deallocator after successful allocation. */
  alloc_packet ( INITIAL_PACKET_SIZE );
  atexit ( destroy_packet_buffer );

  /* Selects the initial protocol. */
  if ( co->ip.protocol != IPPROTO_T50 )
    ptbl = selectProtocol ( co, &proto );
  else
  {
    proto = co->ip.protocol;
    shuffle ( indices, number_of_modules );   // do initial shuffle.
                                              // this maybe NOT be used afterwards.
    ptbl = &mod_table[get_proto_index ( co )];
  }

  /* Used to calculate the time spent injecting packets */
  gettimeofday ( &tv, NULL );
  t0 = tv.tv_usec * 1e-6 + tv.tv_sec;
  atexit ( show_statistics );                 // Register show_statistics() if
                                              // we got to this point.

  /* MAIN LOOP */
  // OBS: flood means non stop injection.
  //      threshold is the number of packets to inject.
  while ( co->flood || co->threshold )
  {
    /* Will hold the actual packet size after module function call. */
    size_t size;

    /* Set the destination IP address to RANDOM IP address. */
    co->ip.daddr = cidr_ptr->__1st_addr;

    if ( cidr_ptr->hostid )
      // cidr_ptr->hostid has bit 0=0. The remainder is always less
      // then the divisor, so we need to add 1.
      co->ip.daddr += RANDOM() % ( cidr_ptr->hostid + 1 );

    co->ip.daddr = htonl ( co->ip.daddr );

    /* Finally, calls the 'module' function to build the packet. */
    co->ip.protocol = ptbl->protocol_id;
    ptbl->func ( co, &size );

    /* Try to send the packet. */
    if ( ! send_packet ( packet, size, co ) )
#ifndef NDEBUG
      error ( "Packet for protocol %s (%zu bytes long) not sent", ptbl->name, size );

    /* continue trying to send other packets on debug mode! */
#else
      fatal_error ( "Unspecified error sending a packet" );
#endif

    /* If protocol is 'T50', then get the next true protocol. */
    if ( proto == IPPROTO_T50 )
      ptbl = &mod_table[get_proto_index ( co )];

    /* Decrement the threshold only if not flooding! */
    if ( !co->flood )
      co->threshold--;
  }

  /* Show termination message only for parent process. */
  if ( !IS_CHILD_PID ( pid ) )
  {
#ifdef __HAVE_TURBO__

    // NOTE: Notice that for a single process pid will be -1! */
    if ( pid > 0 )
    {
      // Don't do this if child process is already dead!
      if ( !child_is_dead )
      {
        /* Wait 5 seconds for the child to end... */
        alarm ( WAIT_FOR_CHILD_TIMEOUT );

        /* NOTE: SIGALRM will kill the child process if necessary! */
        wait ( NULL );
        child_is_dead = 1;

        alarm ( 0 );
      }
    }

#endif

    close_socket();

    if ( !co->quiet )
    {
      lt = time ( NULL );

      printf ( INFO "" PACKAGE_NAME " successfully finished at %s\n",
               ctime ( &lt ) );
    }
  }

  /* Everything went well. Exit. */
  return EXIT_SUCCESS;
}
#pragma GCC diagnostic pop

/* This function handles signal interrupts. */
static void signal_handler ( int signal )
{
  /* NOTE: SIGALRM and SIGCHLD will happen only in parent process! */
  switch ( signal )
  {
    case SIGALRM:
      if ( !IS_CHILD_PID ( pid ) )
        kill ( pid, SIGKILL );
      return;

    case SIGCHLD:
      child_is_dead = 1;
      return;
  }

  /* Every other signals will exit the process */

  /* The shell documentation (bash) specifies that a process,
     when exits because a signal, must return 128+signal#. */
  exit ( 128 + signal );
}

void initialize ( const config_options_T *co )
{
  /* 0 is an invalid signal! (marks the end of the list) */
  int handled_signals[] = { SIGPIPE, SIGINT, SIGCHLD, SIGALRM, 0 };
  int *sigsp;

  /* allows libc calls to restart after a signal! */
  struct sigaction sa = { .sa_handler = signal_handler, .sa_flags = SA_RESTART };
  sigset_t sigset;

  struct termios tios;

  /* Hide ^X char output from terminal */
  tcgetattr ( STDOUT_FILENO, &tios );
  echo_enabled = tios.c_lflag & ECHO;
  if ( echo_enabled )
    tios.c_lflag &= ~ECHO;
  tcsetattr ( STDOUT_FILENO, TCSANOW, &tios );

  /* Blocks SIGTSTP avoiding ^Z behavior. */
  sigemptyset ( &sigset );
  sigaddset ( &sigset, SIGTSTP );
  /* OBS: SIGSTOP cannot be caught, blocked or ignored! */
  /*      Don't need to block SIGCONT */

  sigprocmask ( SIG_BLOCK, &sigset, NULL );

  /* --- Initialize signal handlers --- */
  /* All these signals are handled by our handle,
     except those blocked previously. */
  sigfillset ( &sigset );
  sa.sa_mask = sigset;
  sigsp = handled_signals;

  while ( *sigsp )
    sigaction ( *sigsp++, &sa, NULL );

  /* To simplify things, make sure stdout is unbuffered
     (otherwise, it's line buffered). */
  fflush ( stdout );
  setvbuf ( stdout, NULL, _IONBF, 0 );

  /* Show some messages. */
  if ( !co->quiet )
  {
    if ( co->flood )
      fputs ( INFO "Entering flood mode...", stdout );
    else
      printf ( INFO "Sending %u packets...\n", co->threshold );

#ifdef __HAVE_TURBO__

    if ( co->turbo )
      puts ( INFO "Turbo mode active..." );

#endif

    if ( co->bits )
      puts ( INFO "Performing stress testing..." );

    puts ( INFO "Hit Ctrl+C to stop..." );
  }
}

/* Selects the initial protocol based on the configuration. */
modules_table_T *selectProtocol ( const config_options_T *restrict co, int *restrict proto )
{
  modules_table_T *ptbl;

  ptbl = mod_table;

  // FIXME: Of course this is a 'hack'. Maybe I should divise something more portable here.
  if ( ( *proto = co->ip.protocol ) != IPPROTO_T50 )
    ptbl += co->ip.protoname;

  return ptbl;
}

void show_statistics ( void )
{
  struct timeval tv;

  // FIXME: This is not as precise as I wanted.
  //        A bunch of microseconds (maybe milisseconds) will
  //        be accounted as the finalization queue routines
  //        are dispatched.
  gettimeofday ( &tv, NULL );

  close_socket(); // NOTE: This will 'flush' the buffers?!

  if ( packets_sent )
  {
    pid_t pid;
    double t1;

    t1 = tv.tv_usec * 1e-6 + tv.tv_sec;

    pid = getpid();
    printf ( INFO "(PID:%1$u) packets:    %2$" PRIu64 " (%3$" PRIu64 " bytes sent).\n"
             INFO "(PID:%1$u) throughput: %4$.2f packets/second.\n",
             pid,
             packets_sent,
             bytes_sent,
             ( double ) packets_sent / ( t1 - t0 ) );
  }
}

_FINI static void dtor ( void )
{
  struct termios tios;

  /* Restore terminal ECHO */
  if ( echo_enabled )
  {
    tcgetattr ( STDOUT_FILENO, &tios );
    tios.c_lflag |= ECHO;
    tcsetattr ( STDOUT_FILENO, TCSANOW, &tios );
  }
}
