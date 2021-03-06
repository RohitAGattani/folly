/*
 * Copyright 2015-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

// included by Future.h, do not include directly.

namespace folly {

template <class> class Promise;

template <class T>
class SemiFuture;

template <typename T>
struct isSemiFuture : std::false_type {
  using Inner = typename Unit::Lift<T>::type;
};

template <typename T>
struct isSemiFuture<SemiFuture<T>> : std::true_type {
  typedef T Inner;
};

template <typename T>
struct isFuture : std::false_type {
  using Inner = typename Unit::Lift<T>::type;
};

template <typename T>
struct isFuture<Future<T>> : std::true_type {
  typedef T Inner;
};

template <typename T>
struct isFutureOrSemiFuture : std::false_type {
  using Inner = typename Unit::Lift<T>::type;
  using Return = Inner;
};

template <typename T>
struct isFutureOrSemiFuture<Future<T>> : std::true_type {
  typedef T Inner;
  using Return = Future<Inner>;
};

template <typename T>
struct isFutureOrSemiFuture<SemiFuture<T>> : std::true_type {
  typedef T Inner;
  using Return = SemiFuture<Inner>;
};

template <typename T>
struct isTry : std::false_type {};

template <typename T>
struct isTry<Try<T>> : std::true_type {};

namespace futures {
namespace detail {

template <class> class Core;
template <class...> struct CollectAllVariadicContext;
template <class...> struct CollectVariadicContext;
template <class> struct CollectContext;

template <typename F, typename... Args>
using resultOf = decltype(std::declval<F>()(std::declval<Args>()...));

template <typename...>
struct ArgType;

template <typename Arg, typename... Args>
struct ArgType<Arg, Args...> {
  typedef Arg FirstArg;
};

template <>
struct ArgType<> {
  typedef void FirstArg;
};

template <bool isTry, typename F, typename... Args>
struct argResult {
  using Result = resultOf<F, Args...>;
};

template <typename F, typename... Args>
struct callableWith {
    template <typename T, typename = detail::resultOf<T, Args...>>
    static constexpr std::true_type
    check(std::nullptr_t) { return std::true_type{}; }

    template <typename>
    static constexpr std::false_type
    check(...) { return std::false_type{}; }

    typedef decltype(check<F>(nullptr)) type;
    static constexpr bool value = type::value;
};

template <typename T, typename F>
struct callableResult {
  typedef typename std::conditional<
    callableWith<F>::value,
    detail::argResult<false, F>,
    typename std::conditional<
      callableWith<F, T&&>::value,
      detail::argResult<false, F, T&&>,
      typename std::conditional<
        callableWith<F, T&>::value,
        detail::argResult<false, F, T&>,
        typename std::conditional<
          callableWith<F, Try<T>&&>::value,
          detail::argResult<true, F, Try<T>&&>,
          detail::argResult<true, F, Try<T>&>>::type>::type>::type>::type Arg;
  typedef isFutureOrSemiFuture<typename Arg::Result> ReturnsFuture;
  typedef Future<typename ReturnsFuture::Inner> Return;
};

template <typename L>
struct Extract : Extract<decltype(&L::operator())> { };

template <typename Class, typename R, typename... Args>
struct Extract<R(Class::*)(Args...) const> {
  typedef isFutureOrSemiFuture<R> ReturnsFuture;
  typedef Future<typename ReturnsFuture::Inner> Return;
  typedef typename ReturnsFuture::Inner RawReturn;
  typedef typename ArgType<Args...>::FirstArg FirstArg;
};

template <typename Class, typename R, typename... Args>
struct Extract<R(Class::*)(Args...)> {
  typedef isFutureOrSemiFuture<R> ReturnsFuture;
  typedef Future<typename ReturnsFuture::Inner> Return;
  typedef typename ReturnsFuture::Inner RawReturn;
  typedef typename ArgType<Args...>::FirstArg FirstArg;
};

template <typename R, typename... Args>
struct Extract<R (*)(Args...)> {
  typedef isFutureOrSemiFuture<R> ReturnsFuture;
  typedef Future<typename ReturnsFuture::Inner> Return;
  typedef typename ReturnsFuture::Inner RawReturn;
  typedef typename ArgType<Args...>::FirstArg FirstArg;
};

template <typename R, typename... Args>
struct Extract<R (&)(Args...)> {
  typedef isFutureOrSemiFuture<R> ReturnsFuture;
  typedef Future<typename ReturnsFuture::Inner> Return;
  typedef typename ReturnsFuture::Inner RawReturn;
  typedef typename ArgType<Args...>::FirstArg FirstArg;
};

/**
 * Defer work until executor is chained.
 *
 * NOTE: that this executor is a private implementation detail belonging to the
 * Folly Futures library and not intended to be used elsewhere. It is designed
 * specifically for the use case of deferring work on a SemiFuture. It is NOT
 * thread safe. Please do not use for any other purpose without great care.
 */
class DeferredExecutor final : public Executor {
 public:
  template <typename Class, typename F>
  struct DeferredWorkWrapper;

  /**
   * Work wrapper class to capture the keepalive and forward the argument
   * list to the captured function.
   */
  template <typename F, typename R, typename... Args>
  struct DeferredWorkWrapper<F, R (F::*)(Args...) const> {
    R operator()(Args... args) {
      return func(std::forward<Args>(args)...);
    }

    Executor::KeepAlive a;
    F func;
  };

  /**
   * Construction is private to ensure that creation and deletion are
   * symmetric
   */
  static KeepAlive create() {
    std::unique_ptr<futures::detail::DeferredExecutor> devb{
        new futures::detail::DeferredExecutor{}};
    auto keepAlive = devb->getKeepAliveToken();
    devb.release();
    return keepAlive;
  }

  /// Enqueue a function to executed by this executor. This is not thread-safe.
  void add(Func func) override {
    // We should never have a function here already. Either we are RUNNING,
    // in which case we are on one and it should have been removed from the
    // executor, or we are not in which case it should be the first.
    assert(!func_);

    // If we are already running, must be reentrant. Just call func.
    if (state_.load() == State::RUNNING) {
      func();
      return;
    }

    // If we already have a function, wrap and chain. Otherwise assign.
    func_ = std::move(func);

    State expected = State::NEW;
    // If the state is new, then attempt to change it to HAS_CALLBACK, set the
    // executor and return.
    if (state_.load() == expected) {
      if (state_.compare_exchange_strong(expected, State::HAS_CALLBACK)) {
        return;
      }
    }
    // If we have the executor set, we now have the callback too.
    // Enqueue the callback on the executor and change to the RUNNING state.
    enqueueWork();
  }

  void setExecutor(Executor* exec) {
    executorKeepAlive_ = exec->getKeepAliveToken();
    State expected = State::NEW;
    // If the state is new, then attempt to change it to HAS_EXECUTOR, set the
    // executor and return.
    if (state_.load() == expected) {
      if (state_.compare_exchange_strong(expected, State::HAS_EXECUTOR)) {
        return;
      }
    }
    // If we have the callback set, we now have the executor too.
    // Enqueue the callback on the executor and change to the RUNNING state.
    enqueueWork();
  }

  KeepAlive getKeepAliveToken() override {
    keepAliveAcquire();
    return makeKeepAlive();
  }

  ~DeferredExecutor() = default;

  template <class F>
  static auto wrap(Executor::KeepAlive keepAlive, F&& func)
      -> DeferredWorkWrapper<F, decltype(&F::operator())> {
    return DeferredExecutor::DeferredWorkWrapper<F, decltype(&F::operator())>{
        std::move(keepAlive), std::forward<F>(func)};
  }

 protected:
  void keepAliveAcquire() override {
    ++keepAliveCount_;
  }

  void keepAliveRelease() override {
    releaseAndTryFree();
  }

  void releaseAndTryFree() {
    --keepAliveCount_;
    if (keepAliveCount_ == 0) {
      delete this;
    }
  }

 private:
  enum class State {
    NEW,
    HAS_EXECUTOR,
    HAS_CALLBACK,
    RUNNING,
  };

  Func func_;
  ssize_t keepAliveCount_{0};
  std::atomic<State> state_{State::NEW};
  KeepAlive executorKeepAlive_;

  DeferredExecutor() = default;

  void enqueueWork() {
    DCHECK(func_);
    state_.store(State::RUNNING);
    executorKeepAlive_.get()->add(std::move(func_));
    return;
  }
};

} // namespace detail
} // namespace futures

class Timekeeper;

} // namespace folly
