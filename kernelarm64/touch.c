/* The touchscreen: virtio-input
 *
 * A phone's screen is a pointer that is only there while a finger is.
 * In QEMU it is a virtio-input device - virtio-tablet-device (one
 * finger, as a pen) or virtio-multitouch-device - which sends Linux's
 * input events, eight bytes each: absolute X and Y in the device's own
 * range, a button or BTN_TOUCH going down and up, and a report that
 * closes each set (SYN_REPORT). The configuration space says what the
 * device is: its name, which events it sends, and each axis's range.
 *
 * Here a report becomes a touch: down when the finger arrives, moves
 * while it is there, up when it leaves, in the screen's logical pixels.
 * A multitouch panel's first finger (slot 0) is the one followed.
 */

#include <stdint.h>
#include "touch.h"
#include "../kernel/include/string.h"
#include "../kernel/include/printk.h"

#define VIRTIO_ID_INPUT 18

/* The configuration space */
#define CFG_SELECT   0x00
#define CFG_SUBSEL   0x01
#define CFG_SIZE     0x02
#define CFG_DATA     0x08
#define SEL_NAME     0x01
#define SEL_EV_BITS  0x11
#define SEL_ABS_INFO 0x12

/* Linux's input events */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_ABS 0x03
#define SYN_REPORT 0
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_MT_SLOT 0x2f
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39
#define BTN_LEFT 0x110
#define BTN_TOUCH 0x14a

#define EVENTS 32
#define QUEUED 64

struct vin_event {
    uint16_t type, code;
    uint32_t value;
};

static struct virtio_dev *dev;
static struct virtq eventq;
static struct vin_event bufs[EVENTS];
static char name[64];
static int multitouch;
static uint32_t min_x, max_x, min_y, max_y;
static int scr_w, scr_h;

void touch_screen(int w, int h)
{
    scr_w = w;
    scr_h = h;
}

/* What the finger is doing, as the reports so far have it */
static int raw_x, raw_y, down, was_down, slot;

static struct touch_event queue[QUEUED];
static unsigned q_head, q_tail;

static uint8_t cfg_query(uint8_t sel, uint8_t subsel)
{
    virtio_cfg_write8(dev, CFG_SELECT, sel);
    virtio_cfg_write8(dev, CFG_SUBSEL, subsel);
    return virtio_cfg8(dev, CFG_SIZE);
}

/* Whether the device sends an event of this type and code */
static int has_event(uint8_t type, unsigned code)
{
    uint8_t size = cfg_query(SEL_EV_BITS, type);
    if (code / 8 >= size)
        return 0;
    return (virtio_cfg8(dev, CFG_DATA + code / 8) >> (code % 8)) & 1;
}

static void axis(unsigned code, uint32_t *lo, uint32_t *hi)
{
    if (cfg_query(SEL_ABS_INFO, (uint8_t)code) >= 8) {
        *lo = virtio_cfg32(dev, CFG_DATA);
        *hi = virtio_cfg32(dev, CFG_DATA + 4);
    }
    if (*hi <= *lo)
        *hi = *lo + 1;
}

const char *touch_name(void)
{
    return name;
}

int touch_init(struct virtio_dev *devs, int n, int w, int h)
{
    for (int i = 0; i < n && !dev; i++) {
        if (devs[i].device_id != VIRTIO_ID_INPUT)
            continue;
        dev = &devs[i];
        if (has_event(EV_ABS, ABS_MT_POSITION_X) &&
            has_event(EV_ABS, ABS_MT_POSITION_Y))
            multitouch = 1;
        else if (!(has_event(EV_ABS, ABS_X) && has_event(EV_ABS, ABS_Y)))
            dev = 0;                     /* a keyboard, a relative mouse */
    }
    if (!dev)
        return -1;

    uint8_t len = cfg_query(SEL_NAME, 0);
    if (len >= sizeof(name))
        len = sizeof(name) - 1;
    for (uint8_t i = 0; i < len; i++)
        name[i] = (char)virtio_cfg8(dev, CFG_DATA + i);
    name[len] = 0;
    axis(multitouch ? ABS_MT_POSITION_X : ABS_X, &min_x, &max_x);
    axis(multitouch ? ABS_MT_POSITION_Y : ABS_Y, &min_y, &max_y);
    scr_w = w;
    scr_h = h;

    if (virtio_begin(dev) < 0 || virtio_queue(dev, 0, &eventq, EVENTS) < 0)
        return -1;
    virtio_ready(dev);
    for (unsigned i = 0; i < eventq.size && i < EVENTS; i++)
        virtio_give(dev, 0, &eventq, i, &bufs[i], sizeof(bufs[i]));
    virtio_notify(dev, 0);
    printk("Touch: %s, X %u..%u, Y %u..%u%s\n", name, min_x, max_x, min_y,
           max_y, multitouch ? ", multitouch" : "");
    return 0;
}

static void push(int kind)
{
    struct touch_event *e = &queue[q_head % QUEUED];
    e->kind = kind;
    /* The device's range onto the screen, to the nearest pixel: its last
       value is the last pixel, as a panel's is */
    uint32_t rx = max_x - min_x, ry = max_y - min_y;
    /* (a value outside the range it said is the device's word, kept in) */
    if (raw_x < (int)min_x)
        raw_x = (int)min_x;
    if (raw_x > (int)max_x)
        raw_x = (int)max_x;
    if (raw_y < (int)min_y)
        raw_y = (int)min_y;
    if (raw_y > (int)max_y)
        raw_y = (int)max_y;
    e->x = (int)(((uint64_t)(raw_x - (int)min_x) * (uint64_t)(scr_w - 1) +
                  rx / 2) / rx);
    e->y = (int)(((uint64_t)(raw_y - (int)min_y) * (uint64_t)(scr_h - 1) +
                  ry / 2) / ry);
    q_head++;
    if (q_head - q_tail > QUEUED)
        q_tail = q_head - QUEUED;       /* the oldest are let go */
}

static void event(const struct vin_event *e)
{
    if (e->type == EV_ABS) {
        if (e->code == ABS_MT_SLOT)
            slot = (int)e->value;
        else if (slot)
            return;                      /* another finger */
        else if (e->code == ABS_X || e->code == ABS_MT_POSITION_X)
            raw_x = (int)e->value;
        else if (e->code == ABS_Y || e->code == ABS_MT_POSITION_Y)
            raw_y = (int)e->value;
        else if (e->code == ABS_MT_TRACKING_ID)
            down = e->value != 0xFFFFFFFFu;
    } else if (e->type == EV_KEY &&
               (e->code == BTN_LEFT || e->code == BTN_TOUCH)) {
        if (!multitouch || e->code == BTN_TOUCH)
            down = e->value != 0;
    } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
        if (down && !was_down)
            push(TOUCH_DOWN);
        else if (down)
            push(TOUCH_MOVE);
        else if (was_down)
            push(TOUCH_UP);
        was_down = down;
    }
}

int touch_next(struct touch_event *e)
{
    if (dev) {
        int id, given = 0;
        while ((id = virtio_take(&eventq, 0)) >= 0) {
            if (id < EVENTS) {
                event(&bufs[id]);
                virtio_give(dev, 0, &eventq, (unsigned)id, &bufs[id],
                            sizeof(bufs[id]));
                given = 1;
            }
        }
        if (given)
            virtio_notify(dev, 0);
    }
    if (q_tail == q_head)
        return 0;
    *e = queue[q_tail % QUEUED];
    q_tail++;
    return 1;
}
