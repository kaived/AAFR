#pragma once

#include <string>

namespace ngr {

enum class ExecutionSupportLevel {
  ObserveOnly,
  InterceptorControlled,
  ProfileControlled,
  CustomSchemeControlled,
  ProxyRequired,
  Planned
};

enum class RequestActionKind {
  Observe,
  Allow,
  Block,
  Redirect,
  SetHeader,
  UseCacheFirst,
  UseNetworkFirst,
  RetryWithBackoff,
  DeferLowPriority,
  PrefetchWhenIdle
};

struct RequestAction {
  RequestActionKind kind{RequestActionKind::Observe};
  ExecutionSupportLevel support_level{ExecutionSupportLevel::ObserveOnly};
  std::string reason;
  std::string target;
};

inline const char* ToString(ExecutionSupportLevel value) {
  switch (value) {
    case ExecutionSupportLevel::ObserveOnly:
      return "OBSERVE_ONLY";
    case ExecutionSupportLevel::InterceptorControlled:
      return "INTERCEPTOR_CONTROLLED";
    case ExecutionSupportLevel::ProfileControlled:
      return "PROFILE_CONTROLLED";
    case ExecutionSupportLevel::CustomSchemeControlled:
      return "CUSTOM_SCHEME_CONTROLLED";
    case ExecutionSupportLevel::ProxyRequired:
      return "PROXY_REQUIRED";
    case ExecutionSupportLevel::Planned:
      return "PLANNED";
  }
  return "UNKNOWN";
}

inline const char* ToString(RequestActionKind value) {
  switch (value) {
    case RequestActionKind::Observe:
      return "OBSERVE";
    case RequestActionKind::Allow:
      return "ALLOW";
    case RequestActionKind::Block:
      return "BLOCK";
    case RequestActionKind::Redirect:
      return "REDIRECT";
    case RequestActionKind::SetHeader:
      return "SET_HEADER";
    case RequestActionKind::UseCacheFirst:
      return "USE_CACHE_FIRST";
    case RequestActionKind::UseNetworkFirst:
      return "USE_NETWORK_FIRST";
    case RequestActionKind::RetryWithBackoff:
      return "RETRY_WITH_BACKOFF";
    case RequestActionKind::DeferLowPriority:
      return "DEFER_LOW_PRIORITY";
    case RequestActionKind::PrefetchWhenIdle:
      return "PREFETCH_WHEN_IDLE";
  }
  return "UNKNOWN";
}

}  // namespace ngr
