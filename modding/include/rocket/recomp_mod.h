#ifndef ROCKET_RECOMP_MOD_H
#define ROCKET_RECOMP_MOD_H
/* Raw game hooks/replacements need activation "restart". The loader rejects
 * protected port functions. Use managed SDK callbacks where available. */
#define ROCKET_HOOK(function)                                                  \
  __attribute__((used, retain, section(".recomp_hook." function)))
#define ROCKET_HOOK_RETURN(function)                                           \
  __attribute__((used, retain, section(".recomp_hook_return." function)))
#define ROCKET_EXPORT __attribute__((used, retain, section(".recomp_export")))
#define ROCKET_PATCH __attribute__((used, retain, section(".recomp_patch")))
#endif
