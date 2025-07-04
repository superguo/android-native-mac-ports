//
// Copyright 2010 The Android Open Source Project
//
// A looper implementation based on epoll().
//
#define LOG_TAG "Looper"

//#define LOG_NDEBUG 0

// Debugs poll and wake interactions.
#ifndef DEBUG_POLL_AND_WAKE
#define DEBUG_POLL_AND_WAKE 0
#endif

// Debugs callback registration and invocation.
#ifndef DEBUG_CALLBACKS
#define DEBUG_CALLBACKS 0
#endif

#include <utils/Looper.h>
#include <sys/eventfd.h>
#include <cinttypes>

namespace android {

namespace {

constexpr uint64_t WAKE_EVENT_FD_SEQ = 1;

#if HAVE_EPOLL
epoll_event createEpollEvent(uint32_t events, uint64_t seq) {
    return {.events = events, .data = {.u64 = seq}};
}
#elif HAVE_KQUEUE
struct kevent createKqueueEvent(int fd, int16_t filter, uint64_t seq) {
    struct kevent eventItem = {
        .ident = static_cast<uintptr_t>(fd),
        .filter = filter,
        .flags = EV_ADD | EV_ENABLE,
        .fflags = 0,
        .data = 0,
        .udata = reinterpret_cast<void*>(seq)
    };
    return eventItem;
}
Vector<struct kevent> createKqueueEvents(int fd, const Vector<int16_t>& filters, uint64_t seq) {
    Vector<struct kevent> result;
    for (int16_t filter : filters) {
        struct kevent eventItem = {
            .ident = static_cast<uintptr_t>(fd),
            .filter = filter,
            .flags = EV_ADD | EV_ENABLE,
            .fflags = 0,
            .data = 0,
            .udata = reinterpret_cast<void*>(seq)
        };
        result.push_back(eventItem);
    }
    return result;
}

Vector<struct kevent> createKqueueEventsForDelete(int fd, uint64_t seq) {
    Vector<struct kevent> result;
    struct kevent eventItem = {
        .ident = static_cast<uintptr_t>(fd),
        .filter = EVFILT_READ,
        .flags = EV_DELETE,
        .fflags = 0,
        .data = 0,
        .udata = reinterpret_cast<void*>(seq)
    };
    result.push_back(eventItem);
    eventItem.filter = EVFILT_WRITE;
    result.push_back(eventItem);
    return result;
}

#endif

}  // namespace

// --- WeakMessageHandler ---

WeakMessageHandler::WeakMessageHandler(const wp<MessageHandler>& handler) :
        mHandler(handler) {
}

WeakMessageHandler::~WeakMessageHandler() {
}

void WeakMessageHandler::handleMessage(const Message& message) {
    sp<MessageHandler> handler = mHandler.promote();
    if (handler != nullptr) {
        handler->handleMessage(message);
    }
}


// --- SimpleLooperCallback ---

SimpleLooperCallback::SimpleLooperCallback(Looper_callbackFunc callback) :
        mCallback(callback) {
}

SimpleLooperCallback::~SimpleLooperCallback() {
}

int SimpleLooperCallback::handleEvent(int fd, int events, void* data) {
    return mCallback(fd, events, data);
}


// --- Looper ---

// Maximum number of file descriptors for which to retrieve poll events each iteration.
#if HAVE_EPOLL
static const int EPOLL_MAX_EVENTS = 16;
#elif HAVE_KQUEUE
static const int KQUEUE_MAX_EVENTS = 16;
#endif

thread_local static sp<Looper> gThreadLocalLooper;

Looper::Looper(bool allowNonCallbacks)
    : mAllowNonCallbacks(allowNonCallbacks),
      mSendingMessage(false),
      mPolling(false),
      mEpollRebuildRequired(false),
      mNextRequestSeq(WAKE_EVENT_FD_SEQ + 1),
      mResponseIndex(0),
      mNextMessageUptime(LLONG_MAX) {
    mWakeEventFd.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    LOG_ALWAYS_FATAL_IF(mWakeEventFd.get() < 0, "Could not make wake event fd: %s", strerror(errno));

    AutoMutex _l(mLock);
    rebuildEpollLocked();
}

Looper::~Looper() {
}

void Looper::setForThread(const sp<Looper>& looper) {
    gThreadLocalLooper = looper;
}

sp<Looper> Looper::getForThread() {
    return gThreadLocalLooper;
}

sp<Looper> Looper::prepare(int opts) {
    bool allowNonCallbacks = opts & PREPARE_ALLOW_NON_CALLBACKS;
    sp<Looper> looper = Looper::getForThread();
    if (looper == nullptr) {
        looper = sp<Looper>::make(allowNonCallbacks);
        Looper::setForThread(looper);
    }
    if (looper->getAllowNonCallbacks() != allowNonCallbacks) {
        ALOGW("Looper already prepared for this thread with a different value for the "
                "LOOPER_PREPARE_ALLOW_NON_CALLBACKS option.");
    }
    return looper;
}

bool Looper::getAllowNonCallbacks() const {
    return mAllowNonCallbacks;
}

void Looper::rebuildEpollLocked() {
    // Close old epoll instance if we have one.
#if HAVE_EPOLL
    if (mEpollFd >= 0) {
#if DEBUG_CALLBACKS
        ALOGD("%p ~ rebuildEpollLocked - rebuilding epoll set", this);
#endif
        mEpollFd.reset();
    }

    // Allocate the new epoll instance and register the WakeEventFd.
    mEpollFd.reset(epoll_create1(EPOLL_CLOEXEC));
    LOG_ALWAYS_FATAL_IF(mEpollFd < 0, "Could not create epoll instance: %s", strerror(errno));

    epoll_event wakeEvent = createEpollEvent(EPOLLIN, WAKE_EVENT_FD_SEQ);
    int result = epoll_ctl(mEpollFd.get(), EPOLL_CTL_ADD, mWakeEventFd.get(), &wakeEvent);
    LOG_ALWAYS_FATAL_IF(result != 0, "Could not add wake event fd to epoll instance: %s",
                        strerror(errno));

    for (const auto& [seq, request] : mRequests) {
        epoll_event eventItem = createEpollEvent(request.getEpollEvents(), seq);

        int epollResult = epoll_ctl(mEpollFd.get(), EPOLL_CTL_ADD, request.fd, &eventItem);
        if (epollResult < 0) {
            ALOGE("Error adding epoll events for fd %d while rebuilding epoll set: %s",
                  request.fd, strerror(errno));
        }
    }
#elif HAVE_KQUEUE
    if (mKqueueFd >= 0) {
        ALOGD("%p ~ rebuildKqueueLocked - rebuilding kqueue set", this);
        mKqueueFd.reset();
    }
    // Allocate the new kqueue instance and register the WakeEventFd.
    mKqueueFd.reset(kqueue());
    LOG_ALWAYS_FATAL_IF(mKqueueFd < 0, "Could not create kqueue instance: %s", strerror(errno));
    struct kevent wakeEvent = createKqueueEvent(mWakeEventFd.get(), EVFILT_READ, WAKE_EVENT_FD_SEQ);
    int result = kevent(mKqueueFd.get(), &wakeEvent, 1, nullptr, 0, nullptr);
    LOG_ALWAYS_FATAL_IF(result < 0, "Could not add wake event fd to kqueue instance: %s",
                        strerror(errno));
    for (const auto& [seq, request] : mRequests) {
        Vector<struct kevent> eventItems = createKqueueEvents(request.fd, request.getKqueueFilters(), seq);

        for (const auto& eventItem : eventItems) {
            int kqueueResult = kevent(mKqueueFd.get(), &eventItem, 1, nullptr, 0, nullptr);
            if (kqueueResult < 0) {
                ALOGE("Error adding kqueue events for fd %d while rebuilding kqueue set: %s",
                      request.fd, strerror(errno));
            }
        }
    }
#endif
}

void Looper::scheduleEpollRebuildLocked() {
    if (!mEpollRebuildRequired) {
#if DEBUG_CALLBACKS
        ALOGD("%p ~ scheduleEpollRebuildLocked - scheduling epoll set rebuild", this);
#endif
        mEpollRebuildRequired = true;
        wake();
    }
}

int Looper::pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
    int result = 0;
    for (;;) {
        while (mResponseIndex < mResponses.size()) {
            const Response& response = mResponses.itemAt(mResponseIndex++);
            int ident = response.request.ident;
            if (ident >= 0) {
                int fd = response.request.fd;
                int events = response.events;
                void* data = response.request.data;
#if DEBUG_POLL_AND_WAKE
                ALOGD("%p ~ pollOnce - returning signalled identifier %d: "
                        "fd=%d, events=0x%x, data=%p",
                        this, ident, fd, events, data);
#endif
                if (outFd != nullptr) *outFd = fd;
                if (outEvents != nullptr) *outEvents = events;
                if (outData != nullptr) *outData = data;
                return ident;
            }
        }

        if (result != 0) {
#if DEBUG_POLL_AND_WAKE
            ALOGD("%p ~ pollOnce - returning result %d", this, result);
#endif
            if (outFd != nullptr) *outFd = 0;
            if (outEvents != nullptr) *outEvents = 0;
            if (outData != nullptr) *outData = nullptr;
            return result;
        }

        result = pollInner(timeoutMillis);
    }
}

int Looper::pollInner(int timeoutMillis) {
#if DEBUG_POLL_AND_WAKE
    ALOGD("%p ~ pollOnce - waiting: timeoutMillis=%d", this, timeoutMillis);
#endif

    // Adjust the timeout based on when the next message is due.
    if (timeoutMillis != 0 && mNextMessageUptime != LLONG_MAX) {
        nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
        int messageTimeoutMillis = toMillisecondTimeoutDelay(now, mNextMessageUptime);
        if (messageTimeoutMillis >= 0
                && (timeoutMillis < 0 || messageTimeoutMillis < timeoutMillis)) {
            timeoutMillis = messageTimeoutMillis;
        }
#if DEBUG_POLL_AND_WAKE
        ALOGD("%p ~ pollOnce - next message in %" PRId64 "ns, adjusted timeout: timeoutMillis=%d",
                this, mNextMessageUptime - now, timeoutMillis);
#endif
    }

    // Poll.
    int result = POLL_WAKE;
    mResponses.clear();
    mResponseIndex = 0;

    // We are about to idle.
    mPolling = true;

#if HAVE_EPOLL
    struct epoll_event eventItems[EPOLL_MAX_EVENTS];
    int eventCount = epoll_wait(mEpollFd.get(), eventItems, EPOLL_MAX_EVENTS, timeoutMillis);
#elif HAVE_KQUEUE
    struct kevent eventItems[KQUEUE_MAX_EVENTS];
    struct timespec timeout = {.tv_sec = timeoutMillis / 1000, .tv_nsec = (timeoutMillis % 1000) * 1000000};
    int eventCount = kevent(mKqueueFd.get(), nullptr, 0, eventItems, KQUEUE_MAX_EVENTS,
                            timeoutMillis < 0 ? nullptr : &timeout);
#endif

    // No longer idling.
    mPolling = false;

    // Acquire lock.
    mLock.lock();

    // Rebuild epoll set if needed.
    if (mEpollRebuildRequired) {
        mEpollRebuildRequired = false;
        rebuildEpollLocked();
        goto Done;
    }

    // Check for poll error.
    if (eventCount < 0) {
        if (errno == EINTR) {
            goto Done;
        }
        ALOGW("Poll failed with an unexpected error: %s", strerror(errno));
        result = POLL_ERROR;
        goto Done;
    }

    // Check for poll timeout.
    if (eventCount == 0) {
#if DEBUG_POLL_AND_WAKE
        ALOGD("%p ~ pollOnce - timeout", this);
#endif
        result = POLL_TIMEOUT;
        goto Done;
    }

    // Handle all events.
#if DEBUG_POLL_AND_WAKE
    ALOGD("%p ~ pollOnce - handling events from %d fds", this, eventCount);
#endif

    for (int i = 0; i < eventCount; i++) {
        const SequenceNumber seq =
#if HAVE_EPOLL
            eventItems[i].data.u64;
            uint32_t epollEvents = eventItems[i].events;
#elif HAVE_KQUEUE
            reinterpret_cast<uint64_t>(eventItems[i].udata);
            int16_t kqueueFilter = eventItems[i].filter;
            uint16_t flags = eventItems[i].flags;
#endif
        if (seq == WAKE_EVENT_FD_SEQ) {
#if HAVE_EPOLL
            if (epollEvents & EPOLLIN) {
#elif HAVE_KQUEUE
            if (kqueueFilter == EVFILT_READ) {
#endif
                awoken();
            } else {
#if HAVE_EPOLL
                ALOGW("Ignoring unexpected epoll events 0x%x on wake event fd.", epollEvents);
#elif HAVE_KQUEUE
                ALOGW("Ignoring unexpected kqueue events 0x%x on wake event fd.", kqueueFilter);
#endif
            }
        } else {
            const auto& request_it = mRequests.find(seq);
            if (request_it != mRequests.end()) {
                const auto& request = request_it->second;
                int events = 0; 
#if HAVE_EPOLL
                if (epollEvents & EPOLLIN) events |= EVENT_INPUT;
                if (epollEvents & EPOLLOUT) events |= EVENT_OUTPUT;
                if (epollEvents & EPOLLERR) events |= EVENT_ERROR;
                if (epollEvents & EPOLLHUP) events |= EVENT_HANGUP;
                mResponses.push({.seq = seq, .events = events, .request = request});
#elif HAVE_KQUEUE
                if (kqueueFilter == EVFILT_READ) events |= EVENT_INPUT;
                if (kqueueFilter == EVFILT_WRITE) events |= EVENT_OUTPUT;
                if (flags & EV_ERROR) events |= EVENT_ERROR;
                if (flags & EV_EOF) events |= EVENT_HANGUP;
                mResponses.push({.seq = seq, .events = events, .request = request});
#endif
            } else {
#if HAVE_EPOLL
                ALOGW("Ignoring unexpected epoll events 0x%x for sequence number %" PRIu64
                      " that is no longer registered.",
                      epollEvents, seq);
#elif HAVE_KQUEUE
                ALOGW("Ignoring unexpected kqueue events %" PRIi16 " for sequence number %" PRIu64
                      " and ident=%" PRIuPTR " that is no longer registered.",
                      kqueueFilter, seq, eventItems[i].ident);
#endif
            }
        }
    }
Done: ;

    // Invoke pending message callbacks.
    mNextMessageUptime = LLONG_MAX;
    while (mMessageEnvelopes.size() != 0) {
        nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
        const MessageEnvelope& messageEnvelope = mMessageEnvelopes.itemAt(0);
        if (messageEnvelope.uptime <= now) {
            // Remove the envelope from the list.
            // We keep a strong reference to the handler until the call to handleMessage
            // finishes.  Then we drop it so that the handler can be deleted *before*
            // we reacquire our lock.
            { // obtain handler
                sp<MessageHandler> handler = messageEnvelope.handler;
                Message message = messageEnvelope.message;
                mMessageEnvelopes.removeAt(0);
                mSendingMessage = true;
                mLock.unlock();

#if DEBUG_POLL_AND_WAKE || DEBUG_CALLBACKS
                ALOGD("%p ~ pollOnce - sending message: handler=%p, what=%d",
                        this, handler.get(), message.what);
#endif
                handler->handleMessage(message);
            } // release handler

            mLock.lock();
            mSendingMessage = false;
            result = POLL_CALLBACK;
        } else {
            // The last message left at the head of the queue determines the next wakeup time.
            mNextMessageUptime = messageEnvelope.uptime;
            break;
        }
    }

    // Release lock.
    mLock.unlock();

    // Invoke all response callbacks.
    for (size_t i = 0; i < mResponses.size(); i++) {
        Response& response = mResponses.editItemAt(i);
        if (response.request.ident == POLL_CALLBACK) {
            int fd = response.request.fd;
            int events = response.events;
            void* data = response.request.data;
#if DEBUG_POLL_AND_WAKE || DEBUG_CALLBACKS
            ALOGD("%p ~ pollOnce - invoking fd event callback %p: fd=%d, events=0x%x, data=%p",
                    this, response.request.callback.get(), fd, events, data);
#endif
            // Invoke the callback.  Note that the file descriptor may be closed by
            // the callback (and potentially even reused) before the function returns so
            // we need to be a little careful when removing the file descriptor afterwards.
            int callbackResult = response.request.callback->handleEvent(fd, events, data);
            if (callbackResult == 0) {
                AutoMutex _l(mLock);
                removeSequenceNumberLocked(response.seq);
            }

            // Clear the callback reference in the response structure promptly because we
            // will not clear the response vector itself until the next poll.
            response.request.callback.clear();
            result = POLL_CALLBACK;
        }
    }
    return result;
}

int Looper::pollAll(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
    if (timeoutMillis <= 0) {
        int result;
        do {
            result = pollOnce(timeoutMillis, outFd, outEvents, outData);
        } while (result == POLL_CALLBACK);
        return result;
    } else {
        nsecs_t endTime = systemTime(SYSTEM_TIME_MONOTONIC)
                + milliseconds_to_nanoseconds(timeoutMillis);

        for (;;) {
            int result = pollOnce(timeoutMillis, outFd, outEvents, outData);
            if (result != POLL_CALLBACK) {
                return result;
            }

            nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
            timeoutMillis = toMillisecondTimeoutDelay(now, endTime);
            if (timeoutMillis == 0) {
                return POLL_TIMEOUT;
            }
        }
    }
}

void Looper::wake() {
#if DEBUG_POLL_AND_WAKE
    ALOGD("%p ~ wake", this);
#endif

    uint64_t inc = 1;
    ssize_t nWrite = TEMP_FAILURE_RETRY(write(mWakeEventFd.get(), &inc, sizeof(uint64_t)));
    if (nWrite != sizeof(uint64_t)) {
        if (errno != EAGAIN) {
            LOG_ALWAYS_FATAL("Could not write wake signal to fd %d (returned %zd): %s",
                             mWakeEventFd.get(), nWrite, strerror(errno));
        }
    }
}

void Looper::awoken() {
#if DEBUG_POLL_AND_WAKE
    ALOGD("%p ~ awoken", this);
#endif

    uint64_t counter;
    TEMP_FAILURE_RETRY(read(mWakeEventFd.get(), &counter, sizeof(uint64_t)));
}

int Looper::addFd(int fd, int ident, int events, Looper_callbackFunc callback, void* data) {
    sp<SimpleLooperCallback> looperCallback;
    if (callback) {
        looperCallback = sp<SimpleLooperCallback>::make(callback);
    }
    return addFd(fd, ident, events, looperCallback, data);
}

int Looper::addFd(int fd, int ident, int events, const sp<LooperCallback>& callback, void* data) {
#if DEBUG_CALLBACKS
    ALOGD("%p ~ addFd - fd=%d, ident=%d, events=0x%x, callback=%p, data=%p", this, fd, ident,
            events, callback.get(), data);
#endif

    if (!callback.get()) {
        if (! mAllowNonCallbacks) {
            ALOGE("Invalid attempt to set NULL callback but not allowed for this looper.");
            return -1;
        }

        if (ident < 0) {
            ALOGE("Invalid attempt to set NULL callback with ident < 0.");
            return -1;
        }
    } else {
        ident = POLL_CALLBACK;
    }

    { // acquire lock
        AutoMutex _l(mLock);
        // There is a sequence number reserved for the WakeEventFd.
        if (mNextRequestSeq == WAKE_EVENT_FD_SEQ) mNextRequestSeq++;
        const SequenceNumber seq = mNextRequestSeq++;

        Request request;
        request.fd = fd;
        request.ident = ident;
        request.events = events;
        request.callback = callback;
        request.data = data;
#if HAVE_EPOLL
        epoll_event eventItem = createEpollEvent(request.getEpollEvents(), seq);
        auto seq_it = mSequenceNumberByFd.find(fd);
        if (seq_it == mSequenceNumberByFd.end()) {
            int epollResult = epoll_ctl(mEpollFd.get(), EPOLL_CTL_ADD, fd, &eventItem);
            if (epollResult < 0) {
                ALOGE("Error adding epoll events for fd %d: %s", fd, strerror(errno));
                return -1;
            }
            mRequests.emplace(seq, request);
            mSequenceNumberByFd.emplace(fd, seq);
        } else {
            int epollResult = epoll_ctl(mEpollFd.get(), EPOLL_CTL_MOD, fd, &eventItem);
            if (epollResult < 0) {
                if (errno == ENOENT) {
                    // Tolerate ENOENT because it means that an older file descriptor was
                    // closed before its callback was unregistered and meanwhile a new
                    // file descriptor with the same number has been created and is now
                    // being registered for the first time.  This error may occur naturally
                    // when a callback has the side-effect of closing the file descriptor
                    // before returning and unregistering itself.  Callback sequence number
                    // checks further ensure that the race is benign.
                    //
                    // Unfortunately due to kernel limitations we need to rebuild the epoll
                    // set from scratch because it may contain an old file handle that we are
                    // now unable to remove since its file descriptor is no longer valid.
                    // No such problem would have occurred if we were using the poll system
                    // call instead, but that approach carries other disadvantages.
#if DEBUG_CALLBACKS
                    ALOGD("%p ~ addFd - EPOLL_CTL_MOD failed due to file descriptor "
                            "being recycled, falling back on EPOLL_CTL_ADD: %s",
                            this, strerror(errno));
#endif
                    epollResult = epoll_ctl(mEpollFd.get(), EPOLL_CTL_ADD, fd, &eventItem);
                    if (epollResult < 0) {
                        ALOGE("Error modifying or adding epoll events for fd %d: %s",
                                fd, strerror(errno));
                        return -1;
                    }
                    scheduleEpollRebuildLocked();
                } else {
                    ALOGE("Error modifying epoll events for fd %d: %s", fd, strerror(errno));
                    return -1;
                }
            }
            const SequenceNumber oldSeq = seq_it->second;
            mRequests.erase(oldSeq);
            mRequests.emplace(seq, request);
            seq_it->second = seq;
        }
#elif HAVE_KQUEUE
        Vector<struct kevent> eventItems = createKqueueEvents(fd, request.getKqueueFilters(), seq);
        auto seq_it = mSequenceNumberByFd.find(fd);
        if (seq_it == mSequenceNumberByFd.end()) {
            int kqueueResult = kevent(mKqueueFd.get(), eventItems.array(), eventItems.size(),
                                      nullptr, 0, nullptr);
            if (kqueueResult < 0) {
                ALOGE("Error adding kqueue events for fd %d: %s", fd, strerror(errno));
                return -1;
            }
            mRequests.emplace(seq, request);
            mSequenceNumberByFd.emplace(fd, seq);
            ALOGD("%p ~ addFd - added fd %d with seq %" PRIu64, this, fd, seq);
        } else {
            int kqueueResult = kevent(mKqueueFd.get(), eventItems.array(), eventItems.size(),
                                      nullptr, 0, nullptr);
            if (kqueueResult < 0) {
                if (errno == ENOENT) {
                    // Tolerate ENOENT because it means that an older file descriptor was
                    // closed before its callback was unregistered and meanwhile a new
                    // file descriptor with the same number has been created and is now
                    // being registered for the first time.  This error may occur naturally
                    // when a callback has the side-effect of closing the file descriptor
                    // before returning and unregistering itself.  Callback sequence number
                    // checks further ensure that the correct callback is unregistered.
                    // This is safe because we hold the lock while making these changes.
                    // If a new callback is registered for the same FD, it will get a new
                    // sequence number and won't interfere with the old one.
               }
                ALOGE("Error modifying kqueue events for fd %d: %s", fd, strerror(errno));
                return -1;
            }
            const SequenceNumber oldSeq = seq_it->second;
            mRequests.erase(oldSeq);
            mRequests.emplace(seq, request);
            seq_it->second = seq;
            ALOGD("%p ~ addFd - modified fd %d with seq %" PRIu64, this, fd, seq);
        }
#endif
    } // release lock
    return 1;
}

bool Looper::getFdStateDebug(int fd, int* ident, int* events, sp<LooperCallback>* cb, void** data) {
    AutoMutex _l(mLock);
    if (auto seqNumIt = mSequenceNumberByFd.find(fd); seqNumIt != mSequenceNumberByFd.cend()) {
        if (auto reqIt = mRequests.find(seqNumIt->second); reqIt != mRequests.cend()) {
            const Request& request = reqIt->second;
            if (ident) *ident = request.ident;
            if (events) *events = request.events;
            if (cb) *cb = request.callback;
            if (data) *data = request.data;
            return true;
        }
    }
    return false;
}

int Looper::removeFd(int fd) {
    AutoMutex _l(mLock);
    const auto& it = mSequenceNumberByFd.find(fd);
    if (it == mSequenceNumberByFd.end()) {
        return 0;
    }
    return removeSequenceNumberLocked(it->second);
}

int Looper::repoll(int fd) {
    AutoMutex _l(mLock);
    const auto& it = mSequenceNumberByFd.find(fd);
    if (it == mSequenceNumberByFd.end()) {
        return 0;
    }

    const auto& request_it = mRequests.find(it->second);
    if (request_it == mRequests.end()) {
        return 0;
    }
    const auto& [seq, request] = *request_it;

    LOG_ALWAYS_FATAL_IF(
            fd != request.fd,
            "Looper has inconsistent data structure. When looking up FD %d found FD %d.", fd,
            request_it->second.fd);

#if HAVE_EPOLL
    epoll_event eventItem = createEpollEvent(request.getEpollEvents(), seq);
    if (epoll_ctl(mEpollFd.get(), EPOLL_CTL_MOD, fd, &eventItem) == -1) return 0;
#elif HAVE_KQUEUE
    Vector<struct kevent> eventItems = createKqueueEvents(fd, request.getKqueueFilters(), seq);
    if (kevent(mKqueueFd.get(), eventItems.array(), eventItems.size(),
               nullptr, 0, nullptr) == -1) {
        return 0;
    }
#endif

    return 1;  // success
}

int Looper::removeSequenceNumberLocked(SequenceNumber seq) {
#if DEBUG_CALLBACKS
    ALOGD("%p ~ removeFd - seq=%" PRIu64, this, seq);
#endif

    const auto& request_it = mRequests.find(seq);
    if (request_it == mRequests.end()) {
        return 0;
    }
    const int fd = request_it->second.fd;

    // Always remove the FD from the request map even if an error occurs while
    // updating the epoll set so that we avoid accidentally leaking callbacks.
    mRequests.erase(request_it);
    mSequenceNumberByFd.erase(fd);

#if HAVE_EPOLL
    int epollResult = epoll_ctl(mEpollFd.get(), EPOLL_CTL_DEL, fd, nullptr);
    if (epollResult < 0) {
#elif HAVE_KQUEUE
    Vector<struct kevent> eventItems = createKqueueEventsForDelete(mKqueueFd.get(), seq);
    int kqueueResult = kevent(mKqueueFd.get(), eventItems.array(), eventItems.size(), nullptr, 0, nullptr);
    if (kqueueResult < 0) {
#endif
        if (errno == EBADF || errno == ENOENT) {
            // Tolerate EBADF or ENOENT because it means that the file descriptor was closed
            // before its callback was unregistered. This error may occur naturally when a
            // callback has the side-effect of closing the file descriptor before returning and
            // unregistering itself.
            //
            // Unfortunately due to kernel limitations we need to rebuild the epoll
            // set from scratch because it may contain an old file handle that we are
            // now unable to remove since its file descriptor is no longer valid.
            // No such problem would have occurred if we were using the poll system
            // call instead, but that approach carries other disadvantages.
#if DEBUG_CALLBACKS
            ALOGD("%p ~ removeFd - EPOLL_CTL_DEL failed due to file descriptor "
                  "being closed: %s",
                  this, strerror(errno));
#endif
            scheduleEpollRebuildLocked();
        } else {
            // Some other error occurred.  This is really weird because it means
            // our list of callbacks got out of sync with the epoll set somehow.
            // We defensively rebuild the epoll set to avoid getting spurious
            // notifications with nowhere to go.
            ALOGE("Error removing epoll events for fd %d: %s", fd, strerror(errno));
            scheduleEpollRebuildLocked();
            return -1;
        }
        ALOGD("%p ~ removeFd - removed fd %d with seq %" PRIu64, this, fd, seq);
    }
    return 1;
}

void Looper::sendMessage(const sp<MessageHandler>& handler, const Message& message) {
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    sendMessageAtTime(now, handler, message);
}

void Looper::sendMessageDelayed(nsecs_t uptimeDelay, const sp<MessageHandler>& handler,
        const Message& message) {
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    sendMessageAtTime(now + uptimeDelay, handler, message);
}

void Looper::sendMessageAtTime(nsecs_t uptime, const sp<MessageHandler>& handler,
        const Message& message) {
#if DEBUG_CALLBACKS
    ALOGD("%p ~ sendMessageAtTime - uptime=%" PRId64 ", handler=%p, what=%d",
            this, uptime, handler.get(), message.what);
#endif

    size_t i = 0;
    { // acquire lock
        AutoMutex _l(mLock);

        size_t messageCount = mMessageEnvelopes.size();
        while (i < messageCount && uptime >= mMessageEnvelopes.itemAt(i).uptime) {
            i += 1;
        }

        MessageEnvelope messageEnvelope(uptime, handler, message);
        mMessageEnvelopes.insertAt(messageEnvelope, i, 1);

        // Optimization: If the Looper is currently sending a message, then we can skip
        // the call to wake() because the next thing the Looper will do after processing
        // messages is to decide when the next wakeup time should be.  In fact, it does
        // not even matter whether this code is running on the Looper thread.
        if (mSendingMessage) {
            return;
        }
    } // release lock

    // Wake the poll loop only when we enqueue a new message at the head.
    if (i == 0) {
        wake();
    }
}

void Looper::removeMessages(const sp<MessageHandler>& handler) {
#if DEBUG_CALLBACKS
    ALOGD("%p ~ removeMessages - handler=%p", this, handler.get());
#endif

    { // acquire lock
        AutoMutex _l(mLock);

        for (size_t i = mMessageEnvelopes.size(); i != 0; ) {
            const MessageEnvelope& messageEnvelope = mMessageEnvelopes.itemAt(--i);
            if (messageEnvelope.handler == handler) {
                mMessageEnvelopes.removeAt(i);
            }
        }
    } // release lock
}

void Looper::removeMessages(const sp<MessageHandler>& handler, int what) {
#if DEBUG_CALLBACKS
    ALOGD("%p ~ removeMessages - handler=%p, what=%d", this, handler.get(), what);
#endif

    { // acquire lock
        AutoMutex _l(mLock);

        for (size_t i = mMessageEnvelopes.size(); i != 0; ) {
            const MessageEnvelope& messageEnvelope = mMessageEnvelopes.itemAt(--i);
            if (messageEnvelope.handler == handler
                    && messageEnvelope.message.what == what) {
                mMessageEnvelopes.removeAt(i);
            }
        }
    } // release lock
}

bool Looper::isPolling() const {
    return mPolling;
}

#if HAVE_EPOLL
uint32_t Looper::Request::getEpollEvents() const {
    uint32_t epollEvents = 0;
    if (events & EVENT_INPUT) epollEvents |= EPOLLIN;
    if (events & EVENT_OUTPUT) epollEvents |= EPOLLOUT;
    return epollEvents;
}
#elif HAVE_KQUEUE
Vector<int16_t> Looper::Request::getKqueueFilters() const {
    Vector<int16_t> filters;
    if (events & EVENT_INPUT) filters.push_back(EVFILT_READ);
    if (events & EVENT_OUTPUT) filters.push_back(EVFILT_WRITE);
    return filters;
}
#endif

MessageHandler::~MessageHandler() { }

LooperCallback::~LooperCallback() { }

} // namespace android