// Rocket-R v29 render recovery compatibility shim.
//
// widescreen_culling.cpp still calls this legacy diagnostic callback.  The
// experimental v28 queue used to own the implementation.  v29 deliberately
// restores Rocket's original retail RenderEntry/sort/draw path, so keep only a
// harmless definition to satisfy the linker without reintroducing v28.
extern "C" void rocket_popdiag_frustum_call(void) {
}
