// Landlock is Confinement's implementation on Linux.
//
// The types and the contract moved to sandbox/confine.hpp (cross-platform) and
// sandbox/confine_posix.hpp (Linux-private). This header is now only a
// forwarder, so that every include on the POSIX side did not have to change.
#pragma once

#include "sandbox/confine.hpp"
#ifndef _WIN32
#include "sandbox/confine_posix.hpp"
#endif
