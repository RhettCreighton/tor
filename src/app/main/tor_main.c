/* Copyright 2001-2004 Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2021, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#include "orconfig.h"
#ifdef ENABLE_RESTART_DEBUGGING
#include <stdlib.h>
#endif
#include <stdlib.h>
#include <string.h>

/**
 * \file tor_main.c
 * \brief Stub module containing a main() function.
 *
 * We keep the main function in a separate module so that the unit
 * tests, which have their own main()s, can link against main.c.
 **/

int tor_main(int argc, char *argv[]);

/** We keep main() in a separate file so that our unit tests can use
 * functions from main.c.
 */
int
main(int argc, char *argv[])
{
  int r;
#ifdef ENABLE_RESTART_DEBUGGING
  int restart_count = getenv("TOR_DEBUG_RESTART") ? 1 : 0;
 again:
#endif
  
  /* Check if we need to inject default dynhost options */
  int inject_defaults = 1;
  for (int i = 1; i < argc; i++) {
    if (strstr(argv[i], "--SocksPort") || strstr(argv[i], "SocksPort")) {
      inject_defaults = 0;
      break;
    }
  }
  
  if (inject_defaults) {
    /* Create new argv with default options for dynhost */
    char **new_argv = malloc(sizeof(char*) * (argc + 5));
    new_argv[0] = argv[0];
    new_argv[1] = "--SocksPort";
    new_argv[2] = "9052";  /* Different port to avoid conflict with Tor Browser */
    new_argv[3] = "--ControlPort";
    new_argv[4] = "9053";  /* Different control port too */
    for (int i = 1; i < argc; i++) {
      new_argv[i + 4] = argv[i];
    }
    argc += 4;
    argv = new_argv;
  }
  
  r = tor_main(argc, argv);
  if (r < 0 || r > 255)
    return 1;
#ifdef ENABLE_RESTART_DEBUGGING
  else if (r == 0 && restart_count--)
    goto again;
#endif
  else
    return r;
}

