#pragma once

#ifndef GATHRA_GIT_COMMIT
#define GATHRA_GIT_COMMIT "unknown"
#endif

#ifndef GATHRA_BUILD_FLAVOR
#ifdef GATHRA_DEFAULT_BUILD_FLAVOR
#define GATHRA_BUILD_FLAVOR GATHRA_DEFAULT_BUILD_FLAVOR
#else
#define GATHRA_BUILD_FLAVOR "unknown"
#endif
#endif

namespace gathra::gateway::firmware {
inline constexpr char kVersion[] = "2.1.0";
inline constexpr char kGitCommit[] = GATHRA_GIT_COMMIT;
inline constexpr char kBuildFlavor[] = GATHRA_BUILD_FLAVOR;
inline constexpr char kBuildDate[] = __DATE__ " " __TIME__;
}  // namespace gathra::gateway::firmware
