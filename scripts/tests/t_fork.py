#!/usr/bin/env python3
"""Test dymny ProtonOS: fork, exec i drzewo procesow.

Uniksowa para fork/exec jest najciezsza czescia zarzadzania procesami,
bo dotyka wszystkiego naraz: przestrzeni adresowej, kopiowania przy
zapisie, tablicy deskryptorow, sygnalow i schedulera.

Dwie asercje sa tu wazniejsze od pozostalych. Pierwsza: fork ma NIE
kopiowac pamieci - program zajmuje 256 KiB, a przyrost RAM tuz po
rozwidleniu ma byc rzedu tablic stron, nie bufora. Druga: wzorzec
rodzica ma zostac nietkniety takze tam, gdzie w dziecku pisalo JADRO,
czyli w buforze wypelnianym przez wywolanie systemowe - o to dba bit WP
w rejestrze CR0 i latwo go przeoczyc.

Uruchamiany osobno albo przez scripts/run_tests.py, ktory puszcza
wszystkie grupy rownolegle.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, pmm_used


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_for("ProtonOS shell")
        q.wait_for("[siec] DHCP", timeout=20)
        time.sleep(0.5)

        # 'run forktest': fork() z kopiowaniem przy zapisie. Program
        # zajmuje 256 KiB, rozwidla sie i sprawdza, ze dziecko dostalo
        # wzorzec rodzica, a wlasne zapisy zostaly u niego.
        q.type_line("clear")
        q.type_line("mem")
        used_before_fk = pmm_used(q.screen())
        q.type_line("run forktest")
        screen = q.wait_for("[forktest] dziecko: wzorzec odziedziczony",
                            timeout=30)
        check("fork: dziecko wraca z syscalla z pamiecia rodzica",
              "[forktest] dziecko: wzorzec odziedziczony" in screen)
        m = re.search(r"przyrost RAM po fork: (\d+) KiB", screen)
        check("fork nie kopiuje pamieci procesu"
              + (f" (256 KiB bufora, przyrost {m.group(1)} KiB)" if m else ""),
              m is not None and int(m.group(1)) < 64)
        screen = q.wait_for("[forktest] rodzic: wzorzec nietkniety",
                            timeout=30)
        check("zapisy dziecka nie widac u rodzica (prywatne kopie stron)",
              "[forktest] rodzic: wzorzec nietkniety" in screen)
        check("zapis jadra w dziecku tez trafil w prywatna kopie strony",
              "rodzic: wzorzec nietkniety" in screen)
        check("rodzic odbiera kod wyjscia dziecka przez wait",
              "kod dziecka=7" in screen)

        # Druga polowa pary uniksowej: dziecko podmienia sie w inny
        # program przez exec, a rodzic czeka na ten sam pid.
        screen = q.wait_for("[forktest] rodzic doczekal sie pidu", timeout=30)
        check("exec podmienia obraz procesu w miejscu",
              "[forktest] po exec: inny program, ten sam pid" in screen)
        check("po exec proces nazywa sie jak nowy program",
              "proces 'echo' zakonczyl sie (kod 0)" in screen)
        check("wait rodzica trafia w ten sam pid po exec",
              "doczekal sie pidu" in screen)
        q.wait_for("proces 'forktest' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_fk = pmm_used(q.screen())
        check(f"brak wycieku ramek po fork"
              f" ({used_before_fk} -> {used_after_fk})",
              used_after_fk is not None and used_after_fk == used_before_fk)

        # 'run ptree': drzewo procesow. Trzy dzieci koncza sie w innej
        # kolejnosci, niz powstaly, a rodzic zbiera je przez waitany.
        q.type_line("clear")
        q.type_line("mem")
        used_before_pt = pmm_used(q.screen())
        q.type_line("run ptree")
        screen = q.wait_for("[ptree] zebrane wszystkie dzieci", timeout=30)
        m = re.findall(r"skonczylo dziecko (\d+) z kodem (\d+)", screen)
        check("wait na dowolne dziecko oddaje je w kolejnosci konczenia"
              + (f" (pidy {[p for p, _ in m]})" if m else ""),
              len(m) == 3 and [int(p) for p, _ in m] == sorted(
                  (int(p) for p, _ in m), reverse=True))
        check("kod wyjscia pasuje do dziecka, ktore go zwrocilo",
              [c for _, c in m] == ["22", "21", "20"])
        screen = q.wait_for("[ptree] sierota przygarnieta", timeout=30)
        check("sierota trafia pod opieke dziadka (kod 99 dociera do niego)",
              "z kodem 99" in screen)
        q.wait_for("proces 'ptree' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_pt = pmm_used(q.screen())
        check(f"brak wycieku ramek po drzewie procesow"
              f" ({used_before_pt} -> {used_after_pt})",
              used_after_pt is not None and used_after_pt == used_before_pt)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
