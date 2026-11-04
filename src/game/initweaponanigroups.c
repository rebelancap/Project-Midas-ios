#include <ultra64.h>
#include "model.h"

//uncomment when actor is worked on
//#include "chr.h"
#ifdef PORT
/* D-219: the real prototype (chr.c), not an `extern s32` object whose
 * address happened to be the function's. */
struct Vertex *get_ptr_allocated_block_for_vertices(s32 numvertices);
#else
extern s32 get_ptr_allocated_block_for_vertices;
#endif

void init_weapon_animation_groups_maybe(void) {
#ifdef PORT
    set_vtxallocator(get_ptr_allocated_block_for_vertices);
#else
    set_vtxallocator(&get_ptr_allocated_block_for_vertices);
#endif
    initWeaponAnimGroups();
}
