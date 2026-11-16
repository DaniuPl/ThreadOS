/* Power, through the firmware's ACPI
 *
 * kernel/acpi.c reads the ACPI tables by hand for the few things the
 * start needs (the cores, switching off, rebooting). Everything else a
 * laptop's firmware knows is in AML - bytecode in the DSDT and SSDTs:
 * what the power button does, where the battery is and how full, the
 * lid. ACPICA (third_party/, the interpreter Linux and the BSDs run)
 * runs it here, on the services in kernel/acpios.c.
 *
 * It starts in a thread of its own, after everything else: firmware is
 * written for other systems, and a method that waits forever on a real
 * machine is to cost this machine its battery reading, not its start.
 *
 *   the power button (ACPI's fixed event): the desktop asks whether to
 *   shut down; with no desktop the machine switches off, as a power
 *   button does
 *
 *   a battery (a device named PNP0C0A): its static description (_BIX,
 *   or the older _BIF) and its state (_BST) read every few seconds and
 *   whenever the firmware notifies that it changed
 */

#include "acpi.h"                 /* ACPICA's */
#include "include/power.h"
#include "include/acpi.h"
#include "include/desktop.h"
#include "include/printk.h"
#include "include/task.h"
#include "include/pit.h"

void acpios_worker(void *arg);
void acpios_calibrate(void);
uint32_t acpios_sci_count(void);

static volatile int ready;               /* 1 running, -1 failed */
static const char *failed_at = "not started";
static volatile int button_pressed;
static uint32_t button_count;
static uint32_t object_count;

static ACPI_HANDLE battery;
static volatile int battery_dirty;
static struct power_battery bat;

int power_ready(void)
{
    return ready;
}

/* In the SCI's interrupt: only a flag - switching off or asking takes
   the power thread */
static UINT32 on_power_button(void *context)
{
    (void)context;
    button_pressed = 1;
    return ACPI_INTERRUPT_HANDLED;
}

static void on_notify(ACPI_HANDLE device, UINT32 value, void *context)
{
    (void)device;
    (void)value;
    (void)context;
    battery_dirty = 1;            /* 0x80 status, 0x81 information */
}

static ACPI_STATUS found_battery(ACPI_HANDLE obj, UINT32 depth,
                                 void *context, void **ret)
{
    (void)depth;
    (void)context;
    (void)ret;
    if (!battery)
        battery = obj;
    return AE_OK;
}

static ACPI_STATUS count_object(ACPI_HANDLE obj, UINT32 depth,
                                void *context, void **ret)
{
    (void)obj;
    (void)depth;
    (void)context;
    (void)ret;
    object_count++;
    return AE_OK;
}

/* The integer at 'i' in a package, or 'dflt' */
static uint32_t pkg_int(ACPI_OBJECT *pkg, uint32_t i, uint32_t dflt)
{
    if (!pkg || pkg->Type != ACPI_TYPE_PACKAGE || i >= pkg->Package.Count ||
        pkg->Package.Elements[i].Type != ACPI_TYPE_INTEGER)
        return dflt;
    return (uint32_t)pkg->Package.Elements[i].Integer.Value;
}

static ACPI_OBJECT *evaluate(ACPI_HANDLE dev, const char *method,
                             ACPI_BUFFER *buf)
{
    buf->Length = ACPI_ALLOCATE_BUFFER;
    buf->Pointer = NULL;
    if (ACPI_FAILURE(AcpiEvaluateObject(dev, (ACPI_STRING)method, NULL, buf)))
        return NULL;
    return buf->Pointer;
}

/* The battery read anew. ACPI's capacities: 0xFFFFFFFF is "unknown",
   and a last full charge that is unknown falls back on the design */
static void read_battery(void)
{
    struct power_battery b = { 0 };
    UINT64 sta = 0x1F;
    if (!battery)
        goto done;
    ACPI_BUFFER sbuf;
    {
        ACPI_OBJECT out;
        ACPI_BUFFER ob = { sizeof(out), &out };
        if (ACPI_SUCCESS(AcpiEvaluateObjectTyped(battery, "_STA", NULL, &ob,
                                                 ACPI_TYPE_INTEGER)))
            sta = out.Integer.Value;
    }
    if (!(sta & 0x10))            /* the battery is not in its bay */
        goto done;

    ACPI_BUFFER ibuf;
    uint32_t unit, design, full;
    ACPI_OBJECT *info = evaluate(battery, "_BIX", &ibuf);
    if (info) {                   /* _BIX: a revision first */
        unit = pkg_int(info, 1, 0);
        design = pkg_int(info, 2, 0xFFFFFFFF);
        full = pkg_int(info, 3, 0xFFFFFFFF);
    } else {
        info = evaluate(battery, "_BIF", &ibuf);
        unit = pkg_int(info, 0, 0);
        design = pkg_int(info, 1, 0xFFFFFFFF);
        full = pkg_int(info, 2, 0xFFFFFFFF);
    }
    if (ibuf.Pointer)
        AcpiOsFree(ibuf.Pointer);
    if (full == 0 || full == 0xFFFFFFFF)
        full = design;

    ACPI_OBJECT *st = evaluate(battery, "_BST", &sbuf);
    if (st) {
        uint32_t state = pkg_int(st, 0, 0);
        uint32_t left = pkg_int(st, 2, 0xFFFFFFFF);
        b.present = 1;
        b.discharging = (state & 1) != 0;
        b.charging = (state & 2) != 0;
        b.critical = (state & 4) != 0;
        b.unit = unit ? "mAh" : "mWh";
        b.full = full;
        b.remaining = left;
        if (left != 0xFFFFFFFF && full && full != 0xFFFFFFFF) {
            uint64_t p = (uint64_t)left * 100 / full;
            b.percent = p > 100 ? 100 : (int)p;
        } else {
            b.percent = -1;
        }
        AcpiOsFree(sbuf.Pointer);
    }
done:
    bat = b;
}

int power_battery(struct power_battery *out)
{
    *out = bat;
    return bat.present;
}

static void power_thread(void *arg)
{
    (void)arg;
    acpios_calibrate();
    task_create("acpiwork", acpios_worker, 0);

    ACPI_STATUS s;
    if (ACPI_FAILURE(s = AcpiInitializeSubsystem()))
        failed_at = "the subsystem";
    else if (ACPI_FAILURE(s = AcpiInitializeTables(NULL, 16, FALSE)))
        failed_at = "the tables";
    else if (ACPI_FAILURE(s = AcpiLoadTables()))
        failed_at = "loading the AML";
    else if (ACPI_FAILURE(s = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION)))
        failed_at = "enabling ACPI";
    else if (ACPI_FAILURE(s = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION)))
        failed_at = "initialising the devices";
    if (ACPI_FAILURE(s)) {
        printk("[ !! ] ACPI: ACPICA stopped at %s (%s)\n", failed_at,
               AcpiFormatException(s));
        ready = -1;
        return;
    }

    AcpiWalkNamespace(ACPI_TYPE_ANY, ACPI_ROOT_OBJECT, ACPI_UINT32_MAX,
                      count_object, NULL, NULL, NULL);
    AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON, on_power_button,
                                 NULL);
    AcpiGetDevices("PNP0C0A", found_battery, NULL, NULL);
    if (battery)
        AcpiInstallNotifyHandler(battery, ACPI_DEVICE_NOTIFY, on_notify, NULL);
    read_battery();
    failed_at = "";
    ready = 1;
    printk("[ OK ] ACPI: the firmware's AML runs (ACPICA %x), %u objects; "
           "power button%s\n", (unsigned)ACPI_CA_VERSION, object_count,
           bat.present ? ", a battery" : battery ? ", a battery bay (empty)"
                                                 : "");

    uint32_t last = pit_ticks();
    for (;;) {
        if (button_pressed) {
            button_pressed = 0;
            button_count++;
            printk("[acpi] the power button\n");
            if (desktop_power_button() < 0) {
                printk("[acpi] no desktop to ask: switching off\n");
                acpi_poweroff();
            }
        }
        /* The level changes slowly, but the firmware need not say so -
           most laptops' do not notify for every percent */
        if (battery_dirty || pit_ticks() - last >= 500) {
            battery_dirty = 0;
            last = pit_ticks();
            read_battery();
        }
        task_sleep(50);
    }
}

void power_start(void)
{
    task_create("acpi", power_thread, 0);
}

void power_report(void)
{
    if (ready <= 0) {
        printk("acpi: %s\n", ready < 0 ? "ACPICA stopped - see the start"
                                        : "ACPICA has not started (yet)");
        return;
    }
    printk("acpi: ACPICA %x, %u objects in the namespace, %u SCIs, "
           "the power button pressed %u time(s)\n",
           (unsigned)ACPI_CA_VERSION, object_count, acpios_sci_count(),
           button_count);
    struct power_battery b;
    if (!power_battery(&b)) {
        printk("battery: none\n");
        return;
    }
    if (b.percent < 0)
        printk("battery: level unknown");
    else
        printk("battery: %d%%", b.percent);
    printk(", %s (%u of %u %s)\n",
           b.charging ? "charging" : b.discharging ? "discharging"
                                                   : "not charging",
           b.remaining, b.full, b.unit);
}
