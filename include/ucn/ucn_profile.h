#ifndef UCN_PROFILE_H
#define UCN_PROFILE_H

/* Public compile-time feature contract.
 *
 * Every translation unit that includes a public UCN header must see the same
 * UCN_PROFILE and UCN_FEATURE_SERVICE values because they change public object
 * layout.  CMake exports both definitions from the ucn_core target; products
 * that build the C sources directly must provide them globally.
 */
#include "ucn/ucn_config.h"

#ifndef UCN_PROFILE_NANO
#define UCN_PROFILE_NANO 1
#endif
#ifndef UCN_PROFILE_LITE
#define UCN_PROFILE_LITE 2
#endif
#ifndef UCN_PROFILE_FULL
#define UCN_PROFILE_FULL 3
#endif

#ifndef UCN_PROFILE
#define UCN_PROFILE UCN_PROFILE_FULL
#endif

#if UCN_PROFILE != UCN_PROFILE_NANO && \
    UCN_PROFILE != UCN_PROFILE_LITE && \
    UCN_PROFILE != UCN_PROFILE_FULL
#error "UCN_PROFILE must be UCN_PROFILE_NANO, UCN_PROFILE_LITE, or UCN_PROFILE_FULL"
#endif

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_PROFILE_NAME "Nano"
#define UCN_FEATURE_DYNAMIC_MESH 0
#define UCN_FEATURE_SECURITY 0
#define UCN_FEATURE_CANDIDATE_ROUTING 0
#define UCN_FEATURE_PATH 0
#define UCN_FEATURE_POLICY 0
#define UCN_FEATURE_DIAGNOSTICS 0
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_PROFILE_NAME "Lite"
#define UCN_FEATURE_DYNAMIC_MESH 1
#define UCN_FEATURE_SECURITY 1
#define UCN_FEATURE_CANDIDATE_ROUTING 0
#define UCN_FEATURE_PATH 0
#define UCN_FEATURE_POLICY 0
#define UCN_FEATURE_DIAGNOSTICS 0
#else
#define UCN_PROFILE_NAME "Full"
#define UCN_FEATURE_DYNAMIC_MESH 1
#define UCN_FEATURE_SECURITY 1
#define UCN_FEATURE_CANDIDATE_ROUTING 1
#define UCN_FEATURE_PATH 1
#define UCN_FEATURE_POLICY 1
#define UCN_FEATURE_DIAGNOSTICS 1
#endif

/* Service Router/Bridge is orthogonal to the network Profile.  It stays ON by
 * default for source compatibility and can be excluded independently. */
#ifndef UCN_FEATURE_SERVICE
#define UCN_FEATURE_SERVICE 1
#endif

#if UCN_FEATURE_SERVICE != 0 && UCN_FEATURE_SERVICE != 1
#error "UCN_FEATURE_SERVICE must be 0 or 1"
#endif

/* Freeze the dependency graph in the public header so invalid custom build
 * definitions fail before object layouts diverge. */
#if UCN_FEATURE_CANDIDATE_ROUTING && !UCN_FEATURE_DYNAMIC_MESH
#error "Candidate routing requires dynamic mesh"
#endif
#if UCN_FEATURE_PATH && !UCN_FEATURE_DYNAMIC_MESH
#error "Path requires dynamic mesh"
#endif
#if UCN_FEATURE_POLICY && !UCN_FEATURE_PATH
#error "Policy requires Path"
#endif
#if UCN_FEATURE_DIAGNOSTICS && (!UCN_FEATURE_DYNAMIC_MESH || !UCN_FEATURE_POLICY)
#error "Diagnostics requires dynamic mesh and Policy"
#endif

#endif
