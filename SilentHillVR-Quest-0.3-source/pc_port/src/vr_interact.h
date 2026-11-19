#ifndef VR_INTERACT_H
#define VR_INTERACT_H

/* VR hands: grab / radial wheel / weapon priorities (vr_interact.c). */

#define VR_WHEEL_MAX 14
#define VR_WHEEL_ID_MAP 0xFE   /* the wheel's Map entry (not an inventory item) */

typedef struct
{
    int           open;
    int           n;                     /* entries */
    int           sel;                   /* -1 = nothing selected */
    unsigned char ids[VR_WHEEL_MAX];     /* healing item ids or VR_WHEEL_ID_MAP, slot 0 at the top, clockwise */
    int           counts[VR_WHEEL_MAX];  /* stock, -1 = none to show */
} VrWheelView;

int  VrInteract_GetWheel(VrWheelView* out);
void VrInteract_GameUpdate(void);
int  VrInteract_HolsterState(void);   /* -1 none, 0 drawn, 1 holstered */

#endif
