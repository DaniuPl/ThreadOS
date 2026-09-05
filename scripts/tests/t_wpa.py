"""ProtonOS smoke test: the cryptography of WPA2 (IEEE 802.11i).

There is no radio in this system, and QEMU has no wireless card to
emulate - so this is where the honest line runs: everything a station
does with a password, a handshake and a frame can be written and
checked now; only the driver of a particular chip cannot.

And it is checked the same way as the rest: against something that is
not our code.

  * the frames go against the vectors from P802.11i/D7.0 - real MPDUs
    with headers, packet numbers and signatures. A CCMP that encrypts
    correctly but assembles the associated data wrongly agrees with
    itself and with no access point in the world;
  * the keys go against Python's hashlib: the system derives a PMK, a
    PTK and the signature of a handshake message from a seed, prints
    everything it used, and the host computes the same with its own
    PBKDF2 and HMAC;
  * the frame is then attacked: one changed address, one changed byte
    of the data, a different key - each has to end with the frame being
    dropped.

Run on its own or through scripts/run_tests.py.
"""

import hashlib
import hmac
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, make_iso64


def fields(out):
    got = {}
    for name, value in re.findall(r"^\s*(haslo|siec|pmk|aa|spa|anonce|snonce"
                                  r"|ptk|ramka|mic)=([0-9a-f]+)\s*$",
                                  out, re.M):
        got[name] = bytes.fromhex(value)
    return got


def host_prf(key, label, data, nbytes):
    """The 802.11i PRF, written here out of the standard - Python's
    hmac underneath, so it shares no line with the code being tested."""
    out = b""
    counter = 0
    while len(out) < nbytes:
        out += hmac.new(key, label + b"\x00" + data + bytes([counter]),
                        hashlib.sha1).digest()
        counter += 1
    return out[:nbytes]


def main():
    print()
    print("--- WPA2: klucze i szyfrowanie ramki ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)

    try:
        check("plyta z jadrem 64-bitowym buduje sie",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=90)

            out = q.command("run wpatest", "kod wyjscia", timeout=240)
            check("SHA-1, HMAC-SHA1 i PMK z hasla (802.11i)",
                  "PMK z hasla (802.11i, 4096 rund): OK" in out)
            check("AES key wrap z RFC 3394 (tak jedzie klucz grupowy)",
                  "AES key wrap (RFC 3394 4.1): OK" in out and
                  "jeden zmieniony bit = odmowa odwiniecia: OK" in out)
            check("CCMP szyfruje trzy ramki z P802.11i tak jak norma",
                  out.count("CCMP szyfruje jak w P802.11i: OK") == 3)
            check("i odszyfrowuje je z powrotem",
                  out.count("i odszyfrowuje z powrotem: OK") == 3)
            check("zmiana adresu w naglowku konczy sie odrzuceniem ramki",
                  "zmiana adresu w naglowku = ramka odrzucona: OK" in out)
            check("zmiana bajtu danych konczy sie odrzuceniem ramki",
                  "zmiana bajtu danych = ramka odrzucona: OK" in out)
            check("obcy klucz nie odszyfrowuje niczego",
                  "obcy klucz nie odszyfrowuje: OK" in out)
            check("caly zestaw wektorow przechodzi",
                  "[wpa] wszystko sie zgadza" in out and "BLAD" not in out)

            # --- The same keys, computed by Python -------------------
            for seed in ("5a", "c3"):
                out = q.command("run wpatest pochodne %s" % seed,
                                "kod wyjscia", timeout=240)
                got = fields(out)
                if "ptk" not in got:
                    check("nasiono %s: system wypisal klucze" % seed, False)
                    continue

                pmk = hashlib.pbkdf2_hmac("sha1", got["haslo"], got["siec"],
                                          4096, 32)
                check("nasiono %s: PMK zgodny z PBKDF2 z hashlib" % seed,
                      pmk == got["pmk"])

                aa, spa = got["aa"], got["spa"]
                an, sn = got["anonce"], got["snonce"]
                data = (min(aa, spa) + max(aa, spa) +
                        min(an, sn) + max(an, sn))
                ptk = host_prf(pmk, b"Pairwise key expansion", data, 48)
                check("nasiono %s: PTK z uzgodnienia czterech ramek" % seed,
                      ptk == got["ptk"])

                mic = hmac.new(ptk[:16], got["ramka"],
                               hashlib.sha1).digest()[:16]
                check("nasiono %s: podpis wiadomosci EAPOL-Key" % seed,
                      mic == got["mic"])
                check("nasiono %s: klucze nie sa wyrodne" % seed,
                      len(set(got["ptk"])) > 8 and
                      got["ptk"][:16] != got["ptk"][16:32])
        finally:
            q.close()

        # The same sources compiled for the other mode - the cheapest
        # known way to catch a shift or a width that one compiler hid.
        disk32 = tempfile.mktemp(suffix=".img")
        with open(disk32, "wb") as fh:
            fh.truncate(DISK_SIZE)
        q = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE,
                 serial=tempfile.mktemp(suffix=".log"))
        try:
            q.wait_for("ProtonOS shell")
            q.type_line("run wpatest")
            screen = q.wait_for("[wpa] wszystko sie zgadza", timeout=240)
            check("te same wektory przechodza w trybie 32-bitowym",
                  "[wpa] wszystko sie zgadza" in screen)
        finally:
            q.close()
            os.unlink(disk32)
    finally:
        for path in (iso, slog, disk):
            if os.path.exists(path):
                os.unlink(path)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
