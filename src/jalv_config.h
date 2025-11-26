// Copyright 2021-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

/*
  Configuration header that defines reasonable defaults at compile time.

  This allows compile-time configuration from the command line, while still
  allowing the source to be built "as-is" without any configuration.  The idea
  is to support an advanced build system with configuration checks, while still
  allowing the code to be simply "thrown at a compiler" with features
  determined from the compiler or system headers.  Everything can be
  overridden, so it should never be necessary to edit this file to build
  successfully.

  To ensure that all configure checks are performed, the build system can
  define JALV_NO_DEFAULT_CONFIG to disable defaults.  In this case, it must
  define all HAVE_FEATURE symbols below to 1 or 0 to enable or disable
  features.  Any missing definitions will generate a compiler warning.

  To ensure that this header is always included properly, all code that uses
  configuration variables includes this header and checks their value with #if
  (not #ifdef).  Variables like USE_FEATURE are internal and should never be
  defined on the command line.
*/

#ifndef JALV_CONFIG_H
#define JALV_CONFIG_H

// Define version unconditionally so a warning will catch a mismatch
#define JALV_VERSION "1.8.0"

#if !defined(JALV_NO_DEFAULT_CONFIG)

// We need unistd.h to check _POSIX_VERSION
#  ifndef JALV_NO_POSIX
#    ifdef __has_include
#      if __has_include(<unistd.h>)
#        include <unistd.h>
#      endif
#    elif defined(__unix__)
#      include <unistd.h>
#    endif
#  endif

// POSIX.1-2001: fileno()
#  ifndef HAVE_FILENO
#    if defined(_POSIX_VERSION) && _POSIX_VERSION >= 200112L
#      define HAVE_FILENO 1
#    else
#      define HAVE_FILENO 0
#    endif
#  endif

// POSIX.1-2001: isatty()
#  ifndef HAVE_ISATTY
#    if defined(_POSIX_VERSION) && _POSIX_VERSION >= 200112L
#      define HAVE_ISATTY 1
#    else
#      define HAVE_ISATTY 0
#    endif
#  endif

// POSIX.1-2001: poll()
#  ifndef HAVE_POLL
#    if defined(_POSIX_VERSION) && _POSIX_VERSION >= 200112L
#      define HAVE_POLL 1
#    else
#      define HAVE_POLL 0
#    endif
#  endif

// POSIX.1-2001: posix_memalign()
#  ifndef HAVE_POSIX_MEMALIGN
#    if defined(_POSIX_VERSION) && _POSIX_VERSION >= 200112L
#      define HAVE_POSIX_MEMALIGN 1
#    else
#      define HAVE_POSIX_MEMALIGN 0
#    endif
#  endif

// POSIX.1-2001: sigaction()
#  ifndef HAVE_SIGACTION
#    if defined(_POSIX_VERSION) && _POSIX_VERSION >= 200112L
#      define HAVE_SIGACTION 1
#    else
#      define HAVE_SIGACTION 0
#    endif
#  endif

// Suil
#  ifndef HAVE_SUIL
#    ifdef __has_include
#      if __has_include("suil/suil.h")
#        define HAVE_SUIL 1
#      else
#        define HAVE_SUIL 0
#      endif
#    endif
#  endif

// JACK metadata API
#  ifndef HAVE_JACK_METADATA
#    ifdef __has_include
#      if __has_include("jack/metadata.h")
#        define HAVE_JACK_METADATA 1
#      else
#        define HAVE_JACK_METADATA 0
#      endif
#    endif
#  endif

// JACK jack_port_type_get_buffer_size() function
#  ifndef HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE
#    ifdef __has_include
#      if __has_include("jack/midiport.h")
#        define HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE 1
#      else
#        define HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE 0
#      endif
#    endif
#  endif

#endif // !defined(JALV_NO_DEFAULT_CONFIG)

/*
  Make corresponding USE_FEATURE defines based on the HAVE_FEATURE defines from
  above or the command line.  The code checks for these using #if (not #ifdef),
  so there will be an undefined warning if it checks for an unknown feature,
  and this header is always required by any code that checks for features, even
  if the build system defines them all.
*/

#if HAVE_FILENO
#  define USE_FILENO 1
#else
#  define USE_FILENO 0
#endif

#if HAVE_ISATTY
#  define USE_ISATTY 1
#else
#  define USE_ISATTY 0
#endif

#if HAVE_POLL
#  define USE_POLL 1
#else
#  define USE_POLL 0
#endif

#if HAVE_POSIX_MEMALIGN
#  define USE_POSIX_MEMALIGN 1
#else
#  define USE_POSIX_MEMALIGN 0
#endif

#if HAVE_SIGACTION
#  define USE_SIGACTION 1
#else
#  define USE_SIGACTION 0
#endif

#if HAVE_SUIL
#  define USE_SUIL 1
#else
#  define USE_SUIL 0
#endif

#if HAVE_JACK_METADATA
#  define USE_JACK_METADATA 1
#else
#  define USE_JACK_METADATA 0
#endif

#if HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE
#  define USE_JACK_PORT_TYPE_GET_BUFFER_SIZE 1
#else
#  define USE_JACK_PORT_TYPE_GET_BUFFER_SIZE 0
#endif

#endif // JALV_CONFIG_H
