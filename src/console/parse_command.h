// Copyright 2026 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include <stddef.h>
#include <stdint.h>

typedef enum {
  COMMAND_SUCCESS,
  COMMAND_ERROR,
  COMMAND_EXPECTED_ALPHA,
  COMMAND_EXPECTED_DIGIT,
  COMMAND_EXPECTED_SYMBOL_FIRST,
  COMMAND_EXPECTED_SYMBOL_REST,
  COMMAND_EXPECTED_CONTROL,
  COMMAND_EXPECTED_VALUE,
  COMMAND_EXPECTED_END,
  COMMAND_HELP,             ///< help
  COMMAND_CONTROLS,         ///< controls
  COMMAND_MONITORS,         ///< monitors
  COMMAND_PRESETS,          ///< presets
  COMMAND_PRESET_URI,       ///< preset URI
  COMMAND_QUIT,             ///< quit
  COMMAND_SET_INDEX_VALUE,  ///< set INDEX VALUE
  COMMAND_SET_SYMBOL_VALUE, ///< set SYMBOL VALUE
} CommandStatus;

typedef struct {
  size_t      caret;       ///< Offset into command string
  size_t      name_length; ///< Length of name in bytes
  const char* name;        ///< URI, SYMBOL
  uint32_t    index;       ///< INDEX
  const char* value;       ///< VALUE
} CommandArguments;

CommandStatus
parse_command(const char* command, CommandArguments* args);
