/* forktest - rozwidlenie procesu (fork) z kopiowaniem przy zapisie
 *
 * Program zajmuje 256 KiB sterty, wypelnia ja wzorcem i rozwidla sie.
 * Dziecko wraca z sys_fork() z zerem, rodzic z pidem dziecka - obaj
 * w tym samym miejscu kodu, z ta sama pamiecia.
 *
 * Pamiec nie jest jednak kopiowana: obie przestrzenie adresowe wskazuja
 * na te same ramki, tyle ze bez prawa zapisu. Widac to po zajetosci RAM
 * tuz po fork - rosnie o kilka KiB (tablice stron), a nie o 256. Dopiero
 * gdy dziecko nadpisuje wzorzec, jadro robi mu prywatne kopie stron;
 * rodzic widzi swoj wzorzec nietkniety.
 */

#include "usys.h"

#define BUF_KIB 256
#define BUF     (BUF_KIB * 1024)
#define STEP    4096              /* jeden bajt na strone wystarczy */

static unsigned char pattern(uint32_t i) { return (unsigned char)(0xA5 ^ (i >> 12)); }

static int check(unsigned char *buf, int mine)
{
    for (uint32_t i = 0; i < BUF; i += STEP) {
        unsigned char want = mine ? pattern(i) : (unsigned char)0x5A;
        if (buf[i] != want)
            return 0;
    }
    return 1;
}

void _start(void)
{
    unsigned char *buf = sys_sbrk(BUF);
    if (buf == (void *)-1) {
        print("[forktest] brak pamieci na bufor\n");
        sys_exit(1);
    }
    for (uint32_t i = 0; i < BUF; i += STEP)
        buf[i] = pattern(i);

    struct sysinfo si;
    sys_sysinfo(&si);
    uint32_t before = si.used_kib;

    int32_t pid = sys_fork();
    if (pid < 0) {
        print("[forktest] fork sie nie udal\n");
        sys_exit(1);
    }

    if (pid == 0) {
        /* Dziecko: daj rodzicowi zmierzyc pamiec, zanim zaczniesz pisac */
        sys_sleep(400);
        if (!check(buf, 1)) {
            print("[forktest] dziecko: wzorzec rodzica NIE dotarl\n");
            sys_exit(1);
        }
        /* Zapis JADRA do strony, ktora wciaz czeka na skopiowanie:
           syscall wypelnia bufor w srodku sterty. Bez bitu WP w CR0
           jadro pisaloby prosto w ramke wspoldzielona z rodzicem
           i po cichu zepsulo mu pamiec. */
        char *slot = (char *)(buf + BUF / 2);
        sys_getcwd(slot);
        if (slot[0] != '/') {
            print("[forktest] dziecko: syscall nie zapisal bufora\n");
            sys_exit(1);
        }
        print("[forktest] dziecko: wzorzec odziedziczony, nadpisuje\n");
        for (uint32_t i = 0; i < BUF; i += STEP)
            buf[i] = 0x5A;
        if (!check(buf, 0)) {
            print("[forktest] dziecko: wlasne zapisy nie doszly\n");
            sys_exit(1);
        }
        sys_exit(7);
    }

    /* Rodzic */
    sys_sysinfo(&si);
    uint32_t grew = si.used_kib - before;
    print("[forktest] bufor ");
    print_dec(BUF_KIB);
    print(" KiB, przyrost RAM po fork: ");
    print_dec(grew);
    print(" KiB\n");
    if (grew >= BUF_KIB) {
        print("[forktest] pamiec zostala skopiowana - to nie jest COW\n");
        sys_exit(1);
    }
    print("[forktest] kopiowanie przy zapisie dziala\n");

    int32_t code = sys_wait(pid);
    if (!check(buf, 1)) {
        print("[forktest] rodzic: dziecko nadpisalo MOJ wzorzec\n");
        sys_exit(1);
    }
    print("[forktest] rodzic: wzorzec nietkniety, kod dziecka=");
    print_dec((uint32_t)code);
    print("\n");

    /* Klasyczna para uniksowa: rozwidl sie, a w dziecku wskocz w inny
       program. Pid, deskryptory i katalog roboczy zostaja - wymienia
       sie tylko obraz procesu, wiec rodzic czeka na ten sam pid. */
    int32_t pid2 = sys_fork();
    if (pid2 < 0) {
        print("[forktest] drugi fork sie nie udal\n");
        sys_exit(1);
    }
    if (pid2 == 0) {
        sys_exec("echo [forktest] po exec: inny program, ten sam pid");
        print("[forktest] exec sie nie udal\n");    /* tu nie powinno dojsc */
        sys_exit(1);
    }
    sys_wait(pid2);
    print("[forktest] rodzic doczekal sie pidu ");
    print_dec((uint32_t)pid2);
    print(" po exec\n");
    sys_exit(0);
}
