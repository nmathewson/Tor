#!/usr/bin/python3
#
# Copyright (c) 2001 Matej Pfajfar.
# Copyright (c) 2001-2004, Roger Dingledine.
# Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
# Copyright (c) 2007-2019, The Tor Project, Inc.
# See LICENSE for licensing information

"""
Helpful script to replace one or more C identifiers, and optionally
generate a commit message explaining what happened.
"""

import argparse
import fileinput
import os
import re
import shlex
import subprocess
import sys
import tempfile

TOPDIR = "src"


def is_c_file(fn):
    """
       Return true iff fn is the name of a C file.

       >>> is_c_file("a/b/module.c")
       True
       >>> is_c_file("a/b/module.h")
       True
       >>> is_c_file("a/b/module.c~")
       False
       >>> is_c_file("a/b/.module.c")
       False
       >>> is_c_file("a/b/module.cpp")
       False
    """
    fn = os.path.split(fn)[1]
    if fn.startswith("."):
        return False
    ext = os.path.splitext(fn)[1]
    return ext in {".c", ".h", ".i", "inc"}


def list_c_files(topdir=TOPDIR):
    """
       Use git to list all the C files under version control.

       >>> lst = list(list_c_files())
       >>> "src/core/mainloop/mainloop.c" in lst
       True
       >>> "src/core/mainloop/twiddledeedoo.c" in lst
       False
    """
    proc = subprocess.Popen(
        ["git", "ls-tree", "--name-only", "-r", "HEAD", topdir],
        stdout=subprocess.PIPE,
        encoding="utf-8")
    for line in proc.stdout.readlines():
        line = line.strip()
        if is_c_file(line):
            yield line


class Rewriter:
    """
       A rewriter applies a series of word-by-word replacements, in
       sequence.  Replacements only happen at word boundaries.

       >>> R = Rewriter([("magic", "secret"), ("words", "codes")])
       >>> R.apply("The magic words are rambunctious bluejay")
       'The secret codes are rambunctious bluejay'
       >>> R.apply("The magical words are rambunctious bluejay")
       'The magical codes are rambunctious bluejay'
       >>> R.get_count()
       3
    """

    def __init__(self, replacements):
        """Make a new Rewriter. Takes a sequence of pairs of
           (from_id, to_id), where from_id is an identifier to replace,
           and to_id is its replacement.
        """
        self._patterns = []
        for id1, id2 in replacements:
            pat = re.compile(r"\b{}\b".format(re.escape(id1)))
            self._patterns.append((pat, id2))

        self._count = 0

    def apply(self, line):
        """Return `line` as transformed by this rewriter."""
        for pat, ident in self._patterns:
            line, count = pat.subn(ident, line)
            self._count += count
        return line

    def get_count(self):
        """Return the number of identifiers that this rewriter has
           rewritten."""
        return self._count


def rewrite_files(files, rewriter):
    """
       Apply `rewriter` to every file in `files`, replacing those files
       with their rewritten contents.
    """
    for line in fileinput.input(files, inplace=True):
        sys.stdout.write(rewriter.apply(line))


def make_commit_msg(pairs, no_verify):
    """Return a commit message to explain what was replaced by the provided
       arguments.
    """
    script = ["./scripts/maint/rename_c_identifier.py"]
    for id1, id2 in pairs:
        qid1 = shlex.quote(id1)
        qid2 = shlex.quote(id2)
        script.append("        {} {}".format(qid1, qid2))
    script = " \\\n".join(script)

    if len(pairs) == 1:
        line1 = "Rename {} to {}".format(pairs[0])
    else:
        line1 = "Replace several C identifiers."

    msg = """\
{}

This is an automated commit, generated by this command:

{}
""".format(line1, script)

    if no_verify:
        msg += """
It was generated with --no-verify, so it probably breaks some commit hooks.
The commiter should be sure to fix them up in a subsequent commit.
"""

    return msg


def commit(pairs, no_verify=False):
    """Try to commit the current git state, generating the commit message as
       appropriate.  If `no_verify` is True, pass the --no-verify argument to
       git commit.
    """
    args = []
    if no_verify:
        args.append("--no-verify")

    fname = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", delete=False) as f:
            fname = f.name
            f.write(make_commit_msg(pairs, no_verify))
        s = subprocess.run(["git", "commit", "-a", "-F", fname, "--edit"]+args)
        if s.returncode != 0 and not no_verify:
            print('"git commit" failed. Maybe retry with --no-verify?',
                  file=sys.stderr)
            revert_changes()
            return False
    finally:
        os.unlink(fname)

    return True


def any_uncommitted_changes():
    """Return True if git says there are any uncommitted changes in the current
       working tree; false otherwise.
    """
    s = subprocess.run(["git", "diff-index", "--quiet", "HEAD"])
    return s.returncode != 0


DESC = "Replace one identifier with another throughout our source."
EXAMPLES = """\
Examples:

   rename_c_identifier.py set_ctrl_id set_controller_id
      (Replaces every occurrence of "set_ctrl_id" with "set_controller_id".)

   rename_c_identifier.py --commit set_ctrl_id set_controller_id
      (As above, but also generate a git commit with an appropriate message.)

   rename_c_identifier.py a b c d
      (Replace "a" with "b", and "c" with "d".)"""


def revert_changes():
    """Tell git to revert all the changes in the current working tree.
    """
    print('Reverting changes.', file=sys.stderr)
    subprocess.run(["git", "checkout", "--quiet", TOPDIR])


def main(argv):
    import argparse
    parser = argparse.ArgumentParser(description=DESC, epilog=EXAMPLES,
                                     # prevent re-wrapping the examples
                                     formatter_class=argparse.RawDescriptionHelpFormatter)

    parser.add_argument("--commit", action='store_true',
                        help="Generate a Git commit.")
    parser.add_argument("--no-verify", action='store_true',
                        help="Tell Git not to run its pre-commit hooks.")
    parser.add_argument("from_id", type=str,  help="Original identifier")
    parser.add_argument("to_id", type=str, help="New identifier")
    parser.add_argument("more", type=str, nargs=argparse.REMAINDER,
                        help="Additional identifier pairs")

    args = parser.parse_args(argv[1:])

    if len(args.more) % 2 != 0:
        print("I require an even number of identifiers.", file=sys.stderr)
        return 1

    if any_uncommitted_changes():
        print("Uncommitted changes found. Not running.", file=sys.stderr)
        return 1

    pairs = []
    print("renaming {} to {}".format(args.from_id, args.to_id))
    pairs.append((args.from_id, args.to_id))
    for idx in range(0, len(args.more), 2):
        id1 = args.more[idx]
        id2 = args.more[idx+1]
        print("renaming {} to {}".format(id1, id2))
        pairs.append((id1, id2))

    rewriter = Rewriter(pairs)

    rewrite_files(list_c_files(), rewriter)

    print("Replaced {} identifiers".format(rewriter.get_count()),
          file=sys.stderr)

    if args.commit:
        commit(pairs, args.no_verify)


if __name__ == '__main__':
    main(sys.argv)
