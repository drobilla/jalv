// Copyright 2007-2026 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "parse_command.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int
is_alpha(const int c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int
is_symbol(const int c)
{
  return (c == '_') || is_alpha(c);
}

static int
is_symbol_start(const int c)
{
  return is_symbol(c) || (c >= '0' && c <= '9');
}

static size_t
skip_whitespace(const char* const s, size_t i)
{
  while (isspace(s[i])) {
    ++i;
  }
  return i;
}

static CommandStatus
check_end(const CommandStatus     success,
          const char* const       command,
          CommandArguments* const args,
          const size_t            i)
{
  args->caret = skip_whitespace(command, i);
  for (; command[args->caret]; ++args->caret) {
    if (!isspace(command[args->caret])) {
      return COMMAND_EXPECTED_END;
    }
  }

  return success;
}

CommandStatus
parse_command(const char* const command, CommandArguments* const args)
{
  size_t            i   = skip_whitespace(command, 0U);
  const char* const cmd = &command[i];

  if (!strncmp(cmd, "help", 4)) {
    return check_end(COMMAND_HELP, command, args, i + 4U);
  }

  if (!strncmp(cmd, "presets", 7)) {
    return check_end(COMMAND_PRESETS, command, args, i + 7U);
  }

  if (!strncmp(cmd, "preset", 6)) {
    i = skip_whitespace(cmd, i + 6U);
    if (!is_alpha(cmd[i])) {
      args->caret = i;
      return COMMAND_EXPECTED_ALPHA;
    }

    args->name        = &cmd[i];
    args->name_length = 0U;
    while (cmd[i] && !isspace(cmd[i])) {
      ++i;
      ++args->name_length;
    }

    return check_end(COMMAND_PRESET_URI, command, args, i);
  }

  if (!strncmp(cmd, "controls", 8U)) {
    return check_end(COMMAND_CONTROLS, command, args, i + 8U);
  }

  if (!strncmp(cmd, "monitors", 8U)) {
    return check_end(COMMAND_MONITORS, command, args, i + 8U);
  }

  if (!strncmp(cmd, "quit", 4U)) {
    return check_end(COMMAND_QUIT, command, args, i + 4U);
  }

  if (!strncmp(cmd, "set ", 4)) {
    i = skip_whitespace(cmd, i + 4U);
    if (isdigit(cmd[i])) { // set INDEX VALUE
      char*      endptr = NULL;
      const long index  = strtol(cmd + i, &endptr, 10);

      i += (endptr - (cmd + i));
      if (index < 0 || index > UINT32_MAX || (*endptr && !isspace(*endptr))) {
        args->caret = i;
        return COMMAND_EXPECTED_DIGIT;
      }

      i           = skip_whitespace(cmd, i);
      args->index = (uint32_t)index;
      args->value = &cmd[i];
      return check_end(COMMAND_SET_INDEX_VALUE, command, args, i);
    }

    if (!is_symbol_start(cmd[i])) {
      args->caret = i;
      return COMMAND_EXPECTED_SYMBOL_FIRST;
    }

    args->name = cmd + i;
    while (isgraph(args->name[args->name_length])) {
      ++args->name_length;
      ++i;
    }

    i           = skip_whitespace(cmd, i);
    args->caret = i;
    if (!cmd[i]) {
      return COMMAND_EXPECTED_VALUE;
    }

    args->value = &cmd[i];
    return COMMAND_SET_SYMBOL_VALUE;
  }

  return COMMAND_ERROR;
}
