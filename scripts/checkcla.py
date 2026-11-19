#!/usr/bin/env python3
"""Has the author of a pull request accepted the contributor agreement?

    checkcla.py <author login> <owner login> <signatures> <CLA.md>

<signatures> is CLA-SIGNATURES.md as the pull request has it (the first
pull request is where a contributor adds their row), <CLA.md> the
agreement as the base branch has it, which says the current version.
The workflow runs this script from the base branch and only reads the
pull request's file, so a pull request cannot change the check it is
checked by. Exits 0 when the author has a row for the current version,
1 (with what to do) otherwise.

The owner holds the rights the agreement grants and signs nothing. An
app's account ("...[bot]") acts for whoever asked it; that person's
own pull requests are checked as anyone's are.
"""

import re
import sys


def current_version(cla_text):
    m = re.search(r"^Version (\d+(?:\.\d+)*)\s*$", cla_text, re.M)
    if not m:
        sys.exit("checkcla: CLA.md does not say its version")
    return m.group(1)


def signatures(text):
    """{login (lower case): set of versions}"""
    out = {}
    for line in text.splitlines():
        cols = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cols) != 4 or not line.lstrip().startswith("|"):
            continue
        login, name, date, version = cols
        if (not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})", login)
                or login.lower() == "github login"
                or not name or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date)):
            continue
        out.setdefault(login.lower(), set()).add(version)
    return out


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    author, owner, sig_path, cla_path = sys.argv[1:]
    with open(cla_path, encoding="utf-8") as fh:
        version = current_version(fh.read())

    if author.lower() == owner.lower():
        print("The owner's pull request: nothing to sign.")
        return 0
    if author.endswith("[bot]"):
        print("%s is an app's account: nothing to sign." % author)
        return 0

    try:
        with open(sig_path, encoding="utf-8") as fh:
            signed = signatures(fh.read())
    except FileNotFoundError:
        signed = {}

    versions = signed.get(author.lower(), set())
    if version in versions:
        print("%s accepted CLA %s." % (author, version))
        return 0
    if versions:
        what = ("%s accepted CLA %s, but the agreement is now at version "
                "%s. Read CLA.md and add a row for %s to CLA-SIGNATURES.md."
                % (author, ", ".join(sorted(versions)), version, version))
    else:
        what = ("%s has not accepted the contributor agreement. Read CLA.md "
                "and add this row to the table in CLA-SIGNATURES.md, in this "
                "pull request:  | %s | Your Full Name | YYYY-MM-DD | %s |"
                % (author, author, version))
    print("::error title=Contributor agreement::" + what)
    return 1


if __name__ == "__main__":
    sys.exit(main())
