#pragma once
#include <poll.h>
namespace co { namespace detail {
using PollFunc = int (*)(pollfd*, nfds_t, int);
int PollWait(pollfd* fds, nfds_t nfds, int timeout, PollFunc native_poll);
}}
