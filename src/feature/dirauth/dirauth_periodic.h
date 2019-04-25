/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2019, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#ifndef DIRVOTE_PERIODIC_H
#define DIRVOTE_PERIODIC_H

void dirauth_add_periodic_events(void);
void reschedule_dirvote(const or_options_t *options);

#endif
