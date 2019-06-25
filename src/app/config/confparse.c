/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2019, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file confparse.c
 *
 * \brief Back-end for parsing and generating key-value files, used to
 *   implement the torrc file format and the state file.
 *
 * This module is used by config.c to parse and encode torrc
 * configuration files, and by statefile.c to parse and encode the
 * $DATADIR/state file.
 *
 * To use this module, its callers provide an instance of
 * config_format_t to describe the mappings from a set of configuration
 * options to a number of fields in a C structure.  With this mapping,
 * the functions here can convert back and forth between the C structure
 * specified, and a linked list of key-value pairs.
 */

#define CONFPARSE_PRIVATE
#include "core/or/or.h"
#include "app/config/confparse.h"
#include "feature/nodelist/routerset.h"

#include "lib/confmgt/unitparse.h"
#include "lib/container/bitarray.h"
#include "lib/encoding/confline.h"
#include "lib/confmgt/structvar.h"

static void config_reset(const config_format_t *fmt, void *options,
                         const config_var_t *var, int use_defaults);

/** Allocate an empty configuration object of a given format type. */
void *
config_new(const config_format_t *fmt)
{
  void *opts = tor_malloc_zero(fmt->size);
  struct_set_magic(opts, &fmt->magic);
  CONFIG_CHECK(fmt, opts);
  return opts;
}

/*
 * Functions to parse config options
 */

/** If <b>option</b> is an official abbreviation for a longer option,
 * return the longer option.  Otherwise return <b>option</b>.
 * If <b>command_line</b> is set, apply all abbreviations.  Otherwise, only
 * apply abbreviations that work for the config file and the command line.
 * If <b>warn_obsolete</b> is set, warn about deprecated names. */
const char *
config_expand_abbrev(const config_format_t *fmt, const char *option,
                     int command_line, int warn_obsolete)
{
  int i;
  if (! fmt->abbrevs)
    return option;
  for (i=0; fmt->abbrevs[i].abbreviated; ++i) {
    /* Abbreviations are case insensitive. */
    if (!strcasecmp(option,fmt->abbrevs[i].abbreviated) &&
        (command_line || !fmt->abbrevs[i].commandline_only)) {
      if (warn_obsolete && fmt->abbrevs[i].warn) {
        log_warn(LD_CONFIG,
                 "The configuration option '%s' is deprecated; "
                 "use '%s' instead.",
                 fmt->abbrevs[i].abbreviated,
                 fmt->abbrevs[i].full);
      }
      /* Keep going through the list in case we want to rewrite it more.
       * (We could imagine recursing here, but I don't want to get the
       * user into an infinite loop if we craft our list wrong.) */
      option = fmt->abbrevs[i].full;
    }
  }
  return option;
}

/** If <b>key</b> is a deprecated configuration option, return the message
 * explaining why it is deprecated (which may be an empty string). Return NULL
 * if it is not deprecated. The <b>key</b> field must be fully expanded. */
const char *
config_find_deprecation(const config_format_t *fmt, const char *key)
{
  if (BUG(fmt == NULL) || BUG(key == NULL))
    return NULL; // LCOV_EXCL_LINE
  if (fmt->deprecations == NULL)
    return NULL;

  const config_deprecation_t *d;
  for (d = fmt->deprecations; d->name; ++d) {
    if (!strcasecmp(d->name, key)) {
      return d->why_deprecated ? d->why_deprecated : "";
    }
  }
  return NULL;
}

/** If <b>key</b> is a configuration option, return the corresponding const
 * config_var_t.  Otherwise, if <b>key</b> is a non-standard abbreviation,
 * warn, and return the corresponding const config_var_t.  Otherwise return
 * NULL.
 */
const config_var_t *
config_find_option(const config_format_t *fmt, const char *key)
{
  int i;
  size_t keylen = strlen(key);
  if (!keylen)
    return NULL; /* if they say "--" on the command line, it's not an option */
  /* First, check for an exact (case-insensitive) match */
  for (i=0; fmt->vars[i].member.name; ++i) {
    if (!strcasecmp(key, fmt->vars[i].member.name)) {
      return &fmt->vars[i];
    }
  }
  /* If none, check for an abbreviated match */
  for (i=0; fmt->vars[i].member.name; ++i) {
    if (!strncasecmp(key, fmt->vars[i].member.name, keylen)) {
      log_warn(LD_CONFIG, "The abbreviation '%s' is deprecated. "
               "Please use '%s' instead",
               key, fmt->vars[i].member.name);
      return &fmt->vars[i];
    }
  }
  /* Okay, unrecognized option */
  return NULL;
}

/** Return the number of option entries in <b>fmt</b>. */
static int
config_count_options(const config_format_t *fmt)
{
  int i;
  for (i=0; fmt->vars[i].member.name; ++i)
    ;
  return i;
}

bool
config_var_is_cumulative(const config_var_t *var)
{
  return struct_var_is_cumulative(&var->member);
}
bool
config_var_is_settable(const config_var_t *var)
{
  if (var->flags & CVFLAG_OBSOLETE)
    return false;
  return struct_var_is_settable(&var->member);
}
bool
config_var_is_contained(const config_var_t *var)
{
  return struct_var_is_contained(&var->member);
}

/*
 * Functions to assign config options.
 */

/** <b>c</b>-\>key is known to be a real key. Update <b>options</b>
 * with <b>c</b>-\>value and return 0, or return -1 if bad value.
 *
 * Called from config_assign_line() and option_reset().
 */
static int
config_assign_value(const config_format_t *fmt, void *options,
                    config_line_t *c, char **msg)
{
  const config_var_t *var;

  CONFIG_CHECK(fmt, options);

  var = config_find_option(fmt, c->key);
  tor_assert(var);
  tor_assert(!strcmp(c->key, var->member.name));

  return struct_var_kvassign(options, c, msg, &var->member);
}

/** Mark every linelist in <b>options</b> "fragile", so that fresh assignments
 * to it will replace old ones. */
static void
config_mark_lists_fragile(const config_format_t *fmt, void *options)
{
  int i;
  tor_assert(fmt);
  tor_assert(options);

  for (i = 0; fmt->vars[i].member.name; ++i) {
    const config_var_t *var = &fmt->vars[i];
    struct_var_mark_fragile(options, &var->member);
  }
}

void
warn_deprecated_option(const char *what, const char *why)
{
  const char *space = (why && strlen(why)) ? " " : "";
  log_warn(LD_CONFIG, "The %s option is deprecated, and will most likely "
           "be removed in a future version of Tor.%s%s (If you think this is "
           "a mistake, please let us know!)",
           what, space, why);
}

/** If <b>c</b> is a syntactically valid configuration line, update
 * <b>options</b> with its value and return 0.  Otherwise return -1 for bad
 * key, -2 for bad value.
 *
 * If <b>clear_first</b> is set, clear the value first. Then if
 * <b>use_defaults</b> is set, set the value to the default.
 *
 * Called from config_assign().
 */
static int
config_assign_line(const config_format_t *fmt, void *options,
                   config_line_t *c, unsigned flags,
                   bitarray_t *options_seen, char **msg)
{
  const unsigned use_defaults = flags & CAL_USE_DEFAULTS;
  const unsigned clear_first = flags & CAL_CLEAR_FIRST;
  const unsigned warn_deprecations = flags & CAL_WARN_DEPRECATIONS;
  const config_var_t *var;

  CONFIG_CHECK(fmt, options);

  var = config_find_option(fmt, c->key);
  if (!var) {
    if (fmt->extra) {
      void *lvalue = STRUCT_VAR_P(options, fmt->extra->offset);
      log_info(LD_CONFIG,
               "Found unrecognized option '%s'; saving it.", c->key);
      config_line_append((config_line_t**)lvalue, c->key, c->value);
      return 0;
    } else {
      tor_asprintf(msg,
                "Unknown option '%s'.  Failing.", c->key);
      return -1;
    }
  }

  /* Put keyword into canonical case. */
  if (strcmp(var->member.name, c->key)) {
    tor_free(c->key);
    c->key = tor_strdup(var->member.name);
  }

  const char *deprecation_msg;
  if (warn_deprecations &&
      (deprecation_msg = config_find_deprecation(fmt, var->member.name))) {
    warn_deprecated_option(var->member.name, deprecation_msg);
  }

  if (!strlen(c->value)) {
    /* reset or clear it, then return */
    if (!clear_first) {
      if (config_var_is_cumulative(var) && c->command != CONFIG_LINE_CLEAR) {
        /* We got an empty linelist from the torrc or command line.
           As a special case, call this an error. Warn and ignore. */
        log_warn(LD_CONFIG,
                 "Linelist option '%s' has no value. Skipping.", c->key);
      } else { /* not already cleared */
        config_reset(fmt, options, var, use_defaults);
      }
    }
    return 0;
  } else if (c->command == CONFIG_LINE_CLEAR && !clear_first) {
    // XXXX This is unreachable, since a CLEAR line always has an
    // XXXX empty value.
    config_reset(fmt, options, var, use_defaults); // LCOV_EXCL_LINE
  }

  if (options_seen && ! config_var_is_cumulative(var)) {
    /* We're tracking which options we've seen, and this option is not
     * supposed to occur more than once. */
    int var_index = (int)(var - fmt->vars);
    if (bitarray_is_set(options_seen, var_index)) {
      log_warn(LD_CONFIG, "Option '%s' used more than once; all but the last "
               "value will be ignored.", var->member.name);
    }
    bitarray_set(options_seen, var_index);
  }

  if (config_assign_value(fmt, options, c, msg) < 0)
    return -2;
  return 0;
}

/** Restore the option named <b>key</b> in options to its default value.
 * Called from config_assign(). */
STATIC void
config_reset_line(const config_format_t *fmt, void *options,
                  const char *key, int use_defaults)
{
  const config_var_t *var;

  CONFIG_CHECK(fmt, options);

  var = config_find_option(fmt, key);
  if (!var)
    return; /* give error on next pass. */

  config_reset(fmt, options, var, use_defaults);
}

/** Return true iff value needs to be quoted and escaped to be used in
 * a configuration file. */
static int
config_value_needs_escape(const char *value)
{
  if (*value == '\"')
    return 1;
  while (*value) {
    switch (*value)
    {
    case '\r':
    case '\n':
    case '#':
      /* Note: quotes and backspaces need special handling when we are using
       * quotes, not otherwise, so they don't trigger escaping on their
       * own. */
      return 1;
    default:
      if (!TOR_ISPRINT(*value))
        return 1;
    }
    ++value;
  }
  return 0;
}

/** Return newly allocated line or lines corresponding to <b>key</b> in the
 * configuration <b>options</b>.  If <b>escape_val</b> is true and a
 * value needs to be quoted before it's put in a config file, quote and
 * escape that value. Return NULL if no such key exists. */
config_line_t *
config_get_assigned_option(const config_format_t *fmt, const void *options,
                           const char *key, int escape_val)
{
  const config_var_t *var;
  config_line_t *result;
  tor_assert(options && key);

  CONFIG_CHECK(fmt, options);

  var = config_find_option(fmt, key);
  if (!var) {
    log_warn(LD_CONFIG, "Unknown option '%s'.  Failing.", key);
    return NULL;
  }

  result = struct_var_kvencode(options, &var->member);

  if (escape_val) {
    config_line_t *line;
    for (line = result; line; line = line->next) {
      if (line->value && config_value_needs_escape(line->value)) {
        char *newval = esc_for_log(line->value);
        tor_free(line->value);
        line->value = newval;
      }
    }
  }

  return result;
}
/** Iterate through the linked list of requested options <b>list</b>.
 * For each item, convert as appropriate and assign to <b>options</b>.
 * If an item is unrecognized, set *msg and return -1 immediately,
 * else return 0 for success.
 *
 * If <b>clear_first</b>, interpret config options as replacing (not
 * extending) their previous values. If <b>clear_first</b> is set,
 * then <b>use_defaults</b> to decide if you set to defaults after
 * clearing, or make the value 0 or NULL.
 *
 * Here are the use cases:
 * 1. A non-empty AllowInvalid line in your torrc. Appends to current
 *    if linelist, replaces current if csv.
 * 2. An empty AllowInvalid line in your torrc. Should clear it.
 * 3. "RESETCONF AllowInvalid" sets it to default.
 * 4. "SETCONF AllowInvalid" makes it NULL.
 * 5. "SETCONF AllowInvalid=foo" clears it and sets it to "foo".
 *
 * Use_defaults   Clear_first
 *    0                0       "append"
 *    1                0       undefined, don't use
 *    0                1       "set to null first"
 *    1                1       "set to defaults first"
 * Return 0 on success, -1 on bad key, -2 on bad value.
 *
 * As an additional special case, if a LINELIST config option has
 * no value and clear_first is 0, then warn and ignore it.
 */

/*
There are three call cases for config_assign() currently.

Case one: Torrc entry
options_init_from_torrc() calls config_assign(0, 0)
  calls config_assign_line(0, 0).
    if value is empty, calls config_reset(0) and returns.
    calls config_assign_value(), appends.

Case two: setconf
options_trial_assign() calls config_assign(0, 1)
  calls config_reset_line(0)
    calls config_reset(0)
      calls option_clear().
  calls config_assign_line(0, 1).
    if value is empty, returns.
    calls config_assign_value(), appends.

Case three: resetconf
options_trial_assign() calls config_assign(1, 1)
  calls config_reset_line(1)
    calls config_reset(1)
      calls option_clear().
      calls config_assign_value(default)
  calls config_assign_line(1, 1).
    returns.
*/
int
config_assign(const config_format_t *fmt, void *options, config_line_t *list,
              unsigned config_assign_flags, char **msg)
{
  config_line_t *p;
  bitarray_t *options_seen;
  const int n_options = config_count_options(fmt);
  const unsigned clear_first = config_assign_flags & CAL_CLEAR_FIRST;
  const unsigned use_defaults = config_assign_flags & CAL_USE_DEFAULTS;

  CONFIG_CHECK(fmt, options);

  /* pass 1: normalize keys */
  for (p = list; p; p = p->next) {
    const char *full = config_expand_abbrev(fmt, p->key, 0, 1);
    if (strcmp(full,p->key)) {
      tor_free(p->key);
      p->key = tor_strdup(full);
    }
  }

  /* pass 2: if we're reading from a resetting source, clear all
   * mentioned config options, and maybe set to their defaults. */
  if (clear_first) {
    for (p = list; p; p = p->next)
      config_reset_line(fmt, options, p->key, use_defaults);
  }

  options_seen = bitarray_init_zero(n_options);
  /* pass 3: assign. */
  while (list) {
    int r;
    if ((r=config_assign_line(fmt, options, list, config_assign_flags,
                              options_seen, msg))) {
      bitarray_free(options_seen);
      return r;
    }
    list = list->next;
  }
  bitarray_free(options_seen);

  /** Now we're done assigning a group of options to the configuration.
   * Subsequent group assignments should _replace_ linelists, not extend
   * them. */
  config_mark_lists_fragile(fmt, options);

  return 0;
}

/** Reset config option <b>var</b> to 0, 0.0, NULL, or the equivalent.
 * Called from config_reset() and config_free(). */
static void
config_clear(const config_format_t *fmt, void *options,
             const config_var_t *var)
{

  (void)fmt; /* unused */

  struct_var_free(options, &var->member);
}

/** Clear the option indexed by <b>var</b> in <b>options</b>. Then if
 * <b>use_defaults</b>, set it to its default value.
 * Called by config_init() and option_reset_line() and option_assign_line(). */
static void
config_reset(const config_format_t *fmt, void *options,
             const config_var_t *var, int use_defaults)
{
  config_line_t *c;
  char *msg = NULL;
  CONFIG_CHECK(fmt, options);
  config_clear(fmt, options, var); /* clear it first */
  if (!use_defaults)
    return; /* all done */
  if (var->initvalue) {
    c = tor_malloc_zero(sizeof(config_line_t));
    c->key = tor_strdup(var->member.name);
    c->value = tor_strdup(var->initvalue);
    if (config_assign_value(fmt, options, c, &msg) < 0) {
      // LCOV_EXCL_START
      log_warn(LD_BUG, "Failed to assign default: %s", msg);
      tor_free(msg); /* if this happens it's a bug */
      // LCOV_EXCL_STOP
    }
    config_free_lines(c);
  }
}

/** Release storage held by <b>options</b>. */
void
config_free_(const config_format_t *fmt, void *options)
{
  int i;

  if (!options)
    return;

  tor_assert(fmt);

  for (i=0; fmt->vars[i].member.name; ++i)
    config_clear(fmt, options, &(fmt->vars[i]));

  if (fmt->extra) {
    config_line_t **linep = STRUCT_VAR_P(options, fmt->extra->offset);
    config_free_lines(*linep);
    *linep = NULL;
  }
  tor_free(options);
}

/** Return true iff the option <b>name</b> has the same value in <b>o1</b>
 * and <b>o2</b>.  Must not be called for LINELIST_S or OBSOLETE options.
 */
int
config_is_same(const config_format_t *fmt,
               const void *o1, const void *o2,
               const char *name)
{
  CONFIG_CHECK(fmt, o1);
  CONFIG_CHECK(fmt, o2);

  const config_var_t *var = config_find_option(fmt, name);
  if (!var) {
    return true;
  }

  return struct_var_eq(o1, o2, &var->member);
}

/** Copy storage held by <b>old</b> into a new or_options_t and return it. */
void *
config_dup(const config_format_t *fmt, const void *old)
{
  void *newopts;
  int i;

  newopts = config_new(fmt);
  for (i=0; fmt->vars[i].member.name; ++i) {
    if (config_var_is_contained(&fmt->vars[i])) {
      // Something else will copy this option, or it doesn't need copying.
      continue;
    }
    if (struct_var_copy(newopts, old, &fmt->vars[i].member) < 0) {
      // LCOV_EXCL_START
      log_err(LD_BUG, "Unable to copy value for %s.",
              fmt->vars[i].member.name);
      tor_assert_unreached();
      // LCOV_EXCL_STOP
    }
  }
  return newopts;
}
/** Set all vars in the configuration object <b>options</b> to their default
 * values. */
void
config_init(const config_format_t *fmt, void *options)
{
  int i;
  const config_var_t *var;
  CONFIG_CHECK(fmt, options);

  for (i=0; fmt->vars[i].member.name; ++i) {
    var = &fmt->vars[i];
    if (!var->initvalue)
      continue; /* defaults to NULL or 0 */
    config_reset(fmt, options, var, 1);
  }
}

/** Allocate and return a new string holding the written-out values of the vars
 * in 'options'.  If 'minimal', do not write out any default-valued vars.
 * Else, if comment_defaults, write default values as comments.
 */
char *
config_dump(const config_format_t *fmt, const void *default_options,
            const void *options, int minimal,
            int comment_defaults)
{
  smartlist_t *elements;
  const void *defaults = default_options;
  void *defaults_tmp = NULL;
  config_line_t *line, *assigned;
  char *result;
  int i;
  char *msg = NULL;

  if (defaults == NULL) {
    defaults = defaults_tmp = config_new(fmt);
    config_init(fmt, defaults_tmp);
  }

  /* XXX use a 1 here so we don't add a new log line while dumping */
  if (default_options == NULL) {
    if (fmt->validate_fn(NULL, defaults_tmp, defaults_tmp, 1, &msg) < 0) {
      // LCOV_EXCL_START
      log_err(LD_BUG, "Failed to validate default config: %s", msg);
      tor_free(msg);
      tor_assert(0);
      // LCOV_EXCL_STOP
    }
  }

  elements = smartlist_new();
  for (i=0; fmt->vars[i].member.name; ++i) {
    int comment_option = 0;
    if (config_var_is_contained(&fmt->vars[i])) {
      // Something else will dump this option, or it doesn't need dumping.
      continue;
    }
    /* Don't save 'hidden' control variables. */
    if (fmt->vars[i].flags & CVFLAG_NODUMP)
      continue;
    if (minimal && config_is_same(fmt, options, defaults,
                                  fmt->vars[i].member.name))
      continue;
    else if (comment_defaults &&
             config_is_same(fmt, options, defaults, fmt->vars[i].member.name))
      comment_option = 1;

    line = assigned =
      config_get_assigned_option(fmt, options, fmt->vars[i].member.name, 1);

    for (; line; line = line->next) {
      if (!strcmpstart(line->key, "__")) {
        /* This check detects "hidden" variables inside LINELIST_V structures.
         */
        continue;
      }
      smartlist_add_asprintf(elements, "%s%s %s\n",
                   comment_option ? "# " : "",
                   line->key, line->value);
    }
    config_free_lines(assigned);
  }

  if (fmt->extra) {
    line = *(config_line_t**)STRUCT_VAR_P(options, fmt->extra->offset);
    for (; line; line = line->next) {
      smartlist_add_asprintf(elements, "%s %s\n", line->key, line->value);
    }
  }

  result = smartlist_join_strings(elements, "", 0, NULL);
  SMARTLIST_FOREACH(elements, char *, cp, tor_free(cp));
  smartlist_free(elements);
  if (defaults_tmp) {
    fmt->free_fn(defaults_tmp);
  }
  return result;
}

/**
 * Return true if every member of <b>options</b> is in-range and well-formed.
 * Return false otherwise.  Log errors at level <b>severity</b>.
 */
bool
config_check_ok(const config_format_t *fmt, const void *options, int severity)
{
  bool all_ok = true;
  for (int i=0; fmt->vars[i].member.name; ++i) {
    if (!struct_var_ok(options, &fmt->vars[i].member)) {
      log_fn(severity, LD_BUG, "Invalid value for %s",
             fmt->vars[i].member.name);
      all_ok = false;
    }
  }
  return all_ok;
}
