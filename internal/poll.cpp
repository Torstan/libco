#include "poll.h"
#include "event.h"
#include "io_backend.h"
#include "co_routine.h"
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace co {
// int poll(struct pollfd fds[], nfds_t nfds, int timeout);
//  { fd,events,revents }
struct PollItem;
struct PollBase {
  detail::WaitRecord wait;
  detail::WaitTimer timer{wait};
  struct pollfd *fds{nullptr};
  nfds_t nfds{0}; // typedef unsigned long int nfds_t;
  PollItem *poll_items{nullptr};
  int raise_cnt{0};
};
struct PollItem : public detail::IoSource {
  struct pollfd *self_pfd{nullptr};
  PollBase *poll{nullptr};
  int registered_fd{-1};
  bool owns_registered_fd{false};

  IoEvent io_event;
};

static int SystemPoll(struct pollfd fds[], nfds_t nfds, int timeout) {
  return ::poll(fds, nfds, timeout);
}

static int DupFdCloseOnExec(int fd) {
#ifdef F_DUPFD_CLOEXEC
  {
    int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd >= 0) {
      return dup_fd;
    }
  }
  int dup_errno = errno;
  if (dup_errno != EINVAL) {
    errno = dup_errno;
    return -1;
  }
#endif

  int dup_fd = dup(fd);
  if (dup_fd < 0) {
    return -1;
  }
  int flags = fcntl(dup_fd, F_GETFD);
  if (flags < 0) {
    int dup_errno = errno;
    close(dup_fd);
    errno = dup_errno;
    return -1;
  }
  if (fcntl(dup_fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
    int dup_errno = errno;
    close(dup_fd);
    errno = dup_errno;
    return -1;
  }
  return dup_fd;
}


static void CleanupPoll(EpollCtx*, pollfd*, PollBase*);
static constexpr nfds_t kStackPollItemCount = 2;

class PollState {
 public:
  PollState(EpollCtx *ep_ctx, struct pollfd fds[], nfds_t nfds)
      : ep_ctx_(ep_ctx), output_(fds), poll_(std::make_unique<PollBase>()) {
    std::unique_ptr<pollfd[]> owned_fds(new pollfd[nfds]);
    for (nfds_t i = 0; i < nfds; ++i) {
      owned_fds[i] = fds[i];
      owned_fds[i].revents = 0;
    }
    std::unique_ptr<PollItem[]> owned_items;
    if (nfds > kStackPollItemCount) {
      owned_items.reset(new PollItem[nfds]);
    }

    poll_->fds = owned_fds.release();
    poll_->nfds = nfds;
    poll_->poll_items =
        nfds <= kStackPollItemCount ? stack_items_ : owned_items.release();
    poll_->wait.SetCleanup([](void* arg) noexcept {
      static_cast<PollState*>(arg)->Detach();
    }, this);
  }

  void Detach() noexcept {
    int saved_errno = errno;
    CleanupPoll(ep_ctx_, output_, poll_.get());
    errno = saved_errno;
  }
  ~PollState() {
    Detach();
    poll_->wait.SetCleanup(nullptr, nullptr);
    if (poll_->poll_items != stack_items_) {
      delete[] poll_->poll_items;
      poll_->poll_items = nullptr;
    }
    delete[] poll_->fds;
    poll_->fds = nullptr;
  }

  PollBase *poll() { return poll_.get(); }

 private:
  EpollCtx* ep_ctx_;
  pollfd* output_;
  std::unique_ptr<PollBase> poll_;
  PollItem stack_items_[kStackPollItemCount];
};
static void PollPrepareFunc(detail::IoSource *source, uint32_t events) {
  PollItem *item = static_cast<PollItem*>(source);
  item->self_pfd->revents |= events;
  item->poll->raise_cnt++;
  item->poll->wait.Complete();
}

enum class PollRegisterResult {
  kRegistered,
  kError,
};

static PollRegisterResult RegisterPollFds(EpollCtx *ep_ctx,
                                          struct pollfd fds[], nfds_t nfds,
                                          PollBase *poll) {
  for (nfds_t i = 0; i < nfds; i++) {
    PollItem &item = poll->poll_items[i];
    item.self_pfd = poll->fds + i;
    item.poll = poll;
    item.registered_fd = -1;
    item.owns_registered_fd = false;

    item.notify = PollPrepareFunc;
    IoEvent &ev = item.io_event;

    if (fds[i].fd > -1) {
      ev.data = &item;
      ev.events = fds[i].events;

      int ret = ep_ctx->add(fds[i].fd, &ev);
      int registered_fd = fds[i].fd;
      bool owns_registered_fd = false;
      if (ret < 0 && errno == EEXIST) {
        int dup_fd = DupFdCloseOnExec(fds[i].fd);
        if (dup_fd >= 0) {
          ret = ep_ctx->add(dup_fd, &ev);
          if (ret == 0) {
            registered_fd = dup_fd;
            owns_registered_fd = true;
          } else {
            int add_errno = errno;
            close(dup_fd);
            errno = add_errno;
          }
        }
      }
      if (ret < 0) {
        return PollRegisterResult::kError;
      }
      item.registered_fd = registered_fd;
      item.owns_registered_fd = owns_registered_fd;
    }
  }
  return PollRegisterResult::kRegistered;
}

static void CleanupPoll(EpollCtx *ep_ctx, struct pollfd fds[], PollBase *poll) {
  poll->timer.Cancel();
  for (nfds_t i = 0; i < poll->nfds; i++) {
    PollItem &item = poll->poll_items[i];
    int fd = item.registered_fd;
    if (fd > -1) {
      ep_ctx->del(fd);
      if (item.owns_registered_fd) {
        close(fd);
      }
      item.registered_fd = -1;
      item.owns_registered_fd = false;
    }
    fds[i].revents = poll->fds[i].revents;
  }
}

static int co_poll_inner(struct pollfd fds[], nfds_t nfds, int timeout,
                  detail::PollFunc poll_func) {
  // Preserve native readiness/error semantics before using the async backend.
  int ready = poll_func ? poll_func(fds, nfds, 0) : SystemPoll(fds, nfds, 0);
  if (ready != 0 || timeout == 0) {
    return ready;
  }
  EpollCtx *ep_ctx = co_get_epoll_ct();
  if (!ep_ctx) {
    if (errno == 0) {
      errno = ENOMEM;
    }
    return -1;
  }
  if (timeout < 0) {
    timeout = INT_MAX;
  }

  std::unique_ptr<PollState> state;
  try {
    state.reset(new PollState(ep_ctx, fds, nfds));
  } catch (const std::bad_alloc &) {
    errno = ENOMEM;
    return -1;
  } catch (const std::logic_error&) {
    errno = EINVAL;
    return -1;
  }
  PollBase *poll = state->poll();
  PollRegisterResult register_result =
      RegisterPollFds(ep_ctx, fds, nfds, poll);
  if (register_result == PollRegisterResult::kError) {
    int register_errno = errno;
    CleanupPoll(ep_ctx, fds, poll);
    errno = register_errno;
    return -1;
  }

  unsigned long long now = GetTickMS();
  int ret = poll->timer.Arm(now + timeout);
  int raise_cnt = 0;
  if (ret != 0) {
    co_log_err(
        "CO_ERR: AddItem ret %d now %lld timeout %d arg.expire_time_ms %lld",
        ret, now, timeout, poll->timer.expire_time_ms);
    errno = EINVAL;
    raise_cnt = -1;

  } else {
    poll->wait.Suspend();
    raise_cnt = poll->raise_cnt;
  }

  CleanupPoll(ep_ctx, fds, poll);
  return raise_cnt;
}

int detail::PollWait(struct pollfd fds[], nfds_t nfds, int timeout_ms, PollFunc poll_func) {
  if (nfds <= 1) {
    return co_poll_inner(fds, nfds, timeout_ms, poll_func);
  }

  std::map<int, nfds_t> fd_to_merged_idx;
  std::unique_ptr<pollfd[]> fds_merge;
  try {
    fds_merge.reset(new pollfd[nfds]);
  } catch (const std::bad_alloc &) {
    errno = ENOMEM;
    return -1;
  }

  nfds_t nfds_merge = 0;
  bool has_duplicate = false;
  for (nfds_t i = 0; i < nfds; ++i) {
    fds[i].revents = 0;
    std::pair<std::map<int, nfds_t>::iterator, bool> ret;
    try {
      ret = fd_to_merged_idx.insert(std::make_pair(fds[i].fd, nfds_merge));
    } catch (const std::bad_alloc &) {
      errno = ENOMEM;
      return -1;
    }
    if (ret.second) {
      fds_merge[nfds_merge] = fds[i];
      fds_merge[nfds_merge].revents = 0;
      ++nfds_merge;
    } else {
      fds_merge[ret.first->second].events |= fds[i].events;
      has_duplicate = true;
    }
  }

  if (!has_duplicate) {
    return co_poll_inner(fds, nfds, timeout_ms, poll_func);
  }

  int ret = co_poll_inner(fds_merge.get(), nfds_merge, timeout_ms, poll_func);
  if (ret <= 0) {
    return ret;
  }

  ret = 0;
  const short always_reported = POLLERR | POLLHUP | POLLNVAL;
  for (nfds_t i = 0; i < nfds; ++i) {
    auto it = fd_to_merged_idx.find(fds[i].fd);
    if (it == fd_to_merged_idx.end()) {
      continue;
    }
    fds[i].revents =
        fds_merge[it->second].revents & (fds[i].events | always_reported);
    if (fds[i].revents) {
      ++ret;
    }
  }
  return ret;
}


} // namespace co
