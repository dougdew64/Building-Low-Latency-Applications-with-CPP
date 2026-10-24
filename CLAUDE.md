# CLAUDE.md

**Purpose:** how Claude works in this fork. **Status:** procedure. **Read when:** always, first.

This fork is the back end of [Fillscope](https://github.com/dougdew64/fillscope), and Fillscope's
own `CLAUDE.md` applies here too: the repository is public, attribution is mechanical (every
commit Claude makes carries a `Co-Authored-By: Claude` trailer; the owner's carry none), and what
Claude claims works is built and run first. Fillscope's `docs/PLAN.md` says what is being done
here, and in what order.

Here, in addition:
- Work on the `fillscope` branch, in `Chapter12/`. `main` follows upstream; nothing else changes it.
- Upstream's code changes only where Fillscope needs it or a bug is fixed; each change is listed in
  the README's section on this fork.
- Test what Fillscope relies on and every change; a bug found in the book's code gets a test that
  fails before its fix.
- In every commit it makes, Claude first regenerates `ATTRIBUTION.md`, the count of each file's
  lines by upstream, the owner and Claude: `python3 -I tools/attribution/attribution.py --report
  ATTRIBUTION.md --upstream 11e3925f431f7c8c39739ed90b874d6ff2aa2966` (upstream's last commit before
  the fork). The script is a copy of Fillscope's (`tools/attribution/`), which Fillscope's tests keep
  identical: change it there.
