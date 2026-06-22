#pragma once
#include <functional>

// ScopeGuard: runs func() at scope exit (RAII defer idiom).
template <typename F> class ScopeGuard {
  F func;

public:
  explicit ScopeGuard(F &&f) : func(std::forward<F>(f)) {}
  ~ScopeGuard() { func(); }
};

#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)

// defer { ... }; runs the block when the enclosing scope exits.
#define defer const auto CONCAT(defer_func_, __LINE__) = ScopeGuard([&]()
