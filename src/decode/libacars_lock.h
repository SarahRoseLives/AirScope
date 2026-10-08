// Process-wide lock serializing all libacars calls. libacars uses global state
// (and reassembly contexts), so the AM ACARS decoders, the application-layer
// decoders and the VDL2 channel workers must not call it concurrently.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void airscope_libacars_lock(void);
void airscope_libacars_unlock(void);

#ifdef __cplusplus
}
#endif
