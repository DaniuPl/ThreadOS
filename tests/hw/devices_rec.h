/* The Device Manager's registry (kernel/devices.c) for a model on the
   host: what a driver said about its device, kept to be checked - the
   driver claimed it, said it is up, gave a reason, or left a function
   to be asked. One device: each model has the one it models. */
#ifndef HW_DEVICES_REC_H
#define HW_DEVICES_REC_H

struct dev_record {
    int claims;                 /* how many times device_claim() came */
    int bus, slot, func;        /* of the last claim */
    char driver[16];
    int up;                     /* device_up() came after it */
    int state;                  /* the last device_trouble()'s, or -1 */
    char why[64];
    int (*live)(char *why, int n);
};
extern struct dev_record dev_rec;

#endif
